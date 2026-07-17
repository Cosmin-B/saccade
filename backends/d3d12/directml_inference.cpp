#include "backends/d3d12/directml_inference.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <DirectML.h>
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <dml_provider_factory.h>
#include <onnxruntime_c_api.h>
#include <onnxruntime_run_options_config_keys.h>
#include <onnxruntime_session_options_config_keys.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>

namespace saccade::backend::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t runtime_path_capacity = 32 * 1024;

HMODULE load_adjacent_runtime(const wchar_t* name) noexcept {
    std::array<wchar_t, runtime_path_capacity> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return nullptr;
    size_t separator = length;
    while (separator != 0 && path[separator - 1U] != L'\\')
        --separator;
    const size_t name_length = std::wcslen(name);
    if (separator == 0 || separator + name_length + 1U > path.size()) {
        return nullptr;
    }
    std::memcpy(path.data() + separator, name, (name_length + 1U) * sizeof(wchar_t));
    return LoadLibraryExW(path.data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

size_t element_size(TensorElementType type) noexcept {
    switch (type) {
    case TensorElementType::fp32:
        return 4;
    case TensorElementType::fp16:
        return 2;
    case TensorElementType::int8:
        return 1;
    case TensorElementType::uint8:
        return 1;
    case TensorElementType::int32:
        return 4;
    case TensorElementType::int64:
        return 8;
    }
    return 0;
}

ONNXTensorElementDataType ort_element_type(TensorElementType type) noexcept {
    switch (type) {
    case TensorElementType::fp32:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case TensorElementType::fp16:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    case TensorElementType::int8:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
    case TensorElementType::uint8:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    case TensorElementType::int32:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    case TensorElementType::int64:
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    }
    return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}

bool binding_valid(const DirectMlBindingDesc& binding) noexcept {
    if (binding.name == nullptr || binding.name[0] == '\0' || binding.resource == nullptr || binding.byte_size == 0 ||
        binding.rank == 0 || binding.rank > directml_shape_capacity) {
        return false;
    }
    const size_t bytes = element_size(binding.element_type);
    if (bytes == 0) return false;
    uint64_t elements = 1;
    for (uint32_t index = 0; index < binding.rank; ++index) {
        if (binding.shape[index] <= 0 ||
            elements > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(binding.shape[index])) {
            return false;
        }
        elements *= static_cast<uint64_t>(binding.shape[index]);
    }
    const D3D12_RESOURCE_DESC resource = binding.resource->GetDesc();
    return elements <= binding.byte_size / bytes && resource.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
           resource.Width >= binding.byte_size && (resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}

} // namespace

struct DirectMlInference::Impl {
    struct Binding {
        OrtValue* value_ = nullptr;
        void* allocation_ = nullptr;
    };

    const OrtApi* api_ = nullptr;
    const OrtDmlApi* dml_api_ = nullptr;
    OrtEnv* environment_ = nullptr;
    OrtSessionOptions* session_options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtRunOptions* run_options_ = nullptr;
    OrtMemoryInfo* memory_info_ = nullptr;
    OrtIoBinding* io_binding_ = nullptr;
    HMODULE runtime_module_ = nullptr;
    HMODULE directml_module_ = nullptr;
    ComPtr<IDMLDevice> dml_device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<ID3D12Fence> completion_fence_{};
    std::array<Binding, directml_binding_capacity> inputs_{};
    std::array<Binding, directml_binding_capacity> outputs_{};
    DirectMlStats stats_{};
    HANDLE completion_event_ = nullptr;
    uint64_t next_completion_ = 1;
    DWORD owner_thread_ = 0;

    [[nodiscard]] bool owns_thread() const noexcept { return owner_thread_ == GetCurrentThreadId(); }

    SaccadeResult status(OrtStatus* value) noexcept {
        if (value == nullptr) return SACCADE_OK;
        stats_.last_native_code = static_cast<int32_t>(api_->GetErrorCode(value));
        api_->ReleaseStatus(value);
        ++stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }

    void release_binding(Binding& binding) noexcept {
        if (binding.value_ != nullptr) {
            api_->ReleaseValue(binding.value_);
            binding.value_ = nullptr;
        }
        if (binding.allocation_ != nullptr) {
            OrtStatus* released = dml_api_->FreeGPUAllocation(binding.allocation_);
            if (released != nullptr) api_->ReleaseStatus(released);
            binding.allocation_ = nullptr;
        }
    }

    void release() noexcept {
        if (api_ == nullptr) return;
        if (io_binding_ != nullptr) {
            api_->ReleaseIoBinding(io_binding_);
            io_binding_ = nullptr;
        }
        for (Binding& binding : outputs_)
            release_binding(binding);
        for (Binding& binding : inputs_)
            release_binding(binding);
        if (memory_info_ != nullptr) {
            api_->ReleaseMemoryInfo(memory_info_);
            memory_info_ = nullptr;
        }
        if (session_ != nullptr) {
            api_->ReleaseSession(session_);
            session_ = nullptr;
        }
        if (run_options_ != nullptr) {
            api_->ReleaseRunOptions(run_options_);
            run_options_ = nullptr;
        }
        if (session_options_ != nullptr) {
            api_->ReleaseSessionOptions(session_options_);
            session_options_ = nullptr;
        }
        if (environment_ != nullptr) {
            api_->ReleaseEnv(environment_);
            environment_ = nullptr;
        }
        if (completion_event_ != nullptr) {
            (void)CloseHandle(completion_event_);
            completion_event_ = nullptr;
        }
        completion_fence_.Reset();
        queue_.Reset();
        dml_device_.Reset();
        if (runtime_module_ != nullptr) {
            (void)FreeLibrary(runtime_module_);
            runtime_module_ = nullptr;
        }
        if (directml_module_ != nullptr) {
            (void)FreeLibrary(directml_module_);
            directml_module_ = nullptr;
        }
    }

    SaccadeResult bind(const DirectMlBindingDesc& desc, Binding& binding, bool input) noexcept {
        SaccadeResult result =
            status(dml_api_->CreateGPUAllocationFromD3DResource(desc.resource, &binding.allocation_));
        if (result != SACCADE_OK) return result;
        result = status(api_->CreateTensorWithDataAsOrtValue(memory_info_, binding.allocation_, desc.byte_size,
                                                             desc.shape.data(), desc.rank,
                                                             ort_element_type(desc.element_type), &binding.value_));
        if (result != SACCADE_OK) return result;
        return status(input ? api_->BindInput(io_binding_, desc.name, binding.value_)
                            : api_->BindOutput(io_binding_, desc.name, binding.value_));
    }
};

static_assert(sizeof(DirectMlInference::Impl) <= DirectMlInference::storage_size);
static_assert(alignof(DirectMlInference::Impl) <= 64);

DirectMlInference::DirectMlInference() noexcept {
    new (storage_.data()) Impl{};
}

DirectMlInference::~DirectMlInference() {
    Impl& state = impl();
    if (state.owns_thread()) state.release();
    state.~Impl();
}

DirectMlInference::Impl& DirectMlInference::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const DirectMlInference::Impl& DirectMlInference::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult DirectMlInference::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                            const DirectMlSessionDesc& desc) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (device == nullptr || queue == nullptr || desc.model.data == nullptr || desc.model.size == 0 ||
        desc.inputs == nullptr || desc.outputs == nullptr || desc.input_count == 0 ||
        desc.input_count > directml_binding_capacity || desc.output_count == 0 ||
        desc.output_count > directml_binding_capacity) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < desc.input_count; ++index) {
        if (!binding_valid(desc.inputs[index])) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (uint32_t index = 0; index < desc.output_count; ++index) {
        if (!binding_valid(desc.outputs[index])) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }
    Impl& state = impl();
    state.owner_thread_ = GetCurrentThreadId();
    state.stats_.model_bytes = desc.model.size;
    state.stats_.input_count = desc.input_count;
    state.stats_.output_count = desc.output_count;
    state.runtime_module_ = load_adjacent_runtime(L"onnxruntime.dll");
    if (state.runtime_module_ == nullptr) {
        state.stats_.last_native_code = static_cast<int32_t>(GetLastError());
        ++state.stats_.failures;
        return SACCADE_ERROR_NOT_FOUND;
    }
    using GetApiBaseFn = const OrtApiBase*(__cdecl*)() noexcept;
    const auto get_api_base = reinterpret_cast<GetApiBaseFn>(GetProcAddress(state.runtime_module_, "OrtGetApiBase"));
    const OrtApiBase* api_base = get_api_base == nullptr ? nullptr : get_api_base();
    state.api_ = api_base == nullptr ? nullptr : api_base->GetApi(ORT_API_VERSION);
    if (state.api_ == nullptr) return SACCADE_ERROR_BACKEND;
    SaccadeResult result = state.status(
        state.api_->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&state.dml_api_)));
    if (result != SACCADE_OK) return result;
    state.directml_module_ = load_adjacent_runtime(L"DirectML.dll");
    using CreateDmlDeviceFn = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    const auto create_dml_device =
        state.directml_module_ == nullptr
            ? nullptr
            : reinterpret_cast<CreateDmlDeviceFn>(GetProcAddress(state.directml_module_, "DMLCreateDevice"));
    const HRESULT dml_result =
        create_dml_device == nullptr
            ? HRESULT_FROM_WIN32(GetLastError())
            : create_dml_device(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(state.dml_device_.GetAddressOf()));
    if (FAILED(dml_result)) {
        state.stats_.last_native_code = dml_result;
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    result = state.status(state.api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "saccade", &state.environment_));
    if (result == SACCADE_OK) {
        result = state.status(state.api_->DisableTelemetryEvents(state.environment_));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->CreateSessionOptions(&state.session_options_));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->CreateRunOptions(&state.run_options_));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->AddRunConfigEntry(
            state.run_options_, kOrtRunOptionsConfigDisableSynchronizeExecutionProviders, "1"));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->SetSessionExecutionMode(state.session_options_, ORT_SEQUENTIAL));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->DisableMemPattern(state.session_options_));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->SetIntraOpNumThreads(state.session_options_, 1));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->SetInterOpNumThreads(state.session_options_, 1));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->SetSessionGraphOptimizationLevel(state.session_options_, ORT_ENABLE_ALL));
    }
    if (result == SACCADE_OK) {
        result = state.status(
            state.api_->AddSessionConfigEntry(state.session_options_, kOrtSessionOptionsDisableCPUEPFallback, "1"));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.dml_api_->SessionOptionsAppendExecutionProvider_DML1(
            state.session_options_, state.dml_device_.Get(), queue));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->CreateSessionFromArray(state.environment_, desc.model.data, desc.model.size,
                                                                 state.session_options_, &state.session_));
    }
    if (result == SACCADE_OK) {
        result = state.status(
            state.api_->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &state.memory_info_));
    }
    if (result == SACCADE_OK) {
        result = state.status(state.api_->CreateIoBinding(state.session_, &state.io_binding_));
    }
    for (uint32_t index = 0; result == SACCADE_OK && index < desc.input_count; ++index) {
        result = state.bind(desc.inputs[index], state.inputs_[index], true);
        state.stats_.input_bytes += desc.inputs[index].byte_size;
    }
    for (uint32_t index = 0; result == SACCADE_OK && index < desc.output_count; ++index) {
        result = state.bind(desc.outputs[index], state.outputs_[index], false);
        state.stats_.output_bytes += desc.outputs[index].byte_size;
    }
    if (result == SACCADE_OK) {
        state.queue_ = queue;
        const HRESULT fence_result =
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state.completion_fence_.GetAddressOf()));
        if (FAILED(fence_result)) {
            state.stats_.last_native_code = fence_result;
            ++state.stats_.failures;
            result = SACCADE_ERROR_BACKEND;
        }
    }
    if (result == SACCADE_OK) {
        state.completion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (state.completion_event_ == nullptr) {
            state.stats_.last_native_code = static_cast<int32_t>(GetLastError());
            ++state.stats_.failures;
            result = SACCADE_ERROR_BACKEND;
        }
    }
    if (result != SACCADE_OK) {
        state.release();
        return result;
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DirectMlInference::run() noexcept {
    if (!initialized_ || !impl().owns_thread()) return SACCADE_ERROR_STATE;
    const SaccadeResult result =
        impl().status(impl().api_->RunWithBinding(impl().session_, impl().run_options_, impl().io_binding_));
    if (result == SACCADE_OK) ++impl().stats_.runs;
    return result;
}

SaccadeResult DirectMlInference::synchronize_outputs() noexcept {
    if (!initialized_ || !impl().owns_thread()) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    const uint64_t completion = state.next_completion_++;
    HRESULT result = state.queue_->Signal(state.completion_fence_.Get(), completion);
    if (FAILED(result)) {
        state.stats_.last_native_code = result;
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    const uint64_t completed = state.completion_fence_->GetCompletedValue();
    if (completed == UINT64_MAX) {
        state.stats_.last_native_code = E_FAIL;
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    if (completed >= completion) return SACCADE_OK;
    result = state.completion_fence_->SetEventOnCompletion(completion, state.completion_event_);
    if (FAILED(result)) {
        state.stats_.last_native_code = result;
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    if (WaitForSingleObject(state.completion_event_, INFINITE) == WAIT_OBJECT_0) return SACCADE_OK;
    state.stats_.last_native_code = static_cast<int32_t>(GetLastError());
    ++state.stats_.failures;
    return SACCADE_ERROR_BACKEND;
}

SaccadeResult DirectMlInference::adopt_current_thread() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    impl().owner_thread_ = GetCurrentThreadId();
    return SACCADE_OK;
}

DirectMlStats DirectMlInference::stats() const noexcept {
    return impl().stats_;
}

} // namespace saccade::backend::d3d12

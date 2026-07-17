#include "backends/d3d12/preprocessor.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace saccade::backend::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t thread_group_width = 16;
constexpr uint32_t thread_group_height = 16;
constexpr size_t maximum_shader_bytes = 64 * 1024;

struct alignas(16) PreprocessParameters {
    std::array<float, 4> source_rect{};
    std::array<float, 4> content_rect{};
    std::array<float, 4> channel_scale{};
    std::array<float, 4> channel_bias{};
    std::array<float, 4> letterbox_rgb{};
    std::array<uint32_t, 2> output_size{};
    std::array<uint32_t, 2> reserved{};
};

static_assert(sizeof(PreprocessParameters) == 96);

bool tensor_spec_valid(const TensorSpec& spec) noexcept {
    return spec.width != 0 && spec.height != 0 &&
           (spec.format == TensorFormat::planar_fp16 || spec.format == TensorFormat::planar_int8) && spec.reserved == 0;
}

bool load_shader(const char* directory, const char* name, std::array<std::byte, maximum_shader_bytes>* bytes,
                 size_t* byte_count) noexcept {
    if (directory == nullptr || name == nullptr || bytes == nullptr || byte_count == nullptr) return false;
    std::array<char, MAX_PATH> path{};
    const size_t directory_length = std::strlen(directory);
    const size_t name_length = std::strlen(name);
    if (directory_length + name_length + 2U > path.size()) return false;
    std::memcpy(path.data(), directory, directory_length);
    size_t position = directory_length;
    if (position != 0 && path[position - 1U] != '\\' && path[position - 1U] != '/') {
        path[position++] = '\\';
    }
    std::memcpy(path.data() + position, name, name_length + 1U);
    const HANDLE file =
        CreateFileA(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool loaded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 &&
                        size.QuadPart <= static_cast<LONGLONG>(bytes->size()) &&
                        ReadFile(file, bytes->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) != FALSE &&
                        read == static_cast<DWORD>(size.QuadPart);
    (void)CloseHandle(file);
    if (!loaded) return false;
    *byte_count = read;
    return true;
}

uint64_t timeout_milliseconds(uint64_t timeout_ns) noexcept {
    constexpr uint64_t nanoseconds_per_millisecond = 1'000'000;
    if (timeout_ns == 0) return 0;
    const uint64_t rounded =
        timeout_ns / nanoseconds_per_millisecond + (timeout_ns % nanoseconds_per_millisecond != 0 ? 1U : 0U);
    return std::min<uint64_t>(rounded, INFINITE - 1U);
}

} // namespace

struct ImagePreprocessor::Impl {
    DWORD owner_thread_ = 0;
    TensorSpec spec_{};
    PreprocessorStats stats_{};
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<ID3D12CommandAllocator> allocator_{};
    ComPtr<ID3D12GraphicsCommandList> commands_{};
    ComPtr<ID3D12RootSignature> root_signature_{};
    ComPtr<ID3D12PipelineState> pipeline_{};
    ComPtr<ID3D12DescriptorHeap> descriptors_{};
    ComPtr<ID3D12Resource> output_{};
    ComPtr<ID3D12Fence> fence_{};
    HANDLE completion_event_ = nullptr;
    uint64_t sequence_ = 0;
    uint64_t completed_sequence_ = 0;
    uint64_t frame_id_ = 0;
    uint64_t transform_epoch_ = 0;
    size_t output_bytes_ = 0;
    size_t plane_stride_bytes_ = 0;
    UINT descriptor_size_ = 0;
    bool fence_failed_ = false;

    [[nodiscard]] bool owns_thread() const noexcept { return owner_thread_ == GetCurrentThreadId(); }

    SaccadeResult completion_status() noexcept {
        if (fence_failed_) return SACCADE_ERROR_BACKEND;
        if (sequence_ == completed_sequence_) return SACCADE_OK;
        const uint64_t completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) {
            fence_failed_ = true;
            ++stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
        if (completed < sequence_) return SACCADE_ERROR_BUSY;
        completed_sequence_ = sequence_;
        ++stats_.completions;
        return SACCADE_OK;
    }

    [[nodiscard]] bool submission_matches(const PreprocessSubmission* submission) const noexcept {
        return submission != nullptr && submission->sequence == sequence_ && submission->frame_id == frame_id_ &&
               submission->transform_epoch == transform_epoch_;
    }

    [[nodiscard]] PreprocessParameters parameters(SourceRegion region) const noexcept {
        const float output_width = static_cast<float>(spec_.width);
        const float output_height = static_cast<float>(spec_.height);
        const float region_width = static_cast<float>(region.width);
        const float region_height = static_cast<float>(region.height);
        const float scale = std::min(output_width / region_width, output_height / region_height);
        const float content_width = std::max(1.0F, std::floor(region_width * scale));
        const float content_height = std::max(1.0F, std::floor(region_height * scale));
        PreprocessParameters result{};
        result.source_rect = {static_cast<float>(region.x), static_cast<float>(region.y), region_width, region_height};
        result.content_rect = {std::floor((output_width - content_width) * 0.5F),
                               std::floor((output_height - content_height) * 0.5F), content_width, content_height};
        result.channel_scale = {spec_.channel_scale[0], spec_.channel_scale[1], spec_.channel_scale[2], 0.0F};
        result.channel_bias = {spec_.channel_bias[0], spec_.channel_bias[1], spec_.channel_bias[2], 0.0F};
        result.letterbox_rgb = {spec_.letterbox_rgb[0], spec_.letterbox_rgb[1], spec_.letterbox_rgb[2], 0.0F};
        result.output_size = {spec_.width, spec_.height};
        return result;
    }
};

static_assert(sizeof(ImagePreprocessor::Impl) <= ImagePreprocessor::storage_size);
static_assert(alignof(ImagePreprocessor::Impl) <= 64);

ImagePreprocessor::ImagePreprocessor() noexcept {
    new (storage_.data()) Impl{};
}

ImagePreprocessor::~ImagePreprocessor() {
    Impl& state = impl();
    if (initialized_ && state.owns_thread() && state.completion_status() == SACCADE_ERROR_BUSY) {
        const PreprocessSubmission submission{state.sequence_, state.frame_id_, state.transform_epoch_};
        (void)wait(&submission, UINT64_C(1'000'000'000));
    }
    if (state.completion_event_ != nullptr) {
        (void)CloseHandle(state.completion_event_);
    }
    state.~Impl();
}

ImagePreprocessor::Impl& ImagePreprocessor::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const ImagePreprocessor::Impl& ImagePreprocessor::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult ImagePreprocessor::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                            const char* shader_directory, const TensorSpec& spec) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (device == nullptr || queue == nullptr || shader_directory == nullptr || !tensor_spec_valid(spec))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    state.owner_thread_ = GetCurrentThreadId();
    state.device_ = device;
    state.queue_ = queue;
    state.spec_ = spec;

    const size_t element_bytes = spec.format == TensorFormat::planar_fp16 ? 2U : 1U;
    const uint64_t plane_elements = static_cast<uint64_t>(spec.width) * spec.height;
    const uint64_t output_elements = plane_elements * 3U;
    const uint64_t output_bytes = output_elements * element_bytes;
    if (output_elements > UINT32_MAX || output_bytes > SIZE_MAX) {
        return SACCADE_ERROR_CAPACITY;
    }
    state.plane_stride_bytes_ = static_cast<size_t>(plane_elements * element_bytes);
    state.output_bytes_ = static_cast<size_t>(output_bytes);
    state.stats_.output_bytes = output_bytes;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER root_parameters[2]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    root_parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.Num32BitValues = sizeof(PreprocessParameters) / sizeof(uint32_t);
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = root_parameters;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, root_blob.GetAddressOf(),
                                           root_error.GetAddressOf())) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(state.root_signature_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }

    std::array<std::byte, maximum_shader_bytes> shader{};
    size_t shader_bytes = 0;
    const char* shader_name =
        spec.format == TensorFormat::planar_fp16 ? "preprocess_fp16.dxil" : "preprocess_int8.dxil";
    if (!load_shader(shader_directory, shader_name, &shader, &shader_bytes)) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = state.root_signature_.Get();
    pipeline_desc.CS = {shader.data(), shader_bytes};
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(state.pipeline_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(state.descriptors_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    state.descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC output_desc{};
    output_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    output_desc.Width = output_bytes;
    output_desc.Height = 1;
    output_desc.DepthOrArraySize = 1;
    output_desc.MipLevels = 1;
    output_desc.SampleDesc.Count = 1;
    output_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &output_desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(state.output_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_view{};
    output_view.Format = spec.format == TensorFormat::planar_fp16 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R8_SINT;
    output_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    output_view.Buffer.NumElements = static_cast<UINT>(output_elements);
    D3D12_CPU_DESCRIPTOR_HANDLE output_handle = state.descriptors_->GetCPUDescriptorHandleForHeapStart();
    output_handle.ptr += state.descriptor_size_;
    device->CreateUnorderedAccessView(state.output_.Get(), nullptr, &output_view, output_handle);

    state.completion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state.completion_event_ == nullptr ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(state.allocator_.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.allocator_.Get(),
                                         state.pipeline_.Get(), IID_PPV_ARGS(state.commands_.GetAddressOf()))) ||
        FAILED(state.commands_->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state.fence_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::submit(ID3D12Resource* source, uint32_t source_width, uint32_t source_height,
                                        SourceRegion region, uint64_t frame_id, uint64_t transform_epoch,
                                        PreprocessSubmission* output) noexcept {
    if (!initialized_ || source == nullptr || output == nullptr || frame_id == 0 || transform_epoch == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    const SaccadeResult completion = state.completion_status();
    if (completion == SACCADE_ERROR_BUSY) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    if (completion != SACCADE_OK) return completion;
    if (state.sequence_ == std::numeric_limits<uint64_t>::max()) {
        return SACCADE_ERROR_CAPACITY;
    }
    const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source_desc.Width < source_width ||
        source_desc.Height < source_height || source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (region.width == 0 || region.height == 0) {
        region = {0, 0, source_width, source_height};
    }
    if (static_cast<uint64_t>(region.x) + region.width > source_width ||
        static_cast<uint64_t>(region.y) + region.height > source_height) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};
    source_view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    source_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_view.Texture2D.MipLevels = 1;
    state.device_->CreateShaderResourceView(source, &source_view,
                                            state.descriptors_->GetCPUDescriptorHandleForHeapStart());
    if (FAILED(state.allocator_->Reset()) ||
        FAILED(state.commands_->Reset(state.allocator_.Get(), state.pipeline_.Get()))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    D3D12_RESOURCE_BARRIER output_to_write{};
    output_to_write.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_to_write.Transition.pResource = state.output_.Get();
    output_to_write.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_to_write.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    output_to_write.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    state.commands_->ResourceBarrier(1, &output_to_write);
    D3D12_RESOURCE_BARRIER source_to_read{};
    source_to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    source_to_read.Transition.pResource = source;
    source_to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    source_to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    source_to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    state.commands_->ResourceBarrier(1, &source_to_read);
    ID3D12DescriptorHeap* heaps[]{state.descriptors_.Get()};
    state.commands_->SetDescriptorHeaps(1, heaps);
    state.commands_->SetComputeRootSignature(state.root_signature_.Get());
    state.commands_->SetComputeRootDescriptorTable(0, state.descriptors_->GetGPUDescriptorHandleForHeapStart());
    const PreprocessParameters parameters = state.parameters(region);
    state.commands_->SetComputeRoot32BitConstants(1, sizeof(parameters) / sizeof(uint32_t), &parameters, 0);
    state.commands_->Dispatch((state.spec_.width + thread_group_width - 1U) / thread_group_width,
                              (state.spec_.height + thread_group_height - 1U) / thread_group_height, 1);
    std::swap(output_to_write.Transition.StateBefore, output_to_write.Transition.StateAfter);
    state.commands_->ResourceBarrier(1, &output_to_write);
    std::swap(source_to_read.Transition.StateBefore, source_to_read.Transition.StateAfter);
    state.commands_->ResourceBarrier(1, &source_to_read);
    if (FAILED(state.commands_->Close())) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    ID3D12CommandList* command_lists[]{state.commands_.Get()};
    state.queue_->ExecuteCommandLists(1, command_lists);
    const uint64_t sequence = state.sequence_ + 1U;
    if (FAILED(state.queue_->Signal(state.fence_.Get(), sequence))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    state.sequence_ = sequence;
    state.frame_id_ = frame_id;
    state.transform_epoch_ = transform_epoch;
    ++state.stats_.submissions;
    *output = {sequence, frame_id, transform_epoch};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::adopt_current_thread() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    const SaccadeResult completion = state.completion_status();
    if (completion != SACCADE_OK) return completion;
    state.owner_thread_ = GetCurrentThreadId();
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::poll(const PreprocessSubmission* submission) noexcept {
    if (!initialized_ || !impl().owns_thread() || !impl().submission_matches(submission)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return impl().completion_status();
}

SaccadeResult ImagePreprocessor::wait(const PreprocessSubmission* submission, uint64_t timeout_ns) noexcept {
    if (!initialized_ || !impl().owns_thread() || !impl().submission_matches(submission)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    SaccadeResult completion = state.completion_status();
    if (completion != SACCADE_ERROR_BUSY) return completion;
    if (FAILED(state.fence_->SetEventOnCompletion(submission->sequence, state.completion_event_))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    const DWORD waited =
        WaitForSingleObject(state.completion_event_, static_cast<DWORD>(timeout_milliseconds(timeout_ns)));
    if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_BUSY;
    if (waited != WAIT_OBJECT_0) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    completion = state.completion_status();
    return completion == SACCADE_ERROR_BUSY ? SACCADE_ERROR_BACKEND : completion;
}

SaccadeResult ImagePreprocessor::tensor(const PreprocessSubmission* submission, TensorView* output) noexcept {
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult ready = poll(submission);
    if (ready != SACCADE_OK) return ready;
    const Impl& state = impl();
    *output = {state.output_.Get(),
               state.output_bytes_,
               state.plane_stride_bytes_,
               state.spec_.width,
               state.spec_.height,
               state.spec_.format,
               3};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::tensor_storage(TensorView* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl& state = impl();
    *output = {state.output_.Get(),
               state.output_bytes_,
               state.plane_stride_bytes_,
               state.spec_.width,
               state.spec_.height,
               state.spec_.format,
               3};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::completion_dependency(const PreprocessSubmission* submission, ID3D12Fence** fence,
                                                       uint64_t* value) noexcept {
    if (!initialized_ || !impl().owns_thread() || !impl().submission_matches(submission) || fence == nullptr ||
        value == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *fence = impl().fence_.Get();
    *value = submission->sequence;
    return SACCADE_OK;
}

PreprocessorStats ImagePreprocessor::stats() const noexcept {
    return impl().stats_;
}

} // namespace saccade::backend::d3d12

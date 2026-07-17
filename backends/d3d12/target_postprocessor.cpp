#include "backends/d3d12/target_postprocessor.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace saccade::backend::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t radix_passes = 16;
constexpr uint32_t radix_bins = 16;
constexpr uint32_t block_size = 256;
constexpr size_t parameter_stride = 256;
constexpr size_t maximum_shader_bytes = 64 * 1024;

enum Pipeline : size_t {
    pack_normalized,
    prepare,
    histogram,
    scan,
    scatter,
    suppression_masks,
    finalize,
    pipeline_count
};

constexpr std::array<const char*, pipeline_count> shader_names{
    "targets_pack_normalized.dxil", "targets_prepare.dxil",       "targets_radix_histogram.dxil",
    "targets_radix_scan.dxil",      "targets_radix_scatter.dxil", "targets_suppression_masks.dxil",
    "targets_finalize.dxil"};

struct alignas(16) Parameters {
    uint32_t candidate_count;
    uint32_t maximum_targets;
    uint32_t block_count;
    uint32_t mask_word_count;
    uint32_t minimum_confidence_q16;
    uint32_t band_minimum_confidence_q16;
    uint32_t band_min_short_side_q3;
    uint32_t band_max_short_side_q3;
    uint32_t iou_threshold_q16;
    uint32_t coordinate_space;
    uint32_t radix_shift;
    uint32_t model_width;
    uint32_t model_height;
    uint32_t source_width;
    uint32_t source_height;
    uint64_t frame_id;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
};

struct RadixEntry {
    uint64_t key;
    uint32_t candidate_index;
    uint32_t reserved;
};

static_assert(sizeof(Parameters) == 112);
static_assert(sizeof(RadixEntry) == 16);

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

bool config_valid(const kernels::targets::PostprocessConfig& config, const kernels::targets::PostprocessEpochs& epochs,
                  uint32_t target_capacity) noexcept {
    return config.maximum_targets != 0 && config.maximum_targets <= target_capacity &&
           (config.coordinate_space == SACCADE_COORDINATE_SPACE_MODEL_Q8 ||
            config.coordinate_space == SACCADE_COORDINATE_SPACE_SOURCE_Q8) &&
           kernels::targets::confidence_band_valid(config) && config.reserved == 0 && epochs.frame_id != 0 &&
           epochs.model_epoch != 0 && epochs.session_epoch != 0 && epochs.transform_epoch != 0 &&
           epochs.topology_epoch != 0 && epochs.source_id != 0;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition.pResource = resource;
    value.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    value.Transition.StateBefore = before;
    value.Transition.StateAfter = after;
    return value;
}

} // namespace

struct TargetPostprocessor::Impl {
    DWORD owner_thread_ = 0;
    TargetPostprocessorSpec spec_{};
    TargetPostprocessorStats stats_{};
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<ID3D12CommandAllocator> allocator_{};
    ComPtr<ID3D12GraphicsCommandList> commands_{};
    ComPtr<ID3D12RootSignature> root_signature_{};
    std::array<ComPtr<ID3D12PipelineState>, pipeline_count> pipelines_{};
    ComPtr<ID3D12Resource> parameters_{};
    ComPtr<ID3D12Resource> entries_a_{};
    ComPtr<ID3D12Resource> entries_b_{};
    ComPtr<ID3D12Resource> block_offsets_{};
    ComPtr<ID3D12Resource> local_ranks_{};
    ComPtr<ID3D12Resource> masks_{};
    ComPtr<ID3D12Resource> suppressed_{};
    ComPtr<ID3D12Resource> packet_{};
    ComPtr<ID3D12Resource> counters_{};
    ComPtr<ID3D12Resource> packed_candidates_{};
    ComPtr<ID3D12Resource> readback_{};
    ComPtr<ID3D12Fence> fence_{};
    HANDLE completion_event_ = nullptr;
    uint8_t* parameter_bytes_ = nullptr;
    const uint8_t* readback_bytes_ = nullptr;
    size_t packet_capacity_ = 0;
    uint64_t sequence_ = 0;
    uint64_t counted_sequence_ = 0;
    uint64_t frame_id_ = 0;
    bool fence_failed_ = false;

    [[nodiscard]] bool owns_thread() const noexcept { return owner_thread_ == GetCurrentThreadId(); }

    SaccadeResult completion_status() noexcept {
        if (fence_failed_) return SACCADE_ERROR_BACKEND;
        if (sequence_ == counted_sequence_) return SACCADE_OK;
        const uint64_t completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) {
            fence_failed_ = true;
            ++stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
        if (completed < sequence_) return SACCADE_ERROR_BUSY;
        counted_sequence_ = sequence_;
        ++stats_.completed;
        return SACCADE_OK;
    }

    [[nodiscard]] bool matches(const TargetPostprocessSubmission& submission) const noexcept {
        return submission.sequence != 0 && submission.sequence == sequence_ && submission.frame_id == frame_id_;
    }

    void bind_resources(uint32_t pass, bool model_rows = false) noexcept {
        commands_->SetComputeRootConstantBufferView(0, parameters_->GetGPUVirtualAddress() +
                                                           static_cast<uint64_t>(pass) * parameter_stride);
        commands_->SetComputeRootShaderResourceView(1, model_rows || packed_candidates_ == nullptr
                                                           ? spec_.candidate_buffer->GetGPUVirtualAddress() +
                                                                 spec_.candidate_offset
                                                           : packed_candidates_->GetGPUVirtualAddress());
        const std::array<ID3D12Resource*, 8> resources{entries_a_.Get(),   entries_b_.Get(), block_offsets_.Get(),
                                                       local_ranks_.Get(), masks_.Get(),     suppressed_.Get(),
                                                       packet_.Get(),      counters_.Get()};
        for (size_t index = 0; index < resources.size(); ++index) {
            commands_->SetComputeRootUnorderedAccessView(static_cast<UINT>(index + 2U),
                                                         resources[index]->GetGPUVirtualAddress());
        }
        if (packed_candidates_ != nullptr) {
            commands_->SetComputeRootUnorderedAccessView(10, packed_candidates_->GetGPUVirtualAddress());
        }
    }
};

static_assert(sizeof(TargetPostprocessor::Impl) <= TargetPostprocessor::storage_size);
static_assert(alignof(TargetPostprocessor::Impl) <= 64);

TargetPostprocessor::TargetPostprocessor() noexcept {
    new (storage_.data()) Impl{};
}

TargetPostprocessor::~TargetPostprocessor() {
    Impl& state = impl();
    if (initialized_ && state.owns_thread() && state.completion_status() == SACCADE_ERROR_BUSY) {
        const TargetPostprocessSubmission submission{state.sequence_, state.frame_id_};
        (void)wait(submission, UINT64_C(1'000'000'000));
    }
    if (state.parameters_ != nullptr && state.parameter_bytes_ != nullptr) {
        state.parameters_->Unmap(0, nullptr);
    }
    if (state.readback_ != nullptr && state.readback_bytes_ != nullptr) {
        state.readback_->Unmap(0, nullptr);
    }
    if (state.completion_event_ != nullptr) {
        (void)CloseHandle(state.completion_event_);
    }
    state.~Impl();
}

TargetPostprocessor::Impl& TargetPostprocessor::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const TargetPostprocessor::Impl& TargetPostprocessor::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult TargetPostprocessor::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                              const char* shader_directory,
                                              const TargetPostprocessorSpec& spec) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (device == nullptr || queue == nullptr || shader_directory == nullptr || spec.candidate_buffer == nullptr ||
        spec.candidate_capacity == 0 || spec.candidate_capacity > kernels::targets::maximum_candidates ||
        spec.target_capacity == 0 || spec.target_capacity > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        spec.target_capacity > spec.candidate_capacity ||
        spec.candidate_offset > spec.candidate_buffer->GetDesc().Width || spec.reserved != 0 ||
        (spec.candidate_input != CandidateInput::packed_q3 &&
         spec.candidate_input != CandidateInput::normalized_fp16) ||
        (spec.candidate_input == CandidateInput::normalized_fp16 &&
         (spec.model_width == 0 || spec.model_height == 0))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t candidate_stride =
        spec.candidate_input == CandidateInput::normalized_fp16 ? 12U : sizeof(kernels::targets::DenseCandidate);
    if (static_cast<uint64_t>(spec.candidate_capacity) * candidate_stride >
        spec.candidate_buffer->GetDesc().Width - spec.candidate_offset) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    state.owner_thread_ = GetCurrentThreadId();
    state.device_ = device;
    state.queue_ = queue;
    state.spec_ = spec;
    state.stats_.candidate_capacity = spec.candidate_capacity;
    state.stats_.target_capacity = spec.target_capacity;
    state.stats_.radix_passes = radix_passes;

    std::array<D3D12_ROOT_PARAMETER, 11> root_parameters{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    for (uint32_t index = 0; index < 9; ++index) {
        root_parameters[index + 2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        root_parameters[index + 2U].Descriptor.ShaderRegister = index;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(root_parameters.size());
    root_desc.pParameters = root_parameters.data();
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, root_blob.GetAddressOf(),
                                           root_error.GetAddressOf())) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(state.root_signature_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }

    std::array<std::byte, maximum_shader_bytes> shader{};
    for (size_t index = 0; index < shader_names.size(); ++index) {
        size_t shader_size = 0;
        if (!load_shader(shader_directory, shader_names[index], &shader, &shader_size)) {
            return SACCADE_ERROR_NOT_FOUND;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = state.root_signature_.Get();
        pipeline_desc.CS = {shader.data(), shader_size};
        if (FAILED(device->CreateComputePipelineState(&pipeline_desc,
                                                      IID_PPV_ARGS(state.pipelines_[index].GetAddressOf())))) {
            return SACCADE_ERROR_BACKEND;
        }
    }

    const uint64_t block_count = (static_cast<uint64_t>(spec.candidate_capacity) + block_size - 1U) / block_size;
    const uint64_t mask_words = (spec.target_capacity + 31U) / 32U;
    state.packet_capacity_ =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(spec.target_capacity) * sizeof(SaccadeTargetRecord);
    auto create_buffer = [device](uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial_state,
                                  D3D12_RESOURCE_FLAGS flags, ID3D12Resource** output) noexcept {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
                                               IID_PPV_ARGS(output));
    };
    const uint64_t entries_bytes = static_cast<uint64_t>(spec.candidate_capacity) * sizeof(RadixEntry);
    const uint64_t offsets_bytes = block_count * radix_bins * sizeof(uint32_t);
    const uint64_t ranks_bytes = static_cast<uint64_t>(spec.candidate_capacity) * sizeof(uint32_t);
    const uint64_t masks_bytes = static_cast<uint64_t>(spec.target_capacity) * mask_words * sizeof(uint32_t);
    const uint64_t suppressed_bytes = mask_words * sizeof(uint32_t);
    const uint64_t packed_candidate_bytes =
        static_cast<uint64_t>(spec.candidate_capacity) * sizeof(kernels::targets::DenseCandidate);
    constexpr D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const bool resources_created =
        SUCCEEDED(create_buffer(radix_passes * parameter_stride, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                                state.parameters_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(entries_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.entries_a_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(entries_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.entries_b_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(offsets_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.block_offsets_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(ranks_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.local_ranks_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(masks_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.masks_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(suppressed_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.suppressed_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(state.packet_capacity_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                uav, state.packet_.GetAddressOf())) &&
        SUCCEEDED(create_buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav,
                                state.counters_.GetAddressOf())) &&
        (spec.candidate_input != CandidateInput::normalized_fp16 ||
         SUCCEEDED(create_buffer(packed_candidate_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 uav, state.packed_candidates_.GetAddressOf()))) &&
        SUCCEEDED(create_buffer(state.packet_capacity_, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_FLAG_NONE, state.readback_.GetAddressOf()));
    if (!resources_created) return SACCADE_ERROR_BACKEND;

    void* parameter_bytes = nullptr;
    void* readback_bytes = nullptr;
    if (FAILED(state.parameters_->Map(0, nullptr, &parameter_bytes)) ||
        FAILED(state.readback_->Map(0, nullptr, &readback_bytes))) {
        return SACCADE_ERROR_BACKEND;
    }
    state.parameter_bytes_ = static_cast<uint8_t*>(parameter_bytes);
    state.readback_bytes_ = static_cast<const uint8_t*>(readback_bytes);
    state.stats_.workspace_bytes = radix_passes * parameter_stride + entries_bytes * 2U + offsets_bytes + ranks_bytes +
                                   masks_bytes + suppressed_bytes + state.packet_capacity_ + 16 +
                                   (state.packed_candidates_ == nullptr ? 0 : packed_candidate_bytes);
    state.stats_.packet_readback_bytes = state.packet_capacity_;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(state.allocator_.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.allocator_.Get(), nullptr,
                                         IID_PPV_ARGS(state.commands_.GetAddressOf()))) ||
        FAILED(state.commands_->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state.fence_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    state.completion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state.completion_event_ == nullptr) return SACCADE_ERROR_BACKEND;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::submit(uint32_t candidate_count, const kernels::targets::PostprocessConfig& config,
                                          const kernels::targets::PostprocessEpochs& epochs,
                                          TargetPostprocessSubmission* output) noexcept {
    return submit(candidate_count, 0, 0, config, epochs, output);
}

SaccadeResult TargetPostprocessor::submit(uint32_t candidate_count, uint32_t source_width, uint32_t source_height,
                                          const kernels::targets::PostprocessConfig& config,
                                          const kernels::targets::PostprocessEpochs& epochs,
                                          TargetPostprocessSubmission* output) noexcept {
    if (!initialized_ || output == nullptr || candidate_count > impl().spec_.candidate_capacity ||
        (impl().spec_.candidate_input == CandidateInput::normalized_fp16 &&
         (source_width == 0 || source_height == 0)) ||
        !config_valid(config, epochs, impl().spec_.target_capacity)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    const SaccadeResult completion = state.completion_status();
    if (completion == SACCADE_ERROR_BUSY) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    if (completion != SACCADE_OK) return completion;
    const uint32_t blocks = (candidate_count + block_size - 1U) / block_size;
    const uint32_t words = (config.maximum_targets + 31U) / 32U;
    for (uint32_t pass = 0; pass < radix_passes; ++pass) {
        auto* parameters =
            reinterpret_cast<Parameters*>(state.parameter_bytes_ + static_cast<size_t>(pass) * parameter_stride);
        *parameters = {candidate_count,
                       config.maximum_targets,
                       blocks,
                       words,
                       config.minimum_confidence_q16,
                       config.band_minimum_confidence_q16,
                       config.band_min_short_side_q3,
                       config.band_max_short_side_q3,
                       config.iou_threshold_q16,
                       config.coordinate_space,
                       pass * 4U,
                       state.spec_.model_width,
                       state.spec_.model_height,
                       source_width,
                       source_height,
                       epochs.frame_id,
                       epochs.model_epoch,
                       epochs.session_epoch,
                       epochs.transform_epoch,
                       epochs.topology_epoch,
                       epochs.source_id};
    }
    if (FAILED(state.allocator_->Reset()) || FAILED(state.commands_->Reset(state.allocator_.Get(), nullptr))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    state.commands_->SetComputeRootSignature(state.root_signature_.Get());
    if (state.sequence_ != 0) {
        const D3D12_RESOURCE_BARRIER barrier =
            transition(state.packet_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        state.commands_->ResourceBarrier(1, &barrier);
    }
    state.bind_resources(0, state.packed_candidates_ != nullptr);
    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    if (candidate_count != 0) {
        if (state.packed_candidates_ != nullptr) {
            state.commands_->SetPipelineState(state.pipelines_[pack_normalized].Get());
            state.commands_->Dispatch(blocks, 1, 1);
            uav_barrier.UAV.pResource = state.packed_candidates_.Get();
            state.commands_->ResourceBarrier(1, &uav_barrier);
            uav_barrier.UAV.pResource = nullptr;
            state.bind_resources(0);
        }
        state.commands_->SetPipelineState(state.pipelines_[prepare].Get());
        state.commands_->Dispatch(blocks, 1, 1);
        state.commands_->ResourceBarrier(1, &uav_barrier);
        for (uint32_t pass = 0; pass < radix_passes; ++pass) {
            state.bind_resources(pass);
            state.commands_->SetPipelineState(state.pipelines_[histogram].Get());
            state.commands_->Dispatch(blocks, 1, 1);
            state.commands_->ResourceBarrier(1, &uav_barrier);
            state.commands_->SetPipelineState(state.pipelines_[scan].Get());
            state.commands_->Dispatch(1, 1, 1);
            state.commands_->ResourceBarrier(1, &uav_barrier);
            state.commands_->SetPipelineState(state.pipelines_[scatter].Get());
            state.commands_->Dispatch(blocks, 1, 1);
            state.commands_->ResourceBarrier(1, &uav_barrier);
        }
        state.bind_resources(0);
        state.commands_->SetPipelineState(state.pipelines_[suppression_masks].Get());
        const uint32_t selected = std::min(candidate_count, config.maximum_targets);
        const uint32_t mask_work = selected * words;
        state.commands_->Dispatch((mask_work + block_size - 1U) / block_size, 1, 1);
        state.commands_->ResourceBarrier(1, &uav_barrier);
    }
    state.bind_resources(0);
    state.commands_->SetPipelineState(state.pipelines_[finalize].Get());
    state.commands_->Dispatch(1, 1, 1);
    state.commands_->ResourceBarrier(1, &uav_barrier);
    const D3D12_RESOURCE_BARRIER copy_barrier =
        transition(state.packet_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    state.commands_->ResourceBarrier(1, &copy_barrier);
    state.commands_->CopyResource(state.readback_.Get(), state.packet_.Get());
    if (FAILED(state.commands_->Close())) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    ID3D12CommandList* command_lists[]{state.commands_.Get()};
    state.queue_->ExecuteCommandLists(1, command_lists);
    ++state.sequence_;
    if (FAILED(state.queue_->Signal(state.fence_.Get(), state.sequence_))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    state.frame_id_ = epochs.frame_id;
    ++state.stats_.submissions;
    *output = {state.sequence_, epochs.frame_id};
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::adopt_current_thread() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    const SaccadeResult completion = state.completion_status();
    if (completion != SACCADE_OK) return completion;
    state.owner_thread_ = GetCurrentThreadId();
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::poll(const TargetPostprocessSubmission& submission, bool* complete) noexcept {
    if (!initialized_ || complete == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (!state.matches(submission)) return SACCADE_ERROR_STALE_HANDLE;
    const SaccadeResult completion = state.completion_status();
    *complete = completion == SACCADE_OK;
    return completion == SACCADE_ERROR_BUSY ? SACCADE_OK : completion;
}

SaccadeResult TargetPostprocessor::wait(const TargetPostprocessSubmission& submission, uint64_t timeout_ns) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (!state.matches(submission)) return SACCADE_ERROR_STALE_HANDLE;
    SaccadeResult completion = state.completion_status();
    if (completion != SACCADE_ERROR_BUSY) return completion;
    if (FAILED(state.fence_->SetEventOnCompletion(submission.sequence, state.completion_event_))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    const DWORD waited =
        WaitForSingleObject(state.completion_event_, static_cast<DWORD>(timeout_milliseconds(timeout_ns)));
    if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_TIMEOUT;
    if (waited != WAIT_OBJECT_0) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    completion = state.completion_status();
    return completion == SACCADE_ERROR_BUSY ? SACCADE_ERROR_BACKEND : completion;
}

SaccadeResult TargetPostprocessor::packet(const TargetPostprocessSubmission& submission,
                                          TargetPacketSpan* output) noexcept {
    if (!initialized_ || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (!state.matches(submission)) return SACCADE_ERROR_STALE_HANDLE;
    const SaccadeResult completion = state.completion_status();
    if (completion != SACCADE_OK) return completion;
    uint32_t structure_size = 0;
    uint32_t target_count = 0;
    uint64_t total_size = 0;
    std::memcpy(&structure_size, state.readback_bytes_, sizeof(structure_size));
    std::memcpy(&target_count, state.readback_bytes_ + 8, sizeof(target_count));
    std::memcpy(&total_size, state.readback_bytes_ + 88, sizeof(total_size));
    if (structure_size != sizeof(SaccadeTargetPacketHeader) || target_count > state.spec_.target_capacity ||
        total_size < sizeof(SaccadeTargetPacketHeader) || total_size > state.packet_capacity_) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    *output = {state.readback_bytes_, static_cast<size_t>(total_size)};
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::memory_stats(SaccadeMemoryStats* output) const noexcept {
    if (!initialized_ || output == nullptr || output->struct_size < offsetof(SaccadeMemoryStats, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeMemoryStats value{};
    value.struct_size = std::min<uint32_t>(output->struct_size, sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    value.host_committed = radix_passes * parameter_stride + impl().packet_capacity_;
    value.device_owned = impl().stats_.workspace_bytes;
    value.high_water_bytes = value.host_committed + value.device_owned;
    std::memcpy(output, &value, value.struct_size);
    return SACCADE_OK;
}

TargetPostprocessorStats TargetPostprocessor::stats() const noexcept {
    return impl().stats_;
}

} // namespace saccade::backend::d3d12

#include "backends/metal/target_postprocessor.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>

namespace saccade::backend::metal {
namespace {

constexpr uint32_t radix_passes = 16;
constexpr uint32_t radix_bins = 16;
constexpr uint32_t block_size = 256;
constexpr size_t parameter_stride = 256;

struct alignas(16) PostprocessParameters {
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
    uint64_t frame_id;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
};

struct alignas(16) RadixEntry {
    uint64_t key;
    uint32_t candidate_index;
    uint32_t reserved;
};

struct PostprocessCounters {
    uint32_t candidates_above_threshold;
    uint32_t targets_written;
    uint32_t containment_suppressed;
    uint32_t iou_suppressed;
};

static_assert(sizeof(PostprocessParameters) == 96);
static_assert(sizeof(RadixEntry) == 16);
static_assert(sizeof(PostprocessCounters) == 16);

uint64_t allocated_bytes(id<MTLBuffer> buffer) noexcept {
    return buffer == nil ? 0 : static_cast<uint64_t>(buffer.allocatedSize);
}

bool config_valid(const kernels::targets::PostprocessConfig& config, const kernels::targets::PostprocessEpochs& epochs,
                  uint32_t target_capacity) noexcept {
    return config.maximum_targets != 0 && config.maximum_targets <= target_capacity &&
           config.coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 &&
           (config.coordinate_space == SACCADE_COORDINATE_SPACE_MODEL_Q8 ||
            config.coordinate_space == SACCADE_COORDINATE_SPACE_SOURCE_Q8) &&
           kernels::targets::confidence_band_valid(config) && config.reserved == 0 && epochs.frame_id != 0 &&
           epochs.model_epoch != 0 && epochs.session_epoch != 0 && epochs.transform_epoch != 0 &&
           epochs.topology_epoch != 0 && epochs.source_id != 0;
}

} // namespace

struct TargetPostprocessor::Impl {
    id<MTLDevice> device_ = nil;
    id<MTLLibrary> library_ = nil;
    id<MTLComputePipelineState> prepare_ = nil;
    id<MTLComputePipelineState> histogram_ = nil;
    id<MTLComputePipelineState> scan_ = nil;
    id<MTLComputePipelineState> scatter_ = nil;
    id<MTLComputePipelineState> masks_pipeline_ = nil;
    id<MTLComputePipelineState> finalize_ = nil;
    id<MTLBuffer> parameters_ = nil;
    id<MTLBuffer> entries_a_ = nil;
    id<MTLBuffer> entries_b_ = nil;
    id<MTLBuffer> block_offsets_ = nil;
    id<MTLBuffer> local_ranks_ = nil;
    id<MTLBuffer> masks_ = nil;
    id<MTLBuffer> suppressed_ = nil;
    id<MTLBuffer> output_ = nil;
    id<MTLBuffer> counters_ = nil;
    id<MTLBuffer> candidate_buffer_ = nil;
    id<MTLCommandQueue> queue3_ = nil;
    id<MTLCommandBuffer> command_buffer3_ = nil;
    id queue4_ = nil;
    std::array<id, radix_passes> argument_tables4_{};
    id allocator4_ = nil;
    id command_buffer4_ = nil;
    id residency_set4_ = nil;
    id<MTLSharedEvent> completion_event_ = nil;
    TargetPostprocessorSpec spec_{};
    TargetPostprocessorStats stats_{};
    uint64_t sequence_ = 0;
    uint64_t frame_id_ = 0;
    uint64_t counted_sequence_ = 0;
    size_t output_capacity_ = 0;
    uint32_t candidate_count_ = 0;
    bool initialized_ = false;

    ~Impl() {
        bool retired = true;
        if (completion_event_ != nil && sequence_ != 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (completion_event_.signaledValue < sequence_) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    retired = false;
                    break;
                }
                std::this_thread::yield();
            }
        }
        if (@available(macOS 26.0, *)) {
            if (retired && queue4_ != nil && residency_set4_ != nil) {
                [queue4_ removeResidencySet:residency_set4_];
                [residency_set4_ endResidency];
            }
        }
    }

    bool metal4_supported() const noexcept {
        if (@available(macOS 26.0, *)) {
            return [device_ supportsFamily:MTLGPUFamilyMetal4];
        }
        return false;
    }

    id<MTLComputePipelineState> create_pipeline(const char* name) noexcept {
        NSString* function_name = [NSString stringWithUTF8String:name];
        id<MTLFunction> function = [library_ newFunctionWithName:function_name];
        if (function == nil) {
            return nil;
        }
        NSError* error = nil;
        return [device_ newComputePipelineStateWithFunction:function error:&error];
    }

    bool create_pipelines(const char* metallib_path) noexcept {
        NSString* path = [NSString stringWithUTF8String:metallib_path];
        NSError* error = nil;
        library_ = [device_ newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (library_ == nil) {
            return false;
        }
        prepare_ = create_pipeline("saccade_targets_prepare");
        histogram_ = create_pipeline("saccade_targets_radix_histogram");
        scan_ = create_pipeline("saccade_targets_radix_scan");
        scatter_ = create_pipeline("saccade_targets_radix_scatter");
        masks_pipeline_ = create_pipeline("saccade_targets_suppression_masks");
        finalize_ = create_pipeline("saccade_targets_finalize");
        return prepare_ != nil && histogram_ != nil && scan_ != nil && scatter_ != nil && masks_pipeline_ != nil &&
               finalize_ != nil;
    }

    bool create_buffers() noexcept {
        constexpr MTLResourceOptions shared = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
        constexpr MTLResourceOptions private_options =
            MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked;
        const size_t entries_bytes = size_t(spec_.candidate_capacity) * sizeof(RadixEntry);
        const size_t block_count = (spec_.candidate_capacity + block_size - 1) / block_size;
        const size_t offsets_bytes = block_count * radix_bins * sizeof(uint32_t);
        const size_t ranks_bytes = size_t(spec_.candidate_capacity) * sizeof(uint32_t);
        const size_t words = (spec_.target_capacity + 31U) / 32U;
        const size_t masks_bytes = size_t(spec_.target_capacity) * words * sizeof(uint32_t);
        output_capacity_ =
            sizeof(SaccadeTargetPacketHeader) + size_t(spec_.target_capacity) * sizeof(SaccadeTargetRecord);
        parameters_ = [device_ newBufferWithLength:radix_passes * parameter_stride options:shared];
        entries_a_ = [device_ newBufferWithLength:entries_bytes options:private_options];
        entries_b_ = [device_ newBufferWithLength:entries_bytes options:private_options];
        block_offsets_ = [device_ newBufferWithLength:offsets_bytes options:private_options];
        local_ranks_ = [device_ newBufferWithLength:ranks_bytes options:private_options];
        masks_ = [device_ newBufferWithLength:masks_bytes options:private_options];
        suppressed_ = [device_ newBufferWithLength:words * sizeof(uint32_t) options:private_options];
        output_ = [device_ newBufferWithLength:output_capacity_ options:shared];
        counters_ = [device_ newBufferWithLength:sizeof(PostprocessCounters) options:shared];
        return parameters_ != nil && entries_a_ != nil && entries_b_ != nil && block_offsets_ != nil &&
               local_ranks_ != nil && masks_ != nil && suppressed_ != nil && output_ != nil && counters_ != nil;
    }

    bool create_metal3() noexcept {
        queue3_ = [device_ newCommandQueueWithMaxCommandBufferCount:1];
        return queue3_ != nil;
    }

    void bind_table(id<MTL4ArgumentTable> table, uint32_t pass) noexcept API_AVAILABLE(macos(26.0)) {
        [table setAddress:candidate_buffer_.gpuAddress + spec_.candidate_offset atIndex:0];
        [table setAddress:parameters_.gpuAddress + pass * parameter_stride atIndex:1];
        [table setAddress:entries_a_.gpuAddress atIndex:2];
        [table setAddress:entries_b_.gpuAddress atIndex:3];
        [table setAddress:block_offsets_.gpuAddress atIndex:4];
        [table setAddress:local_ranks_.gpuAddress atIndex:5];
        [table setAddress:masks_.gpuAddress atIndex:6];
        [table setAddress:suppressed_.gpuAddress atIndex:7];
        [table setAddress:output_.gpuAddress atIndex:8];
        [table setAddress:counters_.gpuAddress atIndex:9];
    }

    bool create_metal4() noexcept API_AVAILABLE(macos(26.0)) {
        queue4_ = [device_ newMTL4CommandQueue];
        MTL4ArgumentTableDescriptor* descriptor = [[MTL4ArgumentTableDescriptor alloc] init];
        descriptor.maxBufferBindCount = 10;
        descriptor.initializeBindings = YES;
        for (uint32_t pass = 0; pass < radix_passes; ++pass) {
            argument_tables4_[pass] = [device_ newArgumentTableWithDescriptor:descriptor error:nil];
            if (argument_tables4_[pass] == nil) {
                return false;
            }
            bind_table(static_cast<id<MTL4ArgumentTable>>(argument_tables4_[pass]), pass);
        }
        allocator4_ = [device_ newCommandAllocator];
        command_buffer4_ = [device_ newCommandBuffer];
        MTLResidencySetDescriptor* residency_descriptor = [[MTLResidencySetDescriptor alloc] init];
        residency_descriptor.initialCapacity = 10;
        residency_set4_ = [device_ newResidencySetWithDescriptor:residency_descriptor error:nil];
        if (queue4_ == nil || allocator4_ == nil || command_buffer4_ == nil || residency_set4_ == nil) {
            return false;
        }
        const std::array<id<MTLAllocation>, 10> allocations{candidate_buffer_, parameters_,  entries_a_, entries_b_,
                                                            block_offsets_,    local_ranks_, masks_,     suppressed_,
                                                            output_,           counters_};
        [residency_set4_ addAllocations:allocations.data() count:allocations.size()];
        [residency_set4_ commit];
        [residency_set4_ requestResidency];
        [queue4_ addResidencySet:residency_set4_];
        return true;
    }

    void discard_metal4() noexcept API_AVAILABLE(macos(26.0)) {
        if (queue4_ != nil && residency_set4_ != nil) {
            [queue4_ removeResidencySet:residency_set4_];
            [residency_set4_ endResidency];
        }
        queue4_ = nil;
        argument_tables4_ = {};
        allocator4_ = nil;
        command_buffer4_ = nil;
        residency_set4_ = nil;
    }

    bool idle() const noexcept { return sequence_ == 0 || completion_event_.signaledValue >= sequence_; }

    void fill_parameters(const kernels::targets::PostprocessConfig& config,
                         const kernels::targets::PostprocessEpochs& epochs) noexcept {
        auto* bytes = static_cast<uint8_t*>(parameters_.contents);
        const uint32_t blocks = (candidate_count_ + block_size - 1) / block_size;
        const uint32_t words = (config.maximum_targets + 31U) / 32U;
        for (uint32_t pass = 0; pass < radix_passes; ++pass) {
            auto* parameters = reinterpret_cast<PostprocessParameters*>(bytes + size_t(pass) * parameter_stride);
            *parameters = {candidate_count_,
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
                           epochs.frame_id,
                           epochs.model_epoch,
                           epochs.session_epoch,
                           epochs.transform_epoch,
                           epochs.topology_epoch,
                           epochs.source_id};
        }
    }

    void bind_metal3(id<MTLComputeCommandEncoder> encoder, uint32_t pass) noexcept {
        [encoder setBuffer:candidate_buffer_ offset:spec_.candidate_offset atIndex:0];
        [encoder setBuffer:parameters_ offset:pass * parameter_stride atIndex:1];
        [encoder setBuffer:entries_a_ offset:0 atIndex:2];
        [encoder setBuffer:entries_b_ offset:0 atIndex:3];
        [encoder setBuffer:block_offsets_ offset:0 atIndex:4];
        [encoder setBuffer:local_ranks_ offset:0 atIndex:5];
        [encoder setBuffer:masks_ offset:0 atIndex:6];
        [encoder setBuffer:suppressed_ offset:0 atIndex:7];
        [encoder setBuffer:output_ offset:0 atIndex:8];
        [encoder setBuffer:counters_ offset:0 atIndex:9];
    }

    void bind_parameters_metal3(id<MTLComputeCommandEncoder> encoder, uint32_t pass) noexcept {
        [encoder setBuffer:parameters_ offset:pass * parameter_stride atIndex:1];
    }

    void encode_graph(id<MTLComputeCommandEncoder> encoder) noexcept {
        const MTLSize block = MTLSizeMake(block_size, 1, 1);
        if (candidate_count_ != 0) {
            [encoder setComputePipelineState:prepare_];
            [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            for (uint32_t pass = 0; pass < radix_passes; ++pass) {
                bind_parameters_metal3(encoder, pass);
                [encoder setComputePipelineState:histogram_];
                [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [encoder setComputePipelineState:scan_];
                [encoder dispatchThreads:MTLSizeMake(radix_bins, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(radix_bins, 1, 1)];
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [encoder setComputePipelineState:scatter_];
                [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            }
            const auto* parameters = static_cast<const PostprocessParameters*>(parameters_.contents);
            const uint32_t selected = std::min(candidate_count_, parameters->maximum_targets);
            [encoder setComputePipelineState:masks_pipeline_];
            [encoder dispatchThreads:MTLSizeMake(size_t(selected) * parameters->mask_word_count, 1, 1)
                threadsPerThreadgroup:block];
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        bind_parameters_metal3(encoder, 0);
        [encoder setComputePipelineState:finalize_];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    }

    bool encode_metal3() noexcept {
        command_buffer3_ = [queue3_ commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer3_ computeCommandEncoder];
        if (command_buffer3_ == nil || encoder == nil) {
            return false;
        }
        bind_metal3(encoder, 0);
        encode_graph(encoder);
        [encoder endEncoding];
        [command_buffer3_ encodeSignalEvent:completion_event_ value:sequence_];
        [command_buffer3_ commit];
        return true;
    }

    bool encode_metal4() noexcept API_AVAILABLE(macos(26.0)) {
        if (sequence_ > 1) {
            [allocator4_ reset];
        }
        id<MTL4CommandBuffer> command_buffer = static_cast<id<MTL4CommandBuffer>>(command_buffer4_);
        [command_buffer beginCommandBufferWithAllocator:allocator4_];
        id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        const MTLSize block = MTLSizeMake(block_size, 1, 1);
        [encoder setArgumentTable:argument_tables4_[0]];
        if (candidate_count_ != 0) {
            [encoder setComputePipelineState:prepare_];
            [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
            [encoder barrierAfterEncoderStages:MTLStageDispatch
                           beforeEncoderStages:MTLStageDispatch
                             visibilityOptions:MTL4VisibilityOptionDevice];
            for (uint32_t pass = 0; pass < radix_passes; ++pass) {
                [encoder setArgumentTable:argument_tables4_[pass]];
                [encoder setComputePipelineState:histogram_];
                [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
                [encoder barrierAfterEncoderStages:MTLStageDispatch
                               beforeEncoderStages:MTLStageDispatch
                                 visibilityOptions:MTL4VisibilityOptionDevice];
                [encoder setComputePipelineState:scan_];
                [encoder dispatchThreads:MTLSizeMake(radix_bins, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(radix_bins, 1, 1)];
                [encoder barrierAfterEncoderStages:MTLStageDispatch
                               beforeEncoderStages:MTLStageDispatch
                                 visibilityOptions:MTL4VisibilityOptionDevice];
                [encoder setComputePipelineState:scatter_];
                [encoder dispatchThreads:MTLSizeMake(candidate_count_, 1, 1) threadsPerThreadgroup:block];
                [encoder barrierAfterEncoderStages:MTLStageDispatch
                               beforeEncoderStages:MTLStageDispatch
                                 visibilityOptions:MTL4VisibilityOptionDevice];
            }
            const auto* parameters = static_cast<const PostprocessParameters*>(parameters_.contents);
            const uint32_t selected = std::min(candidate_count_, parameters->maximum_targets);
            [encoder setArgumentTable:argument_tables4_[0]];
            [encoder setComputePipelineState:masks_pipeline_];
            [encoder dispatchThreads:MTLSizeMake(size_t(selected) * parameters->mask_word_count, 1, 1)
                threadsPerThreadgroup:block];
            [encoder barrierAfterEncoderStages:MTLStageDispatch
                           beforeEncoderStages:MTLStageDispatch
                             visibilityOptions:MTL4VisibilityOptionDevice];
        }
        [encoder setArgumentTable:argument_tables4_[0]];
        [encoder setComputePipelineState:finalize_];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [command_buffer endCommandBuffer];
        id<MTL4CommandBuffer> buffers[] = {command_buffer};
        [queue4_ commit:buffers count:1];
        [queue4_ signalEvent:completion_event_ value:sequence_];
        return true;
    }

    void count_completion() noexcept {
        if (sequence_ != 0 && counted_sequence_ != sequence_ && idle()) {
            counted_sequence_ = sequence_;
            ++stats_.completed;
            const auto* header = static_cast<const SaccadeTargetPacketHeader*>(output_.contents);
            stats_.packet_readback_bytes += header->total_size;
        }
    }
};

TargetPostprocessor::TargetPostprocessor() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    ::new (static_cast<void*>(storage_.data())) Impl{};
}

TargetPostprocessor::~TargetPostprocessor() {
    impl().~Impl();
}

TargetPostprocessor::Impl& TargetPostprocessor::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const TargetPostprocessor::Impl& TargetPostprocessor::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult TargetPostprocessor::initialize(void* metal_device, const char* metallib_path, PathPreference preference,
                                              const TargetPostprocessorSpec& spec) noexcept {
    Impl& state = impl();
    if (metal_device == nullptr || metallib_path == nullptr || metallib_path[0] == '\0' ||
        spec.candidate_capacity == 0 || spec.candidate_capacity > kernels::targets::maximum_candidates ||
        spec.target_capacity == 0 || spec.target_capacity > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        (preference != PathPreference::automatic && preference != PathPreference::metal3 &&
         preference != PathPreference::metal4)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    state.device_ = (__bridge id<MTLDevice>)metal_device;
    state.spec_ = spec;
    state.candidate_buffer_ = (__bridge id<MTLBuffer>)spec.candidate_buffer;
    const size_t candidate_bytes = size_t(spec.candidate_capacity) * sizeof(kernels::targets::DenseCandidate);
    if (state.candidate_buffer_ == nil || state.candidate_buffer_.device != state.device_ ||
        spec.candidate_offset > state.candidate_buffer_.length ||
        candidate_bytes > state.candidate_buffer_.length - spec.candidate_offset) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state.stats_.candidate_capacity = spec.candidate_capacity;
    state.stats_.target_capacity = spec.target_capacity;
    state.stats_.radix_passes = radix_passes;
    if (!state.create_buffers() || !state.create_pipelines(metallib_path)) {
        return SACCADE_ERROR_BACKEND;
    }
    state.completion_event_ = [state.device_ newSharedEvent];
    if (state.completion_event_ == nil) {
        return SACCADE_ERROR_BACKEND;
    }
    const bool supports_metal4 = state.metal4_supported();
    if (preference == PathPreference::metal4 && !supports_metal4) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    const bool use_metal4 =
        preference == PathPreference::metal4 || (preference == PathPreference::automatic && supports_metal4);
    if (use_metal4) {
        if (@available(macOS 26.0, *)) {
            if (!state.create_metal4()) {
                state.discard_metal4();
                return SACCADE_ERROR_BACKEND;
            }
            state.stats_.path = Path::metal4;
        }
    } else {
        if (!state.create_metal3()) {
            return SACCADE_ERROR_BACKEND;
        }
        state.stats_.path = Path::metal3;
    }
    state.stats_.workspace_bytes =
        allocated_bytes(state.parameters_) + allocated_bytes(state.entries_a_) + allocated_bytes(state.entries_b_) +
        allocated_bytes(state.block_offsets_) + allocated_bytes(state.local_ranks_) + allocated_bytes(state.masks_) +
        allocated_bytes(state.suppressed_) + allocated_bytes(state.output_) + allocated_bytes(state.counters_);
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::submit(uint32_t candidate_count, const kernels::targets::PostprocessConfig& config,
                                          const kernels::targets::PostprocessEpochs& epochs,
                                          TargetPostprocessSubmission* submission) noexcept {
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (submission == nullptr || candidate_count > state.spec_.candidate_capacity ||
        !config_valid(config, epochs, state.spec_.target_capacity)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state.count_completion();
    if (!state.idle()) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    state.candidate_count_ = candidate_count;
    state.frame_id_ = epochs.frame_id;
    state.fill_parameters(config, epochs);
    ++state.sequence_;
    const bool encoded = state.stats_.path == Path::metal4 ? ([&]() noexcept {
        if (@available(macOS 26.0, *)) {
            return state.encode_metal4();
        }
        return false;
    })()
                                                           : state.encode_metal3();
    if (!encoded) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    ++state.stats_.submissions;
    *submission = {state.sequence_, epochs.frame_id, candidate_count, 0};
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::poll(const TargetPostprocessSubmission& submission, bool* complete) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || complete == nullptr || submission.sequence == 0 ||
        submission.sequence != state.sequence_ || submission.frame_id != state.frame_id_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *complete = state.idle();
    state.count_completion();
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::wait(const TargetPostprocessSubmission& submission, uint64_t timeout_ns) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || submission.sequence == 0 || submission.sequence != state.sequence_ ||
        submission.frame_id != state.frame_id_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const auto start = std::chrono::steady_clock::now();
    while (!state.idle()) {
        if (timeout_ns != UINT64_MAX && static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                  std::chrono::steady_clock::now() - start)
                                                                  .count()) >= timeout_ns) {
            return SACCADE_ERROR_TIMEOUT;
        }
        std::this_thread::yield();
    }
    state.count_completion();
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::packet(const TargetPostprocessSubmission& submission,
                                          TargetPacketSpan* packet) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || packet == nullptr || submission.sequence == 0 ||
        submission.sequence != state.sequence_ || !state.idle()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state.count_completion();
    const auto* header = static_cast<const SaccadeTargetPacketHeader*>(state.output_.contents);
    *packet = {static_cast<const uint8_t*>(state.output_.contents), static_cast<size_t>(header->total_size)};
    return SACCADE_OK;
}

SaccadeResult TargetPostprocessor::memory_stats(SaccadeMemoryStats* output) const noexcept {
    const Impl& state = impl();
    if (!state.initialized_ || output == nullptr || output->struct_size != sizeof(*output) ||
        output->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeMemoryStats value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.host_committed = sizeof(Impl);
    value.device_imported = allocated_bytes(state.candidate_buffer_);
    value.device_owned = state.stats_.workspace_bytes;
    if (@available(macOS 26.0, *)) {
        if (state.allocator4_ != nil) {
            value.framework_opaque = [state.allocator4_ allocatedSize];
        }
    }
    value.high_water_bytes = value.host_committed + value.device_imported + value.device_owned + value.framework_opaque;
    *output = value;
    return SACCADE_OK;
}

TargetPostprocessorStats TargetPostprocessor::stats() const noexcept {
    TargetPostprocessorStats value = impl().stats_;
    if (@available(macOS 26.0, *)) {
        if (impl().allocator4_ != nil) {
            value.command_allocator_bytes = [impl().allocator4_ allocatedSize];
        }
    }
    return value;
}

} // namespace saccade::backend::metal

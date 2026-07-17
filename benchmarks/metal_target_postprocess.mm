#include "backends/metal/target_postprocessor.hpp"
#include "kernels/targets/postprocess.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    candidate_buffer_failed = 2,
    metal3_path_failed = 3,
    automatic_path_failed = 4,
    unsupported = 77
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

using Clock = std::chrono::steady_clock;
using saccade::backend::metal::PathPreference;
using saccade::backend::metal::TargetPostprocessor;
using saccade::backend::metal::TargetPostprocessorSpec;
using saccade::backend::metal::TargetPostprocessSubmission;
using saccade::kernels::targets::DenseCandidate;
using saccade::kernels::targets::PostprocessConfig;
using saccade::kernels::targets::PostprocessEpochs;

constexpr uint32_t target_capacity = 1024;
constexpr uint32_t sample_count = 64;

alignas(64) std::array<DenseCandidate, saccade::kernels::targets::maximum_candidates> candidates;

void make_candidates() {
    uint32_t value = 0x9e3779b9U;
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        value = value * 1664525U + 1013904223U;
        DenseCandidate candidate{};
        candidate.x_q3 = static_cast<uint16_t>((value >> 2) & 0xffffU);
        candidate.y_q3 = static_cast<uint16_t>((value >> 14) & 0xffffU);
        candidate.width_q3 = static_cast<uint16_t>(16U + (value & 127U));
        candidate.height_q3 = static_cast<uint16_t>(16U + ((value >> 7) & 127U));
        candidate.confidence_q16 = static_cast<uint16_t>(value >> 16);
        candidate.role = static_cast<uint8_t>(1U + index % 10U);
        candidate.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        candidate.flags = SACCADE_TARGET_ACTIONABLE;
        candidates[index] = candidate;
    }
}

bool run_shape(TargetPostprocessor* postprocessor, uint32_t candidate_count, uint64_t* frame_id) {
    PostprocessConfig config{};
    config.maximum_targets = target_capacity;
    config.minimum_confidence_q16 = 8192;
    config.iou_threshold_q16 = 32768;
    std::array<double, sample_count> samples{};
    for (uint32_t iteration = 0; iteration < sample_count + 4; ++iteration) {
        const uint64_t current_frame = ++*frame_id;
        const PostprocessEpochs epochs{current_frame, 1, 1, 1, 1, 1};
        TargetPostprocessSubmission submission{};
        const auto start = Clock::now();
        if (postprocessor->submit(candidate_count, config, epochs, &submission) != SACCADE_OK ||
            postprocessor->wait(submission, UINT64_MAX) != SACCADE_OK) {
            return false;
        }
        const auto end = Clock::now();
        if (iteration >= 4) {
            samples[iteration - 4] = std::chrono::duration<double, std::milli>(end - start).count();
        }
    }
    std::sort(samples.begin(), samples.end());
    const double p50 = samples[sample_count / 2];
    const double p95 = samples[(sample_count * 95U) / 100U];
    std::printf("candidates=%5u targets=%4u p50=%7.3fms p95=%7.3fms\n", candidate_count, target_capacity, p50, p95);
    return true;
}

bool run_path(id<MTLDevice> device, const char* metallib, void* buffer, PathPreference preference) {
    TargetPostprocessor postprocessor;
    if (postprocessor.initialize((__bridge void*)device, metallib, preference,
                                 {saccade::kernels::targets::maximum_candidates, target_capacity, buffer, 0}) !=
        SACCADE_OK) {
        return false;
    }
    const auto stats = postprocessor.stats();
    std::printf("path=Metal %u workspace=%.2fMiB radix_passes=%u\n", static_cast<unsigned>(stats.path),
                static_cast<double>(stats.workspace_bytes) / (1024.0 * 1024.0), stats.radix_passes);
    uint64_t frame_id = 0;
    const std::array<uint32_t, 3> shapes{1024, 10000, saccade::kernels::targets::maximum_candidates};
    for (uint32_t shape : shapes) {
        if (!run_shape(&postprocessor, shape, &frame_id)) {
            return false;
        }
    }
    const auto final_stats = postprocessor.stats();
    std::printf("command_allocator=%.2fMiB\n",
                static_cast<double>(final_stats.command_allocator_bytes) / (1024.0 * 1024.0));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return exit_code(ExitCode::invalid_arguments);
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        return exit_code(ExitCode::unsupported);
    }
    make_candidates();
    id<MTLBuffer> candidate_buffer =
        [device newBufferWithBytes:candidates.data()
                            length:sizeof(candidates)
                           options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked];
    if (candidate_buffer == nil) {
        return exit_code(ExitCode::candidate_buffer_failed);
    }
    if (!run_path(device, argv[1], (__bridge void*)candidate_buffer, PathPreference::metal3)) {
        return exit_code(ExitCode::metal3_path_failed);
    }
    if (!run_path(device, argv[1], (__bridge void*)candidate_buffer, PathPreference::automatic)) {
        return exit_code(ExitCode::automatic_path_failed);
    }
    return exit_code(ExitCode::success);
}

#include "backends/metal/target_postprocessor.hpp"
#include "kernels/targets/postprocess.hpp"
#include "scene/packet.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

using saccade::backend::metal::PathPreference;
using saccade::backend::metal::TargetPacketSpan;
using saccade::backend::metal::TargetPostprocessor;
using saccade::backend::metal::TargetPostprocessorSpec;
using saccade::backend::metal::TargetPostprocessSubmission;
using saccade::kernels::targets::DenseCandidate;
using saccade::kernels::targets::PostprocessConfig;
using saccade::kernels::targets::PostprocessEpochs;
using saccade::kernels::targets::PostprocessStats;
using saccade::kernels::targets::PostprocessWorkspace;

constexpr uint32_t candidate_count = 777;
constexpr uint32_t target_capacity = 96;

void make_candidates(std::array<DenseCandidate, candidate_count>* output) {
    uint32_t value = 0x12345678U;
    for (uint32_t index = 0; index < output->size(); ++index) {
        value = value * 1664525U + 1013904223U;
        DenseCandidate candidate{};
        candidate.x_q3 = static_cast<uint16_t>((index % 28U) * 2000U);
        candidate.y_q3 = static_cast<uint16_t>((index / 28U) * 2000U);
        candidate.width_q3 = static_cast<uint16_t>(24U + (value & 255U));
        candidate.height_q3 = static_cast<uint16_t>(24U + ((value >> 8) & 255U));
        candidate.confidence_q16 = static_cast<uint16_t>(value >> 16);
        candidate.role = static_cast<uint8_t>(index % 11U);
        candidate.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        candidate.flags = SACCADE_TARGET_ACTIONABLE;
        (*output)[index] = candidate;
    }
    (*output)[13] = {
        40, 40, 800, 600, 65535, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[29] = {
        80, 80, 100, 100, 65534, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[47] = {
        44, 44, 800, 600, 65533, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[5] = {12000,
                    12000,
                    16,
                    16,
                    65532,
                    SACCADE_TARGET_ROLE_BUTTON,
                    SACCADE_TARGET_SOURCE_NEURAL,
                    SACCADE_TARGET_ACTIONABLE,
                    0};
}

int run_path(id<MTLDevice> device, const char* metallib, const std::array<DenseCandidate, candidate_count>& candidates,
             uint32_t active_candidates, const uint8_t* expected, size_t expected_size, const PostprocessConfig& config,
             const PostprocessEpochs& epochs, PathPreference preference) {
    id<MTLBuffer> candidate_buffer =
        [device newBufferWithBytes:candidates.data()
                            length:sizeof(candidates)
                           options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked];
    if (candidate_buffer == nil) {
        return 10;
    }
    TargetPostprocessor postprocessor;
    if (postprocessor.initialize((__bridge void*)device, metallib, preference,
                                 {candidate_count, target_capacity, (__bridge void*)candidate_buffer, 0}) !=
        SACCADE_OK) {
        return 11;
    }
    TargetPostprocessSubmission submission{};
    if (postprocessor.submit(active_candidates, config, epochs, &submission) != SACCADE_OK ||
        postprocessor.wait(submission, UINT64_MAX) != SACCADE_OK) {
        return 12;
    }
    TargetPacketSpan packet{};
    saccade::scene::PacketView view{};
    if (postprocessor.packet(submission, &packet) != SACCADE_OK ||
        saccade::scene::validate_packet({packet.data, packet.size}, &view) != SACCADE_OK) {
        std::fprintf(stderr, "invalid GPU packet\n");
        return 13;
    }
    if (packet.size != expected_size || std::memcmp(packet.data, expected, std::min(packet.size, expected_size)) != 0) {
        const auto* expected_header = reinterpret_cast<const SaccadeTargetPacketHeader*>(expected);
        std::fprintf(stderr, "packet mismatch path=%u size=%zu/%zu targets=%u/%u\n",
                     static_cast<unsigned>(postprocessor.stats().path), packet.size, expected_size,
                     view.header->target_count, expected_header->target_count);
        if (view.header->target_count != 0 && expected_header->target_count != 0) {
            const auto* expected_targets =
                reinterpret_cast<const SaccadeTargetRecord*>(expected + expected_header->targets_offset);
            std::fprintf(stderr, "first target confidence=%u/%u xy=%d,%d/%d,%d\n", view.targets[0].confidence_q16,
                         expected_targets[0].confidence_q16, view.targets[0].x_q8, view.targets[0].y_q8,
                         expected_targets[0].x_q8, expected_targets[0].y_q8);
            for (uint32_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
                const DenseCandidate& candidate = candidates[candidate_index];
                if (candidate.confidence_q16 == view.targets[0].confidence_q16 &&
                    (static_cast<int32_t>(candidate.x_q3) << 5) == view.targets[0].x_q8) {
                    std::fprintf(stderr, "first GPU candidate index=%u\n", candidate_index);
                    break;
                }
            }
            for (uint32_t target_index = 1; target_index < view.header->target_count; ++target_index) {
                if (view.targets[target_index - 1].confidence_q16 < view.targets[target_index].confidence_q16) {
                    std::fprintf(stderr, "confidence inversion at %u: %u then %u\n", target_index,
                                 view.targets[target_index - 1].confidence_q16,
                                 view.targets[target_index].confidence_q16);
                    break;
                }
            }
            for (uint32_t left = 0; left < view.header->target_count; ++left) {
                for (uint32_t right = left + 1; right < view.header->target_count; ++right) {
                    if (view.targets[left].target_id == view.targets[right].target_id) {
                        std::fprintf(stderr, "duplicate targets at %u and %u\n", left, right);
                        left = view.header->target_count;
                        break;
                    }
                }
            }
        }
        const size_t common = std::min(packet.size, expected_size);
        for (size_t index = 0; index < common; ++index) {
            if (packet.data[index] != expected[index]) {
                std::fprintf(stderr, "first byte %zu: %u/%u\n", index, packet.data[index], expected[index]);
                break;
            }
        }
        return 13;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        return 77;
    }
    std::array<DenseCandidate, candidate_count> candidates{};
    make_candidates(&candidates);
    PostprocessConfig config{};
    config.maximum_targets = target_capacity;
    config.minimum_confidence_q16 = 12000;
    config.band_minimum_confidence_q16 = 10000;
    config.band_min_short_side_q3 = 96;
    config.band_max_short_side_q3 = 192;
    config.iou_threshold_q16 = 32768;
    PostprocessEpochs epochs{101, 202, 303, 404, 505, 606};
    static PostprocessWorkspace workspace;
    alignas(8) std::array<uint8_t, sizeof(SaccadeTargetPacketHeader) + target_capacity * sizeof(SaccadeTargetRecord)>
        expected{};
    size_t expected_size = 0;
    PostprocessStats stats{};
    const std::array<uint32_t, 5> counts{0, 1, 256, 257, candidate_count};
    for (uint32_t active_candidates : counts) {
        if (saccade::kernels::targets::postprocess(candidates.data(), active_candidates, config, epochs, &workspace,
                                                   {expected.data(), expected.size()}, &expected_size,
                                                   &stats) != SACCADE_OK) {
            return 2;
        }
        int result = run_path(device, argv[1], candidates, active_candidates, expected.data(), expected_size, config,
                              epochs, PathPreference::metal3);
        if (result != 0) {
            return result;
        }
        result = run_path(device, argv[1], candidates, active_candidates, expected.data(), expected_size, config,
                          epochs, PathPreference::automatic);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

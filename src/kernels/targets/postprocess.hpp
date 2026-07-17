#ifndef SACCADE_KERNELS_TARGETS_POSTPROCESS_HPP
#define SACCADE_KERNELS_TARGETS_POSTPROCESS_HPP

#include <saccade/saccade_backend.h>
#include <saccade/saccade_scene.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::kernels::targets {

constexpr uint32_t maximum_candidates = 65536;
constexpr uint16_t safe_inset_q3 = 8;

struct DenseCandidate {
    uint16_t x_q3 = 0;
    uint16_t y_q3 = 0;
    uint16_t width_q3 = 0;
    uint16_t height_q3 = 0;
    uint16_t confidence_q16 = 0;
    uint8_t role = 0;
    uint8_t source_bits = 0;
    uint16_t flags = 0;
    uint16_t reserved = 0;
};

struct PostprocessConfig {
    uint32_t maximum_targets = 0;
    uint16_t minimum_confidence_q16 = 0;
    uint16_t band_minimum_confidence_q16 = 0;
    uint16_t band_min_short_side_q3 = 0;
    uint16_t band_max_short_side_q3 = 0;
    uint16_t iou_threshold_q16 = 0;
    SaccadeCoordinateSpace coordinate_space = SACCADE_COORDINATE_SPACE_SOURCE_Q8;
    uint32_t reserved = 0;
};

struct PostprocessEpochs {
    uint64_t frame_id = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
};

struct PostprocessStats {
    uint64_t candidates_read = 0;
    uint64_t candidates_above_threshold = 0;
    uint64_t heap_replacements = 0;
    uint64_t overlap_tests = 0;
    uint64_t containment_suppressed = 0;
    uint64_t iou_suppressed = 0;
    uint32_t targets_written = 0;
    uint32_t reserved = 0;
};

struct PostprocessWorkspace {
    std::array<uint32_t, maximum_candidates> indices{};
};

static_assert(sizeof(DenseCandidate) == 16);
static_assert(sizeof(PostprocessStats) == 56);

constexpr bool confidence_band_valid(const PostprocessConfig& config) noexcept {
    const bool disabled = config.band_minimum_confidence_q16 == 0 && config.band_min_short_side_q3 == 0 &&
                          config.band_max_short_side_q3 == 0;
    const bool enabled = config.band_minimum_confidence_q16 != 0 &&
                         config.band_minimum_confidence_q16 <= config.minimum_confidence_q16 &&
                         config.band_min_short_side_q3 < config.band_max_short_side_q3;
    return disabled || enabled;
}

constexpr bool above_confidence_threshold(const DenseCandidate& candidate, const PostprocessConfig& config) noexcept {
    if (candidate.confidence_q16 >= config.minimum_confidence_q16) {
        return true;
    }
    const uint16_t short_side = candidate.width_q3 < candidate.height_q3 ? candidate.width_q3 : candidate.height_q3;
    return config.band_minimum_confidence_q16 != 0 && short_side >= config.band_min_short_side_q3 &&
           short_side < config.band_max_short_side_q3 && candidate.confidence_q16 >= config.band_minimum_confidence_q16;
}

constexpr bool has_safe_interior(const DenseCandidate& candidate) noexcept {
    return candidate.width_q3 > safe_inset_q3 * 2U && candidate.height_q3 > safe_inset_q3 * 2U;
}

SaccadeResult postprocess(const DenseCandidate*, uint32_t, const PostprocessConfig&, const PostprocessEpochs&,
                          PostprocessWorkspace*, SaccadeMutableSpanU8, size_t*, PostprocessStats*) noexcept;

} // namespace saccade::kernels::targets

#endif

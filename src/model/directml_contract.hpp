#ifndef SACCADE_MODEL_DIRECTML_CONTRACT_HPP
#define SACCADE_MODEL_DIRECTML_CONTRACT_HPP

#include "model/artifact.hpp"

#include <array>
#include <cstdint>

namespace saccade::model::directml {

constexpr uint64_t provider_compatibility_bit = UINT64_C(1) << 1;
constexpr uint32_t contract_version = 2;
constexpr uint32_t payload_header_bytes = 96;
constexpr uint32_t target_row_components = 6;
constexpr uint32_t normalized_target_row_bytes = target_row_components * 2;

enum class InputKind : uint32_t { planar_fp16 = 1, planar_int8 = 2 };

enum class OutputLayout : uint32_t { normalized_target_rows_v1 = 1 };

struct Contract {
    uint32_t candidate_capacity = 0;
    uint16_t minimum_confidence_q16 = 0;
    uint16_t band_minimum_confidence_q16 = 0;
    uint16_t band_min_short_side_q3 = 0;
    uint16_t band_max_short_side_q3 = 0;
    uint16_t iou_threshold_q16 = 0;
    InputKind input_kind = InputKind::planar_fp16;
    OutputLayout output_layout = OutputLayout::normalized_target_rows_v1;
    std::array<float, 3> channel_scale{};
    std::array<float, 3> channel_bias{};
    std::array<float, 3> letterbox_rgb{};
    SaccadeSpanU8 input_name{};
    SaccadeSpanU8 candidate_name{};
    SaccadeSpanU8 graph{};
};

SaccadeResult parse_contract(const ArtifactView&, Contract*) noexcept;

} // namespace saccade::model::directml

#endif

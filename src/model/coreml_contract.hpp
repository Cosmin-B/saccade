#ifndef SACCADE_MODEL_COREML_CONTRACT_HPP
#define SACCADE_MODEL_COREML_CONTRACT_HPP

#include "kernels/targets/postprocess.hpp"
#include "model/artifact.hpp"

#include <array>
#include <cstdint>

namespace saccade::model::coreml {

constexpr uint64_t provider_compatibility_bit = UINT64_C(1) << 0;
constexpr uint32_t contract_version = 4;
constexpr uint32_t payload_header_bytes = 112;
constexpr uint32_t target_row_components = 6;

enum class InputKind : uint32_t { image_bgra8 = 1 };

enum class OutputLayout : uint32_t { normalized_target_rows_v1 = 1 };

enum class ScalarType : uint32_t { float16 = 1, float32 = 2 };

struct Contract {
    uint32_t candidate_capacity = 0;
    uint16_t minimum_confidence_q16 = 0;
    uint16_t band_minimum_confidence_q16 = 0;
    uint16_t band_min_short_side_q3 = 0;
    uint16_t band_max_short_side_q3 = 0;
    uint16_t iou_threshold_q16 = 0;
    InputKind input_kind = InputKind::image_bgra8;
    OutputLayout output_layout = OutputLayout::normalized_target_rows_v1;
    std::array<uint8_t, 32> bundle_sha256{};
    std::array<float, 3> letterbox_rgb{};
    SaccadeSpanU8 locator{};
    SaccadeSpanU8 input_name{};
    SaccadeSpanU8 target_rows_name{};
    SaccadeSpanU8 target_count_name{};
};

struct TargetRows {
    const void* values = nullptr;
    ScalarType scalar_type = ScalarType::float32;
    uint32_t row_count = 0;
    uint32_t row_stride = 0;
    uint32_t column_count = 0;
};

SaccadeResult parse_contract(const ArtifactView&, Contract*) noexcept;
SaccadeResult decode_target_rows(const Contract&, const TargetRows&, uint32_t candidate_count, SaccadeRectI32 scope,
                                 kernels::targets::DenseCandidate*, uint32_t output_capacity,
                                 uint32_t* output_count) noexcept;

} // namespace saccade::model::coreml

#endif

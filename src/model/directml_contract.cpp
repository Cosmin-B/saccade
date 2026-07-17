#include "model/directml_contract.hpp"

#include "kernels/targets/postprocess.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace saccade::model::directml {
namespace {

constexpr uint8_t magic[] = {'S', 'C', 'D', 'M'};
constexpr uint32_t maximum_feature_name_bytes = 64;

constexpr size_t version_offset = 4;
constexpr size_t input_kind_offset = 8;
constexpr size_t output_layout_offset = 12;
constexpr size_t candidate_capacity_offset = 16;
constexpr size_t target_capacity_offset = 20;
constexpr size_t minimum_confidence_offset = 24;
constexpr size_t iou_threshold_offset = 28;
constexpr size_t input_name_size_offset = 32;
constexpr size_t candidate_name_size_offset = 36;
constexpr size_t channel_scale_offset = 40;
constexpr size_t channel_bias_offset = 52;
constexpr size_t letterbox_rgb_offset = 64;
constexpr size_t reserved_offset = 76;
constexpr size_t band_minimum_confidence_offset = 80;
constexpr size_t band_min_short_side_offset = 84;
constexpr size_t band_max_short_side_offset = 88;
constexpr size_t extended_reserved_offset = 92;

uint32_t read_u32(const uint8_t* bytes, size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1U]) << 8U |
           static_cast<uint32_t>(bytes[offset + 2U]) << 16U | static_cast<uint32_t>(bytes[offset + 3U]) << 24U;
}

std::array<float, 3> read_float3(const uint8_t* bytes, size_t offset) noexcept {
    std::array<float, 3> value{};
    std::memcpy(value.data(), bytes + offset, sizeof(value));
    return value;
}

bool finite(const std::array<float, 3>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool is_alpha(uint8_t value) noexcept {
    return (value >= static_cast<uint8_t>('A') && value <= static_cast<uint8_t>('Z')) ||
           (value >= static_cast<uint8_t>('a') && value <= static_cast<uint8_t>('z'));
}

bool is_digit(uint8_t value) noexcept {
    return value >= static_cast<uint8_t>('0') && value <= static_cast<uint8_t>('9');
}

bool valid_feature_name(SaccadeSpanU8 name) noexcept {
    if (name.data == nullptr || name.size == 0 || name.size > maximum_feature_name_bytes ||
        (!is_alpha(name.data[0]) && name.data[0] != '_')) {
        return false;
    }
    for (size_t index = 1; index < name.size; ++index) {
        const uint8_t value = name.data[index];
        if (!is_alpha(value) && !is_digit(value) && value != '_') return false;
    }
    return true;
}

} // namespace

SaccadeResult parse_contract(const ArtifactView& artifact, Contract* output) noexcept {
    if (output == nullptr || artifact.artifact != ArtifactKind::onnx ||
        (artifact.flags & artifact_relative_locator) != 0 ||
        (artifact.provider_compatibility_bits & provider_compatibility_bit) == 0 || artifact.input_channels != 3 ||
        artifact.payload.data == nullptr || artifact.payload.size <= payload_header_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    const uint8_t* data = artifact.payload.data;
    if (data[0] != magic[0] || data[1] != magic[1] || data[2] != magic[2] || data[3] != magic[3] ||
        read_u32(data, version_offset) != contract_version ||
        read_u32(data, output_layout_offset) != static_cast<uint32_t>(OutputLayout::normalized_target_rows_v1)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t input_kind = read_u32(data, input_kind_offset);
    const uint32_t candidate_capacity = read_u32(data, candidate_capacity_offset);
    const uint32_t target_capacity = read_u32(data, target_capacity_offset);
    const uint32_t minimum_confidence = read_u32(data, minimum_confidence_offset);
    const uint32_t iou_threshold = read_u32(data, iou_threshold_offset);
    const uint32_t band_minimum_confidence = read_u32(data, band_minimum_confidence_offset);
    const uint32_t band_min_short_side = read_u32(data, band_min_short_side_offset);
    const uint32_t band_max_short_side = read_u32(data, band_max_short_side_offset);
    const uint32_t input_name_size = read_u32(data, input_name_size_offset);
    const uint32_t candidate_name_size = read_u32(data, candidate_name_size_offset);
    const std::array<float, 3> channel_scale = read_float3(data, channel_scale_offset);
    const std::array<float, 3> channel_bias = read_float3(data, channel_bias_offset);
    const std::array<float, 3> letterbox_rgb = read_float3(data, letterbox_rgb_offset);
    const uint64_t names_size = static_cast<uint64_t>(input_name_size) + candidate_name_size;
    const bool fp16 = input_kind == static_cast<uint32_t>(InputKind::planar_fp16) &&
                      artifact.precision_bits == SACCADE_PRECISION_FP16;
    const bool int8 = input_kind == static_cast<uint32_t>(InputKind::planar_int8) &&
                      artifact.precision_bits == SACCADE_PRECISION_INT8;
    if ((!fp16 && !int8) || candidate_capacity == 0 || candidate_capacity > kernels::targets::maximum_candidates ||
        target_capacity != artifact.max_targets || target_capacity == 0 || target_capacity > candidate_capacity ||
        minimum_confidence > UINT16_MAX || band_minimum_confidence > UINT16_MAX || band_min_short_side > UINT16_MAX ||
        band_max_short_side > UINT16_MAX || iou_threshold > UINT16_MAX || !finite(channel_scale) ||
        !finite(channel_bias) || !finite(letterbox_rgb) || read_u32(data, reserved_offset) != 0 ||
        read_u32(data, extended_reserved_offset) != 0 || names_size >= artifact.payload.size - payload_header_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    kernels::targets::PostprocessConfig postprocess{};
    postprocess.minimum_confidence_q16 = static_cast<uint16_t>(minimum_confidence);
    postprocess.band_minimum_confidence_q16 = static_cast<uint16_t>(band_minimum_confidence);
    postprocess.band_min_short_side_q3 = static_cast<uint16_t>(band_min_short_side);
    postprocess.band_max_short_side_q3 = static_cast<uint16_t>(band_max_short_side);
    if (!kernels::targets::confidence_band_valid(postprocess)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint8_t* strings = data + payload_header_bytes;
    const SaccadeSpanU8 input_name{strings, input_name_size};
    const SaccadeSpanU8 candidate_name{strings + input_name_size, candidate_name_size};
    const SaccadeSpanU8 graph{strings + names_size,
                              artifact.payload.size - payload_header_bytes - static_cast<size_t>(names_size)};
    if (!valid_feature_name(input_name) || !valid_feature_name(candidate_name) || graph.size == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {candidate_capacity,
               static_cast<uint16_t>(minimum_confidence),
               static_cast<uint16_t>(band_minimum_confidence),
               static_cast<uint16_t>(band_min_short_side),
               static_cast<uint16_t>(band_max_short_side),
               static_cast<uint16_t>(iou_threshold),
               static_cast<InputKind>(input_kind),
               OutputLayout::normalized_target_rows_v1,
               channel_scale,
               channel_bias,
               letterbox_rgb,
               input_name,
               candidate_name,
               graph};
    return SACCADE_OK;
}

} // namespace saccade::model::directml

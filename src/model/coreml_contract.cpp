#include "model/coreml_contract.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace saccade::model::coreml {
namespace {

constexpr uint8_t magic[] = {'S', 'C', 'M', 'C'};
constexpr uint32_t maximum_locator_bytes = 256;
constexpr uint32_t maximum_feature_name_bytes = 64;

constexpr size_t version_offset = 4;
constexpr size_t input_kind_offset = 8;
constexpr size_t output_layout_offset = 12;
constexpr size_t candidate_capacity_offset = 16;
constexpr size_t target_capacity_offset = 20;
constexpr size_t minimum_confidence_offset = 24;
constexpr size_t iou_threshold_offset = 28;
constexpr size_t locator_size_offset = 32;
constexpr size_t input_name_size_offset = 36;
constexpr size_t target_rows_name_size_offset = 40;
constexpr size_t target_count_name_size_offset = 44;
constexpr size_t bundle_sha256_offset = 48;
constexpr size_t band_minimum_confidence_offset = 80;
constexpr size_t band_min_short_side_offset = 84;
constexpr size_t band_max_short_side_offset = 88;
constexpr size_t reserved_offset = 92;
constexpr size_t letterbox_rgb_offset = 96;
constexpr size_t preprocess_reserved_offset = 108;

uint32_t read_u32(const uint8_t* bytes, size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1U]) << 8U |
           static_cast<uint32_t>(bytes[offset + 2U]) << 16U | static_cast<uint32_t>(bytes[offset + 3U]) << 24U;
}

std::array<float, 3> read_float3(const uint8_t* bytes, size_t offset) noexcept {
    std::array<float, 3> output{};
    std::memcpy(output.data(), bytes + offset, sizeof(output));
    return output;
}

bool finite(const std::array<float, 3>& values) noexcept {
    return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

bool bounded_add(uint64_t left, uint64_t right, uint64_t maximum, uint64_t* output) noexcept {
    if (left > maximum || right > maximum - left) return false;
    *output = left + right;
    return true;
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
        (!is_alpha(name.data[0]) && name.data[0] != static_cast<uint8_t>('_'))) {
        return false;
    }
    for (size_t index = 1; index < name.size; ++index) {
        const uint8_t value = name.data[index];
        if (!is_alpha(value) && !is_digit(value) && value != static_cast<uint8_t>('_')) {
            return false;
        }
    }
    return true;
}

bool valid_locator(SaccadeSpanU8 locator) noexcept {
    constexpr char suffix[] = ".mlmodelc";
    if (locator.data == nullptr || locator.size == 0 || locator.size > maximum_locator_bytes ||
        locator.size < sizeof(suffix) - 1U ||
        std::memcmp(locator.data + locator.size - (sizeof(suffix) - 1U), suffix, sizeof(suffix) - 1U) != 0 ||
        locator.data[0] == static_cast<uint8_t>('/')) {
        return false;
    }
    size_t segment_start = 0;
    for (size_t index = 0; index <= locator.size; ++index) {
        if (index != locator.size && locator.data[index] != static_cast<uint8_t>('/')) {
            const uint8_t value = locator.data[index];
            if (!is_alpha(value) && !is_digit(value) && value != static_cast<uint8_t>('_') &&
                value != static_cast<uint8_t>('-') && value != static_cast<uint8_t>('.')) {
                return false;
            }
            continue;
        }
        const size_t segment_size = index - segment_start;
        if (segment_size == 0 || (segment_size == 1 && locator.data[segment_start] == static_cast<uint8_t>('.')) ||
            (segment_size == 2 && locator.data[segment_start] == static_cast<uint8_t>('.') &&
             locator.data[segment_start + 1U] == static_cast<uint8_t>('.'))) {
            return false;
        }
        segment_start = index + 1U;
    }
    return true;
}

float half_to_float(uint16_t value) noexcept {
    const uint32_t sign = static_cast<uint32_t>(value & UINT16_C(0x8000)) << 16U;
    const uint32_t exponent = (value >> 10U) & UINT16_C(0x1f);
    uint32_t fraction = value & UINT16_C(0x03ff);
    uint32_t bits = 0;
    if (exponent == 0) {
        if (fraction != 0) {
            uint32_t adjusted_exponent = 113;
            while ((fraction & UINT32_C(0x0400)) == 0) {
                fraction <<= 1U;
                --adjusted_exponent;
            }
            bits = sign | (adjusted_exponent << 23U) | ((fraction & UINT32_C(0x03ff)) << 13U);
        } else {
            bits = sign;
        }
    } else if (exponent == UINT32_C(0x1f)) {
        bits = sign | UINT32_C(0x7f800000) | (fraction << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (fraction << 13U);
    }
    float output = 0.0F;
    std::memcpy(&output, &bits, sizeof(output));
    return output;
}

bool scalar_at(const TargetRows& rows, uint32_t row, uint32_t column, float* output) noexcept {
    if (output == nullptr || row >= rows.row_count || column >= rows.column_count ||
        rows.row_stride < rows.column_count) {
        return false;
    }
    const uint64_t index = static_cast<uint64_t>(row) * rows.row_stride + column;
    if (rows.scalar_type == ScalarType::float16) {
        uint16_t value = 0;
        std::memcpy(&value, static_cast<const uint8_t*>(rows.values) + index * sizeof(value), sizeof(value));
        *output = half_to_float(value);
    } else if (rows.scalar_type == ScalarType::float32) {
        float value = 0.0F;
        std::memcpy(&value, static_cast<const uint8_t*>(rows.values) + index * sizeof(value), sizeof(value));
        *output = value;
    } else {
        return false;
    }
    return std::isfinite(*output);
}

float clamp_unit(float value) noexcept {
    return std::fmax(0.0F, std::fmin(1.0F, value));
}

bool q3_position(float unit, int32_t origin, int32_t extent, uint16_t* output) noexcept {
    const double scaled = (static_cast<double>(origin) + static_cast<double>(unit) * extent) * 8.0;
    const long rounded = std::lround(scaled);
    if (rounded < 0 || rounded > UINT16_MAX) return false;
    *output = static_cast<uint16_t>(rounded);
    return true;
}

bool q3_extent(float unit, int32_t extent, uint16_t* output) noexcept {
    const long rounded = std::lround(static_cast<double>(unit) * extent * 8.0);
    if (rounded <= 0 || rounded > UINT16_MAX) return false;
    *output = static_cast<uint16_t>(rounded);
    return true;
}

bool role_from_scalar(float value, uint8_t* output) noexcept {
    const long rounded = std::lround(value);
    if (rounded < SACCADE_TARGET_ROLE_UNKNOWN || rounded > SACCADE_TARGET_ROLE_WINDOW) {
        return false;
    }
    *output = static_cast<uint8_t>(rounded);
    return true;
}

} // namespace

SaccadeResult parse_contract(const ArtifactView& artifact, Contract* output) noexcept {
    if (output == nullptr || artifact.artifact != ArtifactKind::coreml_compiled_bundle ||
        (artifact.flags & artifact_relative_locator) == 0 ||
        (artifact.provider_compatibility_bits & provider_compatibility_bit) == 0 || artifact.payload.data == nullptr ||
        artifact.payload.size < payload_header_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    const uint8_t* data = artifact.payload.data;
    if (data[0] != magic[0] || data[1] != magic[1] || data[2] != magic[2] || data[3] != magic[3] ||
        read_u32(data, version_offset) != contract_version ||
        read_u32(data, input_kind_offset) != static_cast<uint32_t>(InputKind::image_bgra8) ||
        read_u32(data, output_layout_offset) != static_cast<uint32_t>(OutputLayout::normalized_target_rows_v1)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t candidate_capacity = read_u32(data, candidate_capacity_offset);
    const uint32_t target_capacity = read_u32(data, target_capacity_offset);
    const uint32_t minimum_confidence = read_u32(data, minimum_confidence_offset);
    const uint32_t iou_threshold = read_u32(data, iou_threshold_offset);
    const uint32_t band_minimum_confidence = read_u32(data, band_minimum_confidence_offset);
    const uint32_t band_min_short_side = read_u32(data, band_min_short_side_offset);
    const uint32_t band_max_short_side = read_u32(data, band_max_short_side_offset);
    const uint32_t locator_size = read_u32(data, locator_size_offset);
    const uint32_t input_name_size = read_u32(data, input_name_size_offset);
    const uint32_t target_rows_name_size = read_u32(data, target_rows_name_size_offset);
    const uint32_t target_count_name_size = read_u32(data, target_count_name_size_offset);
    const std::array<float, 3> letterbox_rgb = read_float3(data, letterbox_rgb_offset);
    uint64_t variable_size = 0;
    if (!bounded_add(locator_size, input_name_size, artifact.payload.size, &variable_size) ||
        !bounded_add(variable_size, target_rows_name_size, artifact.payload.size, &variable_size) ||
        !bounded_add(variable_size, target_count_name_size, artifact.payload.size, &variable_size) ||
        variable_size != artifact.payload.size - payload_header_bytes || candidate_capacity == 0 ||
        candidate_capacity > kernels::targets::maximum_candidates || target_capacity != artifact.max_targets ||
        target_capacity > candidate_capacity || minimum_confidence > UINT16_MAX ||
        band_minimum_confidence > UINT16_MAX || band_min_short_side > UINT16_MAX || band_max_short_side > UINT16_MAX ||
        iou_threshold > UINT16_MAX || !finite(letterbox_rgb) || read_u32(data, reserved_offset) != 0 ||
        read_u32(data, preprocess_reserved_offset) != 0) {
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
    const SaccadeSpanU8 locator{strings, locator_size};
    strings += locator_size;
    const SaccadeSpanU8 input_name{strings, input_name_size};
    strings += input_name_size;
    const SaccadeSpanU8 target_rows_name{strings, target_rows_name_size};
    strings += target_rows_name_size;
    const SaccadeSpanU8 target_count_name{strings, target_count_name_size};
    if (!valid_locator(locator) || !valid_feature_name(input_name) || !valid_feature_name(target_rows_name) ||
        !valid_feature_name(target_count_name)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Contract contract{candidate_capacity,
                      static_cast<uint16_t>(minimum_confidence),
                      static_cast<uint16_t>(band_minimum_confidence),
                      static_cast<uint16_t>(band_min_short_side),
                      static_cast<uint16_t>(band_max_short_side),
                      static_cast<uint16_t>(iou_threshold),
                      InputKind::image_bgra8,
                      OutputLayout::normalized_target_rows_v1};
    std::memcpy(contract.bundle_sha256.data(), data + bundle_sha256_offset, contract.bundle_sha256.size());
    contract.letterbox_rgb = letterbox_rgb;
    contract.locator = locator;
    contract.input_name = input_name;
    contract.target_rows_name = target_rows_name;
    contract.target_count_name = target_count_name;
    *output = contract;
    return SACCADE_OK;
}

SaccadeResult decode_target_rows(const Contract& contract, const TargetRows& rows, uint32_t candidate_count,
                                 SaccadeRectI32 scope, kernels::targets::DenseCandidate* output,
                                 uint32_t output_capacity, uint32_t* output_count) noexcept {
    if (output == nullptr || output_count == nullptr || rows.values == nullptr || rows.row_count < candidate_count ||
        rows.column_count != target_row_components || rows.row_stride < target_row_components ||
        candidate_count > contract.candidate_capacity || output_capacity < candidate_count ||
        (rows.scalar_type != ScalarType::float16 && rows.scalar_type != ScalarType::float32) || scope.x < 0 ||
        scope.y < 0 || scope.width <= 0 || scope.height <= 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output_count = 0;
    for (uint32_t row = 0; row < candidate_count; ++row) {
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
        float confidence = 0.0F;
        float role = 0.0F;
        if (!scalar_at(rows, row, 0, &x) || !scalar_at(rows, row, 1, &y) || !scalar_at(rows, row, 2, &width) ||
            !scalar_at(rows, row, 3, &height) || !scalar_at(rows, row, 4, &confidence) ||
            !scalar_at(rows, row, 5, &role)) {
            continue;
        }
        x = clamp_unit(x);
        y = clamp_unit(y);
        width = clamp_unit(width);
        height = clamp_unit(height);
        width = std::fmin(width, 1.0F - x);
        height = std::fmin(height, 1.0F - y);
        if (width <= 0.0F || height <= 0.0F) continue;
        kernels::targets::DenseCandidate candidate{};
        if (!q3_position(x, scope.x, scope.width, &candidate.x_q3) ||
            !q3_position(y, scope.y, scope.height, &candidate.y_q3) ||
            !q3_extent(width, scope.width, &candidate.width_q3) ||
            !q3_extent(height, scope.height, &candidate.height_q3) || !role_from_scalar(role, &candidate.role)) {
            continue;
        }
        candidate.confidence_q16 =
            static_cast<uint16_t>(std::lround(static_cast<double>(clamp_unit(confidence)) * UINT16_MAX));
        candidate.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        candidate.flags = SACCADE_TARGET_ACTIONABLE;
        output[(*output_count)++] = candidate;
    }
    return SACCADE_OK;
}

} // namespace saccade::model::coreml

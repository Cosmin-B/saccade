#include "model/artifact.hpp"
#include "model/coreml_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int { success, artifact_failed, contract_failed, decode_failed, malformed_rejection_failed };

constexpr size_t artifact_version_offset = 4;
constexpr size_t artifact_header_size_offset = 8;
constexpr size_t artifact_total_size_offset = 12;
constexpr size_t artifact_stable_id_offset = 16;
constexpr size_t artifact_graph_offset = 24;
constexpr size_t artifact_kind_offset = 28;
constexpr size_t artifact_precision_offset = 32;
constexpr size_t artifact_width_offset = 36;
constexpr size_t artifact_height_offset = 40;
constexpr size_t artifact_channels_offset = 44;
constexpr size_t artifact_maximum_targets_offset = 48;
constexpr size_t artifact_maximum_output_offset = 52;
constexpr size_t artifact_payload_offset_offset = 56;
constexpr size_t artifact_payload_size_offset = 64;
constexpr size_t artifact_flags_offset = 84;
constexpr size_t artifact_compatibility_offset = 88;

constexpr size_t contract_version_offset = 4;
constexpr size_t contract_input_kind_offset = 8;
constexpr size_t contract_output_layout_offset = 12;
constexpr size_t contract_candidate_capacity_offset = 16;
constexpr size_t contract_target_capacity_offset = 20;
constexpr size_t contract_minimum_confidence_offset = 24;
constexpr size_t contract_iou_threshold_offset = 28;
constexpr size_t contract_locator_size_offset = 32;
constexpr size_t contract_input_name_size_offset = 36;
constexpr size_t contract_rows_name_size_offset = 40;
constexpr size_t contract_count_name_size_offset = 44;
constexpr size_t contract_bundle_sha256_offset = 48;
constexpr size_t contract_band_minimum_confidence_offset = 80;
constexpr size_t contract_band_min_short_side_offset = 84;
constexpr size_t contract_band_max_short_side_offset = 88;
constexpr size_t contract_letterbox_rgb_offset = 96;

constexpr uint64_t stable_id = 901;
constexpr uint32_t input_width = 320;
constexpr uint32_t input_height = 320;
constexpr uint32_t maximum_targets = 8;
constexpr uint32_t candidate_capacity = 16;
constexpr uint16_t minimum_confidence_q16 = 4096;
constexpr uint16_t band_minimum_confidence_q16 = 3072;
constexpr uint16_t band_min_short_side_q3 = 96;
constexpr uint16_t band_max_short_side_q3 = 192;
constexpr uint16_t iou_threshold_q16 = 32768;
constexpr std::array<float, 3> letterbox_rgb{114.0F / 255.0F, 114.0F / 255.0F, 114.0F / 255.0F};
constexpr std::array<uint8_t, 32> bundle_sha256{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                                16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
constexpr std::array<uint8_t, 20> locator{'m', 'o', 'd', 'e', 'l', 's', '/', 'u', 'i', '_',
                                          'd', 'e', 't', 'e', 'c', 't', 'o', 'r', '.', 'c'};
constexpr std::array<uint8_t, 5> input_name{'i', 'm', 'a', 'g', 'e'};
constexpr std::array<uint8_t, 7> rows_name{'t', 'a', 'r', 'g', 'e', 't', 's'};
constexpr std::array<uint8_t, 5> count_name{'c', 'o', 'u', 'n', 't'};
constexpr size_t contract_size = saccade::model::coreml::payload_header_bytes + locator.size() + sizeof(".mlmodelc") -
                                 1U + input_name.size() + rows_name.size() + count_name.size();
constexpr size_t artifact_size = saccade::model::artifact_header_bytes + contract_size;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void write_u32(uint8_t* bytes, size_t offset, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void write_u64(uint8_t* bytes, size_t offset, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void write_float3(uint8_t* bytes, size_t offset, const std::array<float, 3>& values) noexcept {
    std::memcpy(bytes + offset, values.data(), sizeof(values));
}

std::array<uint8_t, artifact_size> artifact() noexcept {
    std::array<uint8_t, artifact_size> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = 'D';
    write_u32(bytes.data(), artifact_version_offset, saccade::model::artifact_version);
    write_u32(bytes.data(), artifact_header_size_offset, saccade::model::artifact_header_bytes);
    write_u32(bytes.data(), artifact_total_size_offset, static_cast<uint32_t>(bytes.size()));
    write_u64(bytes.data(), artifact_stable_id_offset, stable_id);
    write_u32(bytes.data(), artifact_graph_offset, static_cast<uint32_t>(saccade::model::GraphKind::ui_detector));
    write_u32(bytes.data(), artifact_kind_offset,
              static_cast<uint32_t>(saccade::model::ArtifactKind::coreml_compiled_bundle));
    write_u32(bytes.data(), artifact_precision_offset, SACCADE_PRECISION_FP16);
    write_u32(bytes.data(), artifact_width_offset, input_width);
    write_u32(bytes.data(), artifact_height_offset, input_height);
    write_u32(bytes.data(), artifact_channels_offset, 3);
    write_u32(bytes.data(), artifact_maximum_targets_offset, maximum_targets);
    write_u32(bytes.data(), artifact_maximum_output_offset,
              sizeof(SaccadeTargetPacketHeader) + maximum_targets * sizeof(SaccadeTargetRecord));
    write_u64(bytes.data(), artifact_payload_offset_offset, saccade::model::artifact_header_bytes);
    write_u64(bytes.data(), artifact_payload_size_offset, contract_size);
    write_u32(bytes.data(), artifact_flags_offset, saccade::model::artifact_relative_locator);
    write_u64(bytes.data(), artifact_compatibility_offset, saccade::model::coreml::provider_compatibility_bit);

    uint8_t* payload = bytes.data() + saccade::model::artifact_header_bytes;
    payload[0] = 'S';
    payload[1] = 'C';
    payload[2] = 'M';
    payload[3] = 'C';
    write_u32(payload, contract_version_offset, saccade::model::coreml::contract_version);
    write_u32(payload, contract_input_kind_offset,
              static_cast<uint32_t>(saccade::model::coreml::InputKind::image_bgra8));
    write_u32(payload, contract_output_layout_offset,
              static_cast<uint32_t>(saccade::model::coreml::OutputLayout::normalized_target_rows_v1));
    write_u32(payload, contract_candidate_capacity_offset, candidate_capacity);
    write_u32(payload, contract_target_capacity_offset, maximum_targets);
    write_u32(payload, contract_minimum_confidence_offset, minimum_confidence_q16);
    write_u32(payload, contract_iou_threshold_offset, iou_threshold_q16);
    write_u32(payload, contract_band_minimum_confidence_offset, band_minimum_confidence_q16);
    write_u32(payload, contract_band_min_short_side_offset, band_min_short_side_q3);
    write_u32(payload, contract_band_max_short_side_offset, band_max_short_side_q3);
    write_float3(payload, contract_letterbox_rgb_offset, letterbox_rgb);
    const std::array<uint8_t, sizeof(".mlmodelc") - 1U> suffix{'.', 'm', 'l', 'm', 'o', 'd', 'e', 'l', 'c'};
    write_u32(payload, contract_locator_size_offset, static_cast<uint32_t>(locator.size() + suffix.size()));
    write_u32(payload, contract_input_name_size_offset, static_cast<uint32_t>(input_name.size()));
    write_u32(payload, contract_rows_name_size_offset, static_cast<uint32_t>(rows_name.size()));
    write_u32(payload, contract_count_name_size_offset, static_cast<uint32_t>(count_name.size()));
    std::memcpy(payload + contract_bundle_sha256_offset, bundle_sha256.data(), bundle_sha256.size());
    uint8_t* strings = payload + saccade::model::coreml::payload_header_bytes;
    std::memcpy(strings, locator.data(), locator.size());
    strings += locator.size();
    std::memcpy(strings, suffix.data(), suffix.size());
    strings += suffix.size();
    std::memcpy(strings, input_name.data(), input_name.size());
    strings += input_name.size();
    std::memcpy(strings, rows_name.data(), rows_name.size());
    strings += rows_name.size();
    std::memcpy(strings, count_name.data(), count_name.size());
    return bytes;
}

} // namespace

int main() {
    auto bytes = artifact();
    saccade::model::ArtifactView artifact_view{};
    if (saccade::model::parse_artifact({bytes.data(), bytes.size()}, &artifact_view) != SACCADE_OK) {
        return result(TestResult::artifact_failed);
    }
    saccade::model::coreml::Contract contract{};
    if (saccade::model::coreml::parse_contract(artifact_view, &contract) != SACCADE_OK ||
        contract.candidate_capacity != candidate_capacity ||
        contract.minimum_confidence_q16 != minimum_confidence_q16 ||
        contract.band_minimum_confidence_q16 != band_minimum_confidence_q16 ||
        contract.band_min_short_side_q3 != band_min_short_side_q3 ||
        contract.band_max_short_side_q3 != band_max_short_side_q3 || contract.bundle_sha256 != bundle_sha256 ||
        contract.letterbox_rgb != letterbox_rgb || contract.locator.size != locator.size() + sizeof(".mlmodelc") - 1U) {
        return result(TestResult::contract_failed);
    }
    constexpr std::array<float, 12> rows{
        0.1F, 0.2F, 0.25F, 0.5F, 0.9F, static_cast<float>(SACCADE_TARGET_ROLE_BUTTON),
        0.9F, 0.9F, 0.5F,  0.2F, 0.8F, static_cast<float>(SACCADE_TARGET_ROLE_TEXT_FIELD)};
    std::array<saccade::kernels::targets::DenseCandidate, 2> candidates{};
    uint32_t candidate_count = 0;
    const saccade::model::coreml::TargetRows target_rows{rows.data(), saccade::model::coreml::ScalarType::float32, 2, 6,
                                                         6};
    if (saccade::model::coreml::decode_target_rows(contract, target_rows, 2, {0, 0, 1920, 1080}, candidates.data(),
                                                   static_cast<uint32_t>(candidates.size()),
                                                   &candidate_count) != SACCADE_OK ||
        candidate_count != 2 || candidates[0].x_q3 != 1536 || candidates[0].y_q3 != 1728 ||
        candidates[0].width_q3 != 3840 || candidates[0].height_q3 != 4320 ||
        candidates[0].role != SACCADE_TARGET_ROLE_BUTTON || candidates[1].x_q3 != 13824 ||
        candidates[1].width_q3 != 1536 || candidates[1].role != SACCADE_TARGET_ROLE_TEXT_FIELD) {
        return result(TestResult::decode_failed);
    }
    bytes = artifact();
    uint8_t* malformed =
        bytes.data() + saccade::model::artifact_header_bytes + saccade::model::coreml::payload_header_bytes;
    malformed[0] = static_cast<uint8_t>('/');
    if (saccade::model::parse_artifact({bytes.data(), bytes.size()}, &artifact_view) != SACCADE_OK ||
        saccade::model::coreml::parse_contract(artifact_view, &contract) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return result(TestResult::malformed_rejection_failed);
    }
    return result(TestResult::success);
}

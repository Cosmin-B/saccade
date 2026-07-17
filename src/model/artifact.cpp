#include "model/artifact.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace saccade::model {
namespace {

constexpr uint8_t magic[] = {'S', 'C', 'M', 'D'};
constexpr uint32_t valid_flag_mask = artifact_has_signature | artifact_relative_locator;
constexpr uint32_t maximum_input_extent = 16384;
constexpr uint32_t valid_channel_mask = UINT32_C(1) << 0 | UINT32_C(1) << 1 | UINT32_C(1) << 2;

constexpr size_t version_offset = 4;
constexpr size_t header_size_offset = 8;
constexpr size_t total_size_offset = 12;
constexpr size_t stable_id_offset = 16;
constexpr size_t graph_offset = 24;
constexpr size_t artifact_offset = 28;
constexpr size_t precision_offset = 32;
constexpr size_t width_offset = 36;
constexpr size_t height_offset = 40;
constexpr size_t channels_offset = 44;
constexpr size_t maximum_targets_offset = 48;
constexpr size_t maximum_output_offset = 52;
constexpr size_t payload_offset_offset = 56;
constexpr size_t payload_size_offset = 64;
constexpr size_t signature_offset_offset = 72;
constexpr size_t signature_size_offset = 80;
constexpr size_t flags_offset = 84;
constexpr size_t compatibility_offset = 88;

uint32_t read_u32(const uint8_t* bytes, size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1U]) << 8U |
           static_cast<uint32_t>(bytes[offset + 2U]) << 16U | static_cast<uint32_t>(bytes[offset + 3U]) << 24U;
}

uint64_t read_u64(const uint8_t* bytes, size_t offset) noexcept {
    uint64_t value = 0;
    for (uint32_t index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

bool graph_valid(uint32_t value) noexcept {
    return value == static_cast<uint32_t>(GraphKind::ui_detector);
}

bool artifact_valid(uint32_t value) noexcept {
    return value >= static_cast<uint32_t>(ArtifactKind::fixed_graph) &&
           value <= static_cast<uint32_t>(ArtifactKind::onnx);
}

bool bounds_valid(uint64_t offset, uint64_t size, uint64_t total) noexcept {
    return offset <= total && size <= total - offset;
}

bool channel_count_valid(uint32_t value) noexcept {
    return value != 0 && value <= 3 && (valid_channel_mask & (UINT32_C(1) << (value - 1U))) != 0;
}

} // namespace

SaccadeResult parse_artifact(SaccadeSpanU8 bytes, ArtifactView* output) noexcept {
    if (output == nullptr || bytes.data == nullptr || bytes.size < artifact_header_bytes)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    const uint8_t* data = bytes.data;
    if (data[0] != magic[0] || data[1] != magic[1] || data[2] != magic[2] || data[3] != magic[3] ||
        read_u32(data, version_offset) != artifact_version ||
        read_u32(data, header_size_offset) != artifact_header_bytes || read_u32(data, total_size_offset) != bytes.size)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const uint64_t stable_id = read_u64(data, stable_id_offset);
    const uint32_t graph = read_u32(data, graph_offset);
    const uint32_t artifact = read_u32(data, artifact_offset);
    const uint32_t precision_bits = read_u32(data, precision_offset);
    const uint32_t width = read_u32(data, width_offset);
    const uint32_t height = read_u32(data, height_offset);
    const uint32_t channels = read_u32(data, channels_offset);
    const uint32_t maximum_targets = read_u32(data, maximum_targets_offset);
    const uint32_t maximum_output = read_u32(data, maximum_output_offset);
    const uint64_t payload_offset = read_u64(data, payload_offset_offset);
    const uint64_t payload_size = read_u64(data, payload_size_offset);
    const uint64_t signature_offset = read_u64(data, signature_offset_offset);
    const uint32_t signature_size = read_u32(data, signature_size_offset);
    const uint32_t flags = read_u32(data, flags_offset);
    const uint64_t compatibility = read_u64(data, compatibility_offset);
    const uint64_t required_output =
        sizeof(SaccadeTargetPacketHeader) + static_cast<uint64_t>(maximum_targets) * sizeof(SaccadeTargetRecord);
    if (stable_id == 0 || !graph_valid(graph) || !artifact_valid(artifact) || precision_bits == 0 || width == 0 ||
        width > maximum_input_extent || height == 0 || height > maximum_input_extent ||
        !channel_count_valid(channels) || maximum_targets == 0 || maximum_targets > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        maximum_output < required_output || (flags & ~valid_flag_mask) != 0 || compatibility == 0 ||
        !bounds_valid(payload_offset, payload_size, bytes.size) || payload_offset < artifact_header_bytes ||
        payload_size == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const bool signed_artifact = (flags & artifact_has_signature) != 0;
    if (signed_artifact != (signature_size == artifact_signature_bytes) ||
        !bounds_valid(signature_offset, signature_size, bytes.size) ||
        (signed_artifact && signature_offset < payload_offset + payload_size) ||
        (signed_artifact && signature_offset + signature_size != bytes.size) ||
        (!signed_artifact &&
         (signature_offset != 0 || signature_size != 0 || payload_offset + payload_size != bytes.size))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {stable_id,
               static_cast<GraphKind>(graph),
               static_cast<ArtifactKind>(artifact),
               precision_bits,
               width,
               height,
               channels,
               maximum_targets,
               maximum_output,
               flags,
               compatibility,
               {data + payload_offset, static_cast<size_t>(payload_size)},
               signed_artifact ? SaccadeSpanU8{data, static_cast<size_t>(signature_offset)} : SaccadeSpanU8{},
               signed_artifact ? SaccadeSpanU8{data + signature_offset, static_cast<size_t>(signature_size)}
                               : SaccadeSpanU8{}};
    return SACCADE_OK;
}

SaccadeResult verify_artifact(const ArtifactView& artifact, ArtifactVerifier verifier) noexcept {
    if ((artifact.flags & artifact_has_signature) == 0 || artifact.signature.data == nullptr ||
        artifact.signature.size != artifact_signature_bytes || verifier.verify == nullptr)
        return SACCADE_ERROR_PERMISSION;
    return verifier.verify(verifier.context, artifact);
}

} // namespace saccade::model

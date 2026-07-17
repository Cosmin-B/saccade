#include "scene/packet.hpp"

#include <cstdint>
#include <limits>

namespace saccade::scene {
namespace {

constexpr uint16_t source_mask = SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_ACCESSIBILITY |
                                 SACCADE_TARGET_SOURCE_PIXEL | SACCADE_TARGET_SOURCE_GRID;
constexpr uint32_t target_flag_mask = SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_DISABLED | SACCADE_TARGET_OCCLUDED |
                                      SACCADE_TARGET_SECURE | SACCADE_TARGET_APPROXIMATE |
                                      SACCADE_TARGET_TEXT_REDACTED | SACCADE_TARGET_TEXT_TRUNCATED;
constexpr uint32_t target_capability_mask =
    SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_SCROLL |
    SACCADE_TARGET_CAPABILITY_DRAG_SOURCE | SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
    SACCADE_TARGET_CAPABILITY_INVOKE | SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE |
    SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
constexpr uint32_t packet_flag_mask = SACCADE_TARGET_PACKET_INCOMPLETE | SACCADE_TARGET_PACKET_TEXT_TRUNCATED;

bool aligned(const void* pointer, size_t alignment) noexcept {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1U)) == 0;
}

bool valid_coordinate_space(SaccadeCoordinateSpace value) noexcept {
    return value == SACCADE_COORDINATE_SPACE_MODEL_Q8 || value == SACCADE_COORDINATE_SPACE_SOURCE_Q8 ||
           value == SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
}

bool valid_target(const SaccadeTargetRecord& target) noexcept {
    if (target.width_q8 <= 0 || target.height_q8 <= 0 || target.confidence_q16 > UINT16_MAX ||
        target.role > SACCADE_TARGET_ROLE_WINDOW || (target.source_bits & static_cast<uint16_t>(~source_mask)) != 0 ||
        (target.capability_bits & ~target_capability_mask) != 0 || (target.flags & ~target_flag_mask) != 0 ||
        target.target_id == 0 || target.parent_id == target.target_id ||
        ((target.flags & SACCADE_TARGET_ACTIONABLE) != 0) != (target.capability_bits != 0) ||
        ((target.flags & (SACCADE_TARGET_DISABLED | SACCADE_TARGET_SECURE)) != 0 && target.capability_bits != 0) ||
        ((target.flags & SACCADE_TARGET_SECURE) != 0 && target.text.size != 0) ||
        ((target.flags & (SACCADE_TARGET_TEXT_REDACTED | SACCADE_TARGET_TEXT_TRUNCATED)) != 0 &&
         target.text.size != 0) ||
        ((target.flags & SACCADE_TARGET_TEXT_REDACTED) != 0 && (target.flags & SACCADE_TARGET_TEXT_TRUNCATED) != 0) ||
        (target.text.size == 0 && target.text.offset != 0)) {
        return false;
    }
    const int64_t right = static_cast<int64_t>(target.x_q8) + target.width_q8;
    const int64_t bottom = static_cast<int64_t>(target.y_q8) + target.height_q8;
    return target.safe_x_q8 >= target.x_q8 && target.safe_y_q8 >= target.y_q8 &&
           static_cast<int64_t>(target.safe_x_q8) < right && static_cast<int64_t>(target.safe_y_q8) < bottom;
}

} // namespace

bool valid_utf8(SaccadeSpanU8 bytes) noexcept {
    if (bytes.data == nullptr && bytes.size != 0) return false;
    size_t index = 0;
    while (index < bytes.size) {
        const uint8_t first = bytes.data[index++];
        if (first == 0) return false;
        if (first < 0x80U) continue;
        uint32_t value = 0;
        uint32_t minimum = 0;
        uint32_t continuation_count = 0;
        if ((first & 0xE0U) == 0xC0U) {
            value = first & 0x1FU;
            minimum = 0x80U;
            continuation_count = 1;
        } else if ((first & 0xF0U) == 0xE0U) {
            value = first & 0x0FU;
            minimum = 0x800U;
            continuation_count = 2;
        } else if ((first & 0xF8U) == 0xF0U) {
            value = first & 0x07U;
            minimum = 0x10000U;
            continuation_count = 3;
        } else {
            return false;
        }
        if (continuation_count > bytes.size - index) return false;
        for (uint32_t continuation = 0; continuation < continuation_count; ++continuation) {
            const uint8_t next = bytes.data[index++];
            if ((next & 0xC0U) != 0x80U) return false;
            value = (value << 6U) | (next & 0x3FU);
        }
        if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) return false;
    }
    return true;
}

SaccadeResult validate_packet(SaccadeSpanU8 bytes, PacketView* output) noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    if (bytes.data == nullptr || bytes.size < sizeof(SaccadeTargetPacketHeader) ||
        !aligned(bytes.data, alignof(SaccadeTargetPacketHeader))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const auto* header = reinterpret_cast<const SaccadeTargetPacketHeader*>(bytes.data);
    if (header->struct_size != sizeof(SaccadeTargetPacketHeader) ||
        header->packet_version != SACCADE_TARGET_PACKET_VERSION ||
        header->target_count > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        header->target_stride != sizeof(SaccadeTargetRecord) || (header->flags & ~packet_flag_mask) != 0 ||
        !valid_coordinate_space(header->coordinate_space) || header->frame_id == 0 || header->model_epoch == 0 ||
        header->session_epoch == 0 || header->transform_epoch == 0 || header->topology_epoch == 0 ||
        header->source_id == 0 || header->targets_offset != sizeof(SaccadeTargetPacketHeader) ||
        header->total_size > bytes.size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if ((header->coordinate_space == SACCADE_COORDINATE_SPACE_DESKTOP_Q8) != (header->scene_epoch != 0)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t target_bytes = static_cast<uint64_t>(header->target_count) * header->target_stride;
    if (target_bytes > std::numeric_limits<uint64_t>::max() - header->targets_offset) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t text_offset = header->targets_offset + target_bytes;
    if (text_offset > header->total_size || header->total_size - text_offset > SACCADE_TARGET_PACKET_MAX_TEXT_BYTES)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const auto* targets = reinterpret_cast<const SaccadeTargetRecord*>(bytes.data + header->targets_offset);
    const uint8_t* text = bytes.data + text_offset;
    uint32_t expected_text_offset = 0;
    for (uint32_t index = 0; index < header->target_count; ++index) {
        const SaccadeTargetRecord& target = targets[index];
        if (!valid_target(target) || (target.text.size != 0 && target.text.offset != expected_text_offset) ||
            target.text.size > header->total_size - text_offset - expected_text_offset ||
            !valid_utf8({text + target.text.offset, target.text.size}))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        expected_text_offset += target.text.size;
    }
    if (header->total_size != text_offset + expected_text_offset) return SACCADE_ERROR_INVALID_ARGUMENT;
    output->header = header;
    output->targets = targets;
    output->text = text;
    output->byte_size = static_cast<size_t>(header->total_size);
    output->text_size = expected_text_offset;
    return SACCADE_OK;
}

} // namespace saccade::scene

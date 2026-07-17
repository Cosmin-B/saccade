#include "scene/windows.hpp"

#include "scene/packet.hpp"

#include <cstdint>
#include <cstring>

namespace saccade::scene {
namespace {

bool q8(int32_t value, int32_t* output) noexcept {
    const int64_t scaled = static_cast<int64_t>(value) * 256;
    if (scaled < INT32_MIN || scaled > INT32_MAX) return false;
    *output = static_cast<int32_t>(scaled);
    return true;
}

} // namespace

SaccadeResult build_window_scene(const WindowSceneConfig& config, const SaccadeWindowInfo* windows, uint32_t count,
                                 SaccadeMutableSpanU8 output, size_t* byte_size) noexcept {
    if (byte_size == nullptr || windows == nullptr || count == 0 || count > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        config.scene_epoch == 0 || config.frame_id == 0 || config.model_epoch == 0 || config.session_epoch == 0 ||
        config.transform_epoch == 0 || config.topology_epoch == 0 || config.source_id == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t text_size = 0;
    uint32_t packet_flags = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const SaccadeSpanU8 title = windows[index].title;
        if (title.size > UINT16_MAX || !valid_utf8(title)) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (title.size <= SACCADE_TARGET_PACKET_MAX_TEXT_BYTES - text_size)
            text_size += static_cast<uint32_t>(title.size);
        else
            packet_flags |= SACCADE_TARGET_PACKET_TEXT_TRUNCATED;
    }
    const size_t required =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(count) * sizeof(SaccadeTargetRecord) + text_size;
    *byte_size = required;
    if (output.data == nullptr || output.size < required) return SACCADE_ERROR_CAPACITY;
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.flags = packet_flags;
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = config.scene_epoch;
    header.frame_id = config.frame_id;
    header.model_epoch = config.model_epoch;
    header.session_epoch = config.session_epoch;
    header.transform_epoch = config.transform_epoch;
    header.topology_epoch = config.topology_epoch;
    header.source_id = config.source_id;
    header.targets_offset = sizeof(header);
    header.total_size = required;
    std::memcpy(output.data, &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(output.data + sizeof(header));
    uint8_t* const text = reinterpret_cast<uint8_t*>(targets + count);
    uint32_t text_offset = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const SaccadeWindowInfo& window = windows[index];
        SaccadeTargetRecord target{};
        if (window.stable_id == 0 || window.process_id == 0 || window.desktop_bounds.width <= 0 ||
            window.desktop_bounds.height <= 0 || !q8(window.desktop_bounds.x, &target.x_q8) ||
            !q8(window.desktop_bounds.y, &target.y_q8) || !q8(window.desktop_bounds.width, &target.width_q8) ||
            !q8(window.desktop_bounds.height, &target.height_q8))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        target.target_id = window.stable_id;
        target.window_id = window.stable_id;
        target.safe_x_q8 = target.x_q8 + target.width_q8 / 2;
        target.safe_y_q8 = target.y_q8 + target.height_q8 / 2;
        target.confidence_q16 = UINT16_MAX;
        target.role = SACCADE_TARGET_ROLE_WINDOW;
        target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
        if (window.title.size != 0 && window.title.size <= text_size - text_offset) {
            target.text = {static_cast<uint16_t>(text_offset), static_cast<uint16_t>(window.title.size)};
            std::memcpy(text + text_offset, window.title.data, window.title.size);
            text_offset += static_cast<uint32_t>(window.title.size);
        } else if (window.title.size != 0) {
            target.flags |= SACCADE_TARGET_TEXT_TRUNCATED;
        }
        targets[index] = target;
    }
    return SACCADE_OK;
}

} // namespace saccade::scene

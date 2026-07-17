#include "scene/grid.hpp"

#include <cstdint>
#include <cstring>

namespace saccade::scene {
namespace {

constexpr uint64_t grid_target_prefix = UINT64_C(0x4752494400000000);
constexpr uint32_t grid_capabilities = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                       SACCADE_TARGET_CAPABILITY_SCROLL | SACCADE_TARGET_CAPABILITY_DRAG_SOURCE |
                                       SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
                                       SACCADE_TARGET_CAPABILITY_TEXT_SELECT;

bool config_valid(const GridSceneConfig& config) noexcept {
    const uint32_t count = static_cast<uint32_t>(config.rows) * config.columns;
    const int64_t horizontal_margins = static_cast<int64_t>(config.margin_x_q8) * 2;
    const int64_t vertical_margins = static_cast<int64_t>(config.margin_y_q8) * 2;
    return geometry::rect_valid(config.scope) && config.scene_epoch != 0 && config.frame_id != 0 &&
           config.model_epoch != 0 && config.session_epoch != 0 && config.transform_epoch != 0 &&
           config.topology_epoch != 0 && config.source_id != 0 && config.rows != 0 && config.columns != 0 &&
           count <= SACCADE_TARGET_PACKET_MAX_TARGETS && config.reserved0 == 0 && config.reserved1 == 0 &&
           config.margin_x_q8 >= 0 && config.margin_y_q8 >= 0 && horizontal_margins < config.scope.width &&
           vertical_margins < config.scope.height;
}

} // namespace

SaccadeResult build_grid_scene(const GridSceneConfig& config, SaccadeMutableSpanU8 output, size_t* byte_size) noexcept {
    if (byte_size == nullptr || !config_valid(config)) return SACCADE_ERROR_INVALID_ARGUMENT;
    const uint32_t target_count = static_cast<uint32_t>(config.rows) * config.columns;
    const size_t required =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(target_count) * sizeof(SaccadeTargetRecord);
    *byte_size = required;
    if (output.data == nullptr || output.size < required) return SACCADE_ERROR_CAPACITY;

    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
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
    const int64_t left = static_cast<int64_t>(config.scope.x) + config.margin_x_q8;
    const int64_t top = static_cast<int64_t>(config.scope.y) + config.margin_y_q8;
    const int64_t width = static_cast<int64_t>(config.scope.width) - static_cast<int64_t>(config.margin_x_q8) * 2;
    const int64_t height = static_cast<int64_t>(config.scope.height) - static_cast<int64_t>(config.margin_y_q8) * 2;
    uint32_t target_index = 0;
    for (uint32_t row = 0; row < config.rows; ++row) {
        const int64_t y0 = top + height * row / config.rows;
        const int64_t y1 = top + height * (row + 1U) / config.rows;
        for (uint32_t column = 0; column < config.columns; ++column) {
            const int64_t x0 = left + width * column / config.columns;
            const int64_t x1 = left + width * (column + 1U) / config.columns;
            SaccadeTargetRecord target{};
            target.target_id = grid_target_prefix | (target_index + 1U);
            target.display_id = config.display_id;
            target.x_q8 = static_cast<int32_t>(x0);
            target.y_q8 = static_cast<int32_t>(y0);
            target.width_q8 = static_cast<int32_t>(x1 - x0);
            target.height_q8 = static_cast<int32_t>(y1 - y0);
            target.safe_x_q8 = static_cast<int32_t>(x0 + (x1 - x0) / 2);
            target.safe_y_q8 = static_cast<int32_t>(y0 + (y1 - y0) / 2);
            target.confidence_q16 = UINT16_MAX;
            target.role = SACCADE_TARGET_ROLE_UNKNOWN;
            target.source_bits = SACCADE_TARGET_SOURCE_GRID;
            target.capability_bits = grid_capabilities;
            target.flags = SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_APPROXIMATE;
            target.order = target_index;
            targets[target_index++] = target;
        }
    }
    return SACCADE_OK;
}

} // namespace saccade::scene

#include "scene/grid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

enum class TestResult : int {
    success = 0,
    build_failed,
    packet_invalid,
    geometry_invalid,
    capacity_contract_invalid,
    admission_invalid
};

constexpr size_t packet_capacity = sizeof(SaccadeTargetPacketHeader) + 12U * sizeof(SaccadeTargetRecord);

} // namespace

int main() {
    alignas(SaccadeTargetPacketHeader) std::array<uint8_t, packet_capacity> bytes{};
    saccade::scene::GridSceneConfig config{};
    config.scope = {-1920 * 256, -200 * 256, 3840 * 256, 2160 * 256};
    config.scene_epoch = 1;
    config.frame_id = 2;
    config.model_epoch = 3;
    config.session_epoch = 4;
    config.transform_epoch = 5;
    config.topology_epoch = 6;
    config.source_id = 7;
    config.rows = 3;
    config.columns = 4;
    config.margin_x_q8 = 8 * 256;
    config.margin_y_q8 = 10 * 256;
    size_t byte_size = 0;
    if (saccade::scene::build_grid_scene(config, {bytes.data(), bytes.size()}, &byte_size) != SACCADE_OK ||
        byte_size != bytes.size())
        return static_cast<int>(TestResult::build_failed);
    saccade::scene::PacketView packet{};
    if (saccade::scene::validate_packet({bytes.data(), byte_size}, &packet) != SACCADE_OK ||
        packet.header->target_count != 12 || packet.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8)
        return static_cast<int>(TestResult::packet_invalid);
    const SaccadeTargetRecord& first = packet.targets[0];
    const SaccadeTargetRecord& last = packet.targets[11];
    if (first.x_q8 != config.scope.x + config.margin_x_q8 || first.y_q8 != config.scope.y + config.margin_y_q8 ||
        first.target_id == last.target_id || first.order != 0 || last.order != 11 ||
        first.source_bits != SACCADE_TARGET_SOURCE_GRID || (first.flags & SACCADE_TARGET_ACTIONABLE) == 0 ||
        last.safe_x_q8 < last.x_q8 || last.safe_y_q8 < last.y_q8 || last.safe_x_q8 >= last.x_q8 + last.width_q8 ||
        last.safe_y_q8 >= last.y_q8 + last.height_q8)
        return static_cast<int>(TestResult::geometry_invalid);
    size_t required = 0;
    if (saccade::scene::build_grid_scene(config, {}, &required) != SACCADE_ERROR_CAPACITY ||
        required != packet_capacity)
        return static_cast<int>(TestResult::capacity_contract_invalid);
    config.rows = 101;
    config.columns = 100;
    if (saccade::scene::build_grid_scene(config, {bytes.data(), bytes.size()}, &required) !=
        SACCADE_ERROR_INVALID_ARGUMENT)
        return static_cast<int>(TestResult::admission_invalid);
    return static_cast<int>(TestResult::success);
}

#include "application/overlay_composer.hpp"
#include "geometry/display_catalog.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    transform_failed,
    scene_invalid,
    compose_failed,
    packet_invalid,
    mapping_failed,
    geometry_failed,
    label_collision_failed,
    long_label_failed,
    unsupported_glyph_failed,
    compact_capacity_failed
};

constexpr uint64_t primary_display_id = 100;
constexpr uint64_t secondary_display_id = 200;
constexpr uint64_t scene_epoch = 31;
constexpr uint64_t frame_id = 32;
constexpr uint64_t model_epoch = 33;
constexpr uint64_t session_epoch = 34;
constexpr uint64_t transform_epoch = 35;
constexpr uint64_t topology_epoch = 36;
constexpr uint64_t source_id = 37;
constexpr uint32_t scene_target_count = 4;
constexpr uint32_t expected_overlay_targets = 3;
constexpr int32_t desktop_width = 1920;
constexpr int32_t desktop_height = 1080;
constexpr uint32_t backing_width = 3840;
constexpr uint32_t backing_height = 2160;
constexpr int32_t coordinate_scale = 256;
constexpr int32_t q3_scale = 8;
constexpr uint32_t filtered_scene_index = 2;
constexpr uint32_t clipped_scene_index = 3;
constexpr uint32_t visible_label_index = 1;
constexpr uint32_t expected_clipped_x_q3 = 3800U * q3_scale;
constexpr uint32_t expected_clipped_width_q3 = 40U * q3_scale;
constexpr std::array<uint16_t, SACCADE_OVERLAY_GLYPHS_PER_TARGET> glyph_symbols{
    static_cast<uint16_t>('A'), static_cast<uint16_t>('S'), static_cast<uint16_t>('D'), static_cast<uint16_t>('F'),
    static_cast<uint16_t>('G'), static_cast<uint16_t>('H'), static_cast<uint16_t>('J'), static_cast<uint16_t>('K'),
    static_cast<uint16_t>('L'), static_cast<uint16_t>('Q'), static_cast<uint16_t>('W'), static_cast<uint16_t>('E'),
    static_cast<uint16_t>('R'), static_cast<uint16_t>('T'), static_cast<uint16_t>('Y'), static_cast<uint16_t>('U')};
constexpr size_t scene_packet_size =
    sizeof(SaccadeTargetPacketHeader) + scene_target_count * sizeof(SaccadeTargetRecord);
constexpr size_t overlay_packet_capacity = sizeof(SaccadeOverlayPacketHeader) +
                                           scene_target_count * sizeof(SaccadeOverlayTarget) +
                                           sizeof(SaccadeOverlayStyle);

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct alignas(SaccadeTargetPacketHeader) ScenePacket {
    std::array<uint8_t, scene_packet_size> bytes{};
};

SaccadeSpanU8 make_scene(ScenePacket* packet) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = scene_target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = scene_epoch;
    header.frame_id = frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.source_id = source_id;
    header.targets_offset = sizeof(header);
    header.total_size = packet->bytes.size();
    std::memcpy(packet->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(packet->bytes.data() + sizeof(header));
    constexpr std::array<int32_t, scene_target_count> x_positions{100, 105, 100, 1900};
    constexpr std::array<int32_t, scene_target_count> widths{100, 100, 100, 100};
    for (uint32_t index = 0; index < scene_target_count; ++index) {
        SaccadeTargetRecord& target = targets[index];
        target.target_id = index + 1U;
        target.display_id = index == filtered_scene_index ? secondary_display_id : primary_display_id;
        target.x_q8 = x_positions[index] * coordinate_scale;
        target.y_q8 = 100 * coordinate_scale;
        target.width_q8 = widths[index] * coordinate_scale;
        target.height_q8 = 40 * coordinate_scale;
        target.safe_x_q8 = target.x_q8 + target.width_q8 / 2;
        target.safe_y_q8 = target.y_q8 + target.height_q8 / 2;
        target.confidence_q16 = UINT16_MAX;
        target.role = SACCADE_TARGET_ROLE_BUTTON;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }
    return {packet->bytes.data(), packet->bytes.size()};
}

SaccadeOverlayStyle style() noexcept {
    SaccadeOverlayStyle value{};
    value.target_outline_rgba8 = UINT32_C(0xffffffff);
    value.label_background_rgba8 = UINT32_C(0x000000e0);
    value.label_foreground_rgba8 = UINT32_C(0xffffffff);
    value.active_fill_rgba8 = UINT32_C(0x00ff0060);
    value.active_outline_rgba8 = UINT32_C(0xffffffff);
    value.target_stroke_q3 = 8;
    value.target_radius_q3 = 24;
    value.label_height_q3 = 160;
    value.label_radius_q3 = 24;
    value.label_padding_x_q3 = 32;
    value.glyph_width_q3 = 64;
    value.glyph_height_q3 = 112;
    value.glyph_advance_q3 = 72;
    value.active_stroke_q3 = 16;
    return value;
}

bool overlaps(const SaccadeOverlayTarget& left, const SaccadeOverlayTarget& right,
              const SaccadeOverlayStyle& value) noexcept {
    const uint32_t left_width = value.label_padding_x_q3 * 2U + value.glyph_advance_q3 * left.glyph_count;
    const uint32_t right_width = value.label_padding_x_q3 * 2U + value.glyph_advance_q3 * right.glyph_count;
    return left.label_x_q3 < right.label_x_q3 + right_width && right.label_x_q3 < left.label_x_q3 + left_width &&
           left.label_y_q3 < right.label_y_q3 + value.label_height_q3 &&
           right.label_y_q3 < left.label_y_q3 + value.label_height_q3;
}

} // namespace

int main() {
    saccade::geometry::DisplaySurface display{};
    display.display_id = primary_display_id;
    display.desktop_bounds = {0, 0, desktop_width * coordinate_scale, desktop_height * coordinate_scale};
    display.work_bounds = display.desktop_bounds;
    display.backing_width = backing_width;
    display.backing_height = backing_height;
    display.maximum_fps = 120;
    display.flags = saccade::geometry::display_surface_main | saccade::geometry::display_surface_active;
    saccade::geometry::CoordinateTransform transform;
    if (saccade::geometry::make_desktop_to_surface_transform(display, transform_epoch, &transform) != SACCADE_OK)
        return result(TestResult::transform_failed);

    static ScenePacket scene_packet;
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet(make_scene(&scene_packet), &scene) != SACCADE_OK)
        return result(TestResult::scene_invalid);
    std::array<saccade::interaction::HintLabel, scene_target_count> labels{};
    constexpr std::array<uint32_t, scene_target_count> label_order{2, 0, 3, 1};
    for (uint32_t index = 0; index < labels.size(); ++index) {
        const uint32_t scene_index = label_order[index];
        labels[index].target_id = scene.targets[scene_index].target_id;
        labels[index].target_index = scene_index;
        labels[index].symbol_count = 1;
        labels[index].symbols[0] = glyph_symbols[scene_index];
    }
    const SaccadeOverlayStyle packet_style = style();
    saccade::application::OverlayComposeConfig config{};
    config.display_id = primary_display_id;
    config.transform_epoch = transform_epoch;
    config.desktop_to_surface = &transform;
    config.styles = &packet_style;
    config.style_count = 1;
    config.glyph_symbols = glyph_symbols.data();
    config.glyph_symbol_count = static_cast<uint32_t>(glyph_symbols.size());
    config.active_target_id = scene.targets[1].target_id;

    static saccade::application::OverlayComposeWorkspace workspace;
    alignas(SaccadeOverlayPacketHeader) std::array<uint8_t, overlay_packet_capacity> output{};
    saccade::application::OverlayComposeResult composed{};
    saccade::application::OverlayComposer composer;
    if (composer.compose(scene, labels.data(), static_cast<uint32_t>(labels.size()), config, &workspace,
                         {output.data(), output.size()}, &composed) != SACCADE_OK ||
        composed.target_count != expected_overlay_targets)
        return result(TestResult::compose_failed);
    saccade::overlay::PacketView packet{};
    if (saccade::overlay::validate_packet({output.data(), composed.byte_size}, &packet) != SACCADE_OK)
        return result(TestResult::packet_invalid);
    if (workspace.overlay_index_by_scene_index[filtered_scene_index] != UINT32_MAX ||
        workspace.overlay_index_by_scene_index[clipped_scene_index] == UINT32_MAX)
        return result(TestResult::mapping_failed);

    constexpr size_t compact_capacity = sizeof(SaccadeOverlayPacketHeader) +
                                        expected_overlay_targets * sizeof(SaccadeOverlayTarget) +
                                        sizeof(SaccadeOverlayStyle);
    alignas(SaccadeOverlayPacketHeader) std::array<uint8_t, compact_capacity> compact_output{};
    if (composer.compose(scene, labels.data(), static_cast<uint32_t>(labels.size()), config, &workspace,
                         {compact_output.data(), compact_output.size()}, &composed) != SACCADE_OK ||
        composed.target_count != expected_overlay_targets)
        return result(TestResult::compact_capacity_failed);

    const auto* targets = reinterpret_cast<const SaccadeOverlayTarget*>(packet.targets);
    const SaccadeOverlayTarget& clipped = targets[workspace.overlay_index_by_scene_index[clipped_scene_index]];
    if (clipped.x_q3 != expected_clipped_x_q3 || clipped.width_q3 != expected_clipped_width_q3)
        return result(TestResult::geometry_failed);
    const uint32_t first = workspace.overlay_index_by_scene_index[0];
    const uint32_t second = workspace.overlay_index_by_scene_index[1];
    if (first == UINT32_MAX || second == UINT32_MAX || composed.active_target_index != second ||
        overlaps(targets[first], targets[second], packet_style) || composer.stats().labels_repositioned == 0)
        return result(TestResult::label_collision_failed);

    labels[visible_label_index].symbol_count = SACCADE_OVERLAY_GLYPHS_PER_TARGET;
    for (uint32_t index = 0; index < SACCADE_OVERLAY_GLYPHS_PER_TARGET; ++index) {
        labels[visible_label_index].symbols[index] = glyph_symbols[index];
    }
    if (composer.compose(scene, labels.data(), static_cast<uint32_t>(labels.size()), config, &workspace,
                         {output.data(), output.size()}, &composed) != SACCADE_OK ||
        saccade::overlay::validate_packet({output.data(), composed.byte_size}, &packet) != SACCADE_OK) {
        return result(TestResult::long_label_failed);
    }
    targets = reinterpret_cast<const SaccadeOverlayTarget*>(packet.targets);
    const uint32_t long_label_target = workspace.overlay_index_by_scene_index[labels[visible_label_index].target_index];
    if (long_label_target == UINT32_MAX ||
        targets[long_label_target].glyph_count != SACCADE_OVERLAY_GLYPHS_PER_TARGET) {
        return result(TestResult::long_label_failed);
    }

    labels[visible_label_index].symbol_count = 1;
    labels[visible_label_index].symbols[0] = static_cast<uint16_t>('X');
    if (composer.compose(scene, labels.data(), static_cast<uint32_t>(labels.size()), config, &workspace,
                         {output.data(), output.size()}, &composed) != SACCADE_ERROR_UNSUPPORTED)
        return result(TestResult::unsupported_glyph_failed);
    return result(TestResult::success);
}

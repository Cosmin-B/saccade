#include "../support/allocation_tracker.hpp"
#include "overlay/packet.hpp"

#include <saccade/saccade_overlay.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace {

enum class ExitCode : int {
    success = 0,
    packed_max = 1,
    allocation_tracker = 2,
    packet_view = 3,
    undersized_output = 4,
    static_output = 5,
    outline_rects = 6,
    outline_metadata = 7,
    label = 8,
    active = 9,
    unaligned = 10,
    overflow = 11,
    glyph_count = 12,
    invalid_style = 13,
    animated_style = 14,
    unknown_style = 15,
    trailing_bytes = 16,
    tiny_target = 17,
    empty_active = 18,
    repeated_validation = 19,
    allocation = 20,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint16_t q3(uint16_t pixels) noexcept {
    return static_cast<uint16_t>(pixels * UINT16_C(8));
}

template <class Record, size_t Size>
void store(std::array<uint8_t, Size>& bytes, size_t offset, const Record& record) noexcept {
    std::memcpy(bytes.data() + offset, &record, sizeof(record));
}

bool is_rect(const SaccadeOverlayRect& rect, uint16_t x_q3, uint16_t y_q3, uint16_t width_q3,
             uint16_t height_q3) noexcept {
    return rect.x_q3 == x_q3 && rect.y_q3 == y_q3 && rect.width_q3 == width_q3 && rect.height_q3 == height_q3;
}

} // namespace

int main() {
    using saccade::overlay::ExpandedInstanceSpan;
    using saccade::overlay::PacketView;

    static_assert(std::is_trivially_copyable_v<SaccadeOverlayPacketHeader>);
    static_assert(std::is_trivially_copyable_v<SaccadeOverlayTarget>);
    static_assert(std::is_trivially_copyable_v<SaccadeOverlayStyle>);
    static_assert(std::is_trivially_copyable_v<SaccadeOverlayRect>);
    static_assert(sizeof(SaccadeOverlayPacketHeader) == 64);
    static_assert(sizeof(SaccadeOverlayTarget) == 48);
    static_assert(sizeof(SaccadeOverlayStyle) == 64);
    static_assert(sizeof(SaccadeOverlayRect) == 8);
    static_assert(sizeof(SaccadeOverlayInstanceMeta) == 4);

    const SaccadeOverlayInstanceMeta packed_max = saccade_overlay_instance_meta_make(
        SACCADE_OVERLAY_MAX_TARGETS - 1U, SACCADE_OVERLAY_MAX_STYLES - 1U, SACCADE_OVERLAY_INSTANCE_ACTIVE);
    if (saccade_overlay_instance_meta_target(packed_max) != SACCADE_OVERLAY_MAX_TARGETS - 1U ||
        saccade_overlay_instance_meta_style(packed_max) != SACCADE_OVERLAY_MAX_STYLES - 1U ||
        saccade_overlay_instance_meta_kind(packed_max) != SACCADE_OVERLAY_INSTANCE_ACTIVE) {
        return to_process_exit_code(ExitCode::packed_max);
    }

    if (!saccade::test::allocation_tracker_self_test()) {
        return to_process_exit_code(ExitCode::allocation_tracker);
    }

    constexpr size_t target_offset = sizeof(SaccadeOverlayPacketHeader);
    constexpr size_t style_offset = 112;
    constexpr size_t packet_size = style_offset + sizeof(SaccadeOverlayStyle);
    alignas(16) std::array<uint8_t, packet_size> packet_bytes{};

    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = 1;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.reserved32 = 0;
    header.scene_epoch = UINT64_C(17);
    header.transform_epoch = UINT64_C(9);
    header.targets_offset = target_offset;
    header.styles_offset = style_offset;

    SaccadeOverlayTarget target{};
    target.target_id = UINT64_C(0x1234);
    target.x_q3 = q3(100);
    target.y_q3 = q3(200);
    target.width_q3 = q3(80);
    target.height_q3 = q3(24);
    target.label_x_q3 = q3(100);
    target.label_y_q3 = q3(188);
    target.confidence_q16 = UINT16_C(60000);
    target.style_index = 0;
    target.glyph_count = 3;
    target.glyphs[0] = 4;
    target.glyphs[1] = 2;
    target.glyphs[2] = 7;
    target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;

    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = UINT32_C(0xFFE14BFF);
    style.label_background_rgba8 = UINT32_C(0x101318F2);
    style.label_foreground_rgba8 = UINT32_C(0xFFFFFFFF);
    style.active_fill_rgba8 = UINT32_C(0xFFE14B26);
    style.active_outline_rgba8 = UINT32_C(0xFFFFFFFF);
    style.target_stroke_q3 = q3(2);
    style.target_radius_q3 = q3(3);
    style.label_height_q3 = q3(12);
    style.label_radius_q3 = q3(2);
    style.label_padding_x_q3 = q3(2);
    style.glyph_width_q3 = q3(5);
    style.glyph_height_q3 = q3(7);
    style.glyph_advance_q3 = q3(6);
    style.active_stroke_q3 = q3(2);

    store(packet_bytes, 0, header);
    store(packet_bytes, target_offset, target);
    store(packet_bytes, style_offset, style);

    const SaccadeSpanU8 packet{packet_bytes.data(), packet_bytes.size()};
    PacketView view{};
    if (saccade::overlay::validate_packet(packet, &view) != SACCADE_OK || view.header.target_count != 1 ||
        view.header.scene_epoch != UINT64_C(17) || view.targets != packet.data + target_offset ||
        view.styles != packet.data + style_offset) {
        return to_process_exit_code(ExitCode::packet_view);
    }

    std::array<SaccadeOverlayRect, 4> undersized_rects{};
    std::array<SaccadeOverlayInstanceMeta, 4> undersized_metadata{};
    std::memset(undersized_rects.data(), 0xA5, sizeof(undersized_rects));
    std::memset(undersized_metadata.data(), 0x5A, sizeof(undersized_metadata));
    const auto before_rects = undersized_rects;
    const auto before_metadata = undersized_metadata;
    size_t static_count = 0;
    if (saccade::overlay::expand_static(
            view, ExpandedInstanceSpan{undersized_rects.data(), undersized_metadata.data(), undersized_rects.size()},
            &static_count) != SACCADE_ERROR_CAPACITY ||
        static_count != 5 || std::memcmp(undersized_rects.data(), before_rects.data(), sizeof(undersized_rects)) != 0 ||
        std::memcmp(undersized_metadata.data(), before_metadata.data(), sizeof(undersized_metadata)) != 0) {
        return to_process_exit_code(ExitCode::undersized_output);
    }

    std::array<SaccadeOverlayRect, 5> rects{};
    std::array<SaccadeOverlayInstanceMeta, 5> metadata{};
    if (saccade::overlay::expand_static(view, ExpandedInstanceSpan{rects.data(), metadata.data(), rects.size()},
                                        &static_count) != SACCADE_OK ||
        static_count != rects.size()) {
        return to_process_exit_code(ExitCode::static_output);
    }

    const uint16_t x = target.x_q3;
    const uint16_t y = target.y_q3;
    const uint16_t stroke = style.target_stroke_q3;
    if (!is_rect(rects[0], x, y, target.width_q3, stroke) ||
        !is_rect(rects[1], x, static_cast<uint16_t>(y + target.height_q3 - stroke), target.width_q3, stroke) ||
        !is_rect(rects[2], x, y, stroke, target.height_q3) ||
        !is_rect(rects[3], static_cast<uint16_t>(x + target.width_q3 - stroke), y, stroke, target.height_q3)) {
        return to_process_exit_code(ExitCode::outline_rects);
    }
    for (size_t index = 0; index < 4; ++index) {
        if (saccade_overlay_instance_meta_kind(metadata[index]) != SACCADE_OVERLAY_INSTANCE_OUTLINE ||
            saccade_overlay_instance_meta_target(metadata[index]) != 0 ||
            saccade_overlay_instance_meta_style(metadata[index]) != 0) {
            return to_process_exit_code(ExitCode::outline_metadata);
        }
    }

    const uint16_t label_width =
        static_cast<uint16_t>(static_cast<uint32_t>(style.label_padding_x_q3) * 2U +
                              static_cast<uint32_t>(style.glyph_advance_q3) * target.glyph_count);
    if (!is_rect(rects[4], target.label_x_q3, target.label_y_q3, label_width, style.label_height_q3) ||
        saccade_overlay_instance_meta_kind(metadata[4]) != SACCADE_OVERLAY_INSTANCE_LABEL ||
        saccade_overlay_instance_meta_target(metadata[4]) != 0 ||
        saccade_overlay_instance_meta_style(metadata[4]) != 0) {
        return to_process_exit_code(ExitCode::label);
    }

    std::array<SaccadeOverlayRect, 1> active_rects{};
    std::array<SaccadeOverlayInstanceMeta, 1> active_metadata{};
    size_t active_count = 0;
    if (saccade::overlay::expand_active(
            view, 0, ExpandedInstanceSpan{active_rects.data(), active_metadata.data(), active_rects.size()},
            &active_count) != SACCADE_OK ||
        active_count != 1 || !is_rect(active_rects[0], x, y, target.width_q3, target.height_q3) ||
        saccade_overlay_instance_meta_kind(active_metadata[0]) != SACCADE_OVERLAY_INSTANCE_ACTIVE ||
        saccade_overlay_instance_meta_target(active_metadata[0]) != 0 ||
        saccade_overlay_instance_meta_style(active_metadata[0]) != 0) {
        return to_process_exit_code(ExitCode::active);
    }

    const auto rejected = [](const auto& bytes) noexcept {
        PacketView rejected_view{};
        const SaccadeSpanU8 rejected_packet{bytes.data(), bytes.size()};
        return saccade::overlay::validate_packet(rejected_packet, &rejected_view) == SACCADE_ERROR_INVALID_ARGUMENT;
    };

    auto unaligned_bytes = packet_bytes;
    SaccadeOverlayPacketHeader unaligned_header = header;
    unaligned_header.targets_offset += 2;
    store(unaligned_bytes, 0, unaligned_header);
    if (!rejected(unaligned_bytes)) {
        return to_process_exit_code(ExitCode::unaligned);
    }

    auto overflow_bytes = packet_bytes;
    SaccadeOverlayTarget overflow_target = target;
    overflow_target.x_q3 = UINT16_MAX - UINT16_C(7);
    overflow_target.width_q3 = UINT16_C(16);
    store(overflow_bytes, target_offset, overflow_target);
    if (!rejected(overflow_bytes)) {
        return to_process_exit_code(ExitCode::overflow);
    }

    auto glyph_bytes = packet_bytes;
    SaccadeOverlayTarget glyph_target = target;
    glyph_target.glyph_count = SACCADE_OVERLAY_GLYPHS_PER_TARGET + 1U;
    store(glyph_bytes, target_offset, glyph_target);
    if (!rejected(glyph_bytes)) {
        return to_process_exit_code(ExitCode::glyph_count);
    }

    auto invalid_style_bytes = packet_bytes;
    SaccadeOverlayStyle invalid_style = style;
    invalid_style.glyph_advance_q3 = invalid_style.glyph_width_q3 - 1U;
    store(invalid_style_bytes, style_offset, invalid_style);
    if (!rejected(invalid_style_bytes)) {
        return to_process_exit_code(ExitCode::invalid_style);
    }

    auto animated_style_bytes = packet_bytes;
    SaccadeOverlayStyle animated_style = style;
    animated_style.flags = SACCADE_OVERLAY_STYLE_ANIMATED;
    store(animated_style_bytes, style_offset, animated_style);
    PacketView animated_view{};
    if (saccade::overlay::validate_packet({animated_style_bytes.data(), animated_style_bytes.size()}, &animated_view) !=
        SACCADE_OK) {
        return to_process_exit_code(ExitCode::animated_style);
    }

    auto unknown_style_bytes = packet_bytes;
    SaccadeOverlayStyle unknown_style = style;
    unknown_style.flags = UINT32_C(0x2);
    store(unknown_style_bytes, style_offset, unknown_style);
    if (!rejected(unknown_style_bytes)) {
        return to_process_exit_code(ExitCode::unknown_style);
    }

    std::array<uint8_t, packet_size + 1> trailing_bytes{};
    std::memcpy(trailing_bytes.data(), packet_bytes.data(), packet_bytes.size());
    if (!rejected(trailing_bytes)) {
        return to_process_exit_code(ExitCode::trailing_bytes);
    }

    auto tiny_bytes = packet_bytes;
    SaccadeOverlayTarget tiny_target = target;
    tiny_target.width_q3 = 1;
    tiny_target.height_q3 = 1;
    store(tiny_bytes, target_offset, tiny_target);
    if (!rejected(tiny_bytes)) {
        return to_process_exit_code(ExitCode::tiny_target);
    }

    active_count = 99;
    if (saccade::overlay::expand_active(view, SACCADE_OVERLAY_ACTIVE_TARGET_NONE, ExpandedInstanceSpan{},
                                        &active_count) != SACCADE_OK ||
        active_count != 0) {
        return to_process_exit_code(ExitCode::empty_active);
    }

    saccade::test::begin_allocation_tracking();
    for (size_t repeat = 0; repeat < 1000; ++repeat) {
        PacketView repeated_view{};
        if (saccade::overlay::validate_packet(packet, &repeated_view) != SACCADE_OK ||
            saccade::overlay::expand_static(repeated_view,
                                            ExpandedInstanceSpan{rects.data(), metadata.data(), rects.size()},
                                            &static_count) != SACCADE_OK ||
            saccade::overlay::expand_active(
                repeated_view, 0,
                ExpandedInstanceSpan{active_rects.data(), active_metadata.data(), active_rects.size()},
                &active_count) != SACCADE_OK) {
            saccade::test::end_allocation_tracking();
            return to_process_exit_code(ExitCode::repeated_validation);
        }
    }
    if (saccade::test::end_allocation_tracking() != 0) {
        return to_process_exit_code(ExitCode::allocation);
    }

    return to_process_exit_code(ExitCode::success);
}

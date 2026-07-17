#include "overlay/packet.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace saccade::overlay {
namespace {

constexpr size_t instances_per_target = 5;
constexpr uint32_t style_flag_mask = SACCADE_OVERLAY_STYLE_ANIMATED;

template <class Record> Record load_record(const uint8_t* bytes) noexcept {
    Record record{};
    std::memcpy(&record, bytes, sizeof(record));
    return record;
}

template <class Record> Record load_indexed(const uint8_t* bytes, uint32_t index) noexcept {
    return load_record<Record>(bytes + static_cast<size_t>(index) * sizeof(Record));
}

bool style_valid(const SaccadeOverlayStyle& style) noexcept {
    return style.target_stroke_q3 != 0 && style.label_height_q3 != 0 && style.glyph_width_q3 != 0 &&
           style.glyph_height_q3 != 0 && style.glyph_advance_q3 >= style.glyph_width_q3 &&
           style.label_height_q3 >= style.glyph_height_q3 && style.active_stroke_q3 != 0 && style.reserved16 == 0 &&
           (style.flags & ~style_flag_mask) == 0 && style.reserved32 == 0 && style.reserved[0] == 0 &&
           style.reserved[1] == 0;
}

bool fits_q3(uint16_t origin, uint32_t extent) noexcept {
    return extent <= UINT16_MAX - static_cast<uint32_t>(origin);
}

bool target_valid(const SaccadeOverlayTarget& target, const SaccadeOverlayStyle& style) noexcept {
    if (target.glyph_count > SACCADE_OVERLAY_GLYPHS_PER_TARGET || target.width_q3 < 2 || target.height_q3 < 2 ||
        target.flags != 0 || target.reserved != 0) {
        return false;
    }

    for (uint32_t index = 0; index < target.glyph_count; ++index) {
        if (target.glyphs[index] == SACCADE_OVERLAY_GLYPH_NONE) {
            return false;
        }
    }

    const uint32_t label_width = static_cast<uint32_t>(style.label_padding_x_q3) * 2U +
                                 static_cast<uint32_t>(style.glyph_advance_q3) * target.glyph_count;
    return label_width != 0 && fits_q3(target.x_q3, target.width_q3) && fits_q3(target.y_q3, target.height_q3) &&
           fits_q3(target.label_x_q3, label_width) && fits_q3(target.label_y_q3, style.label_height_q3);
}

SaccadeOverlayTarget target_at(const PacketView& packet, uint32_t index) noexcept {
    return load_indexed<SaccadeOverlayTarget>(packet.targets, index);
}

SaccadeOverlayStyle style_at(const PacketView& packet, uint16_t index) noexcept {
    return load_indexed<SaccadeOverlayStyle>(packet.styles, index);
}

SaccadeOverlayRect rect(uint16_t x_q3, uint16_t y_q3, uint16_t width_q3, uint16_t height_q3) noexcept {
    return {x_q3, y_q3, width_q3, height_q3};
}

} // namespace

SaccadeResult validate_packet(SaccadeSpanU8 packet, PacketView* out_view) noexcept {
    if (packet.data == nullptr || out_view == nullptr || packet.size < sizeof(SaccadeOverlayPacketHeader)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const SaccadeOverlayPacketHeader header = load_record<SaccadeOverlayPacketHeader>(packet.data);
    if (header.struct_size != sizeof(SaccadeOverlayPacketHeader) ||
        header.packet_version != SACCADE_OVERLAY_PACKET_VERSION || header.target_count > SACCADE_OVERLAY_MAX_TARGETS ||
        header.style_count > SACCADE_OVERLAY_MAX_STYLES || ((header.target_count == 0) != (header.style_count == 0)) ||
        header.target_stride != sizeof(SaccadeOverlayTarget) || header.style_stride != sizeof(SaccadeOverlayStyle) ||
        header.reserved32 != 0 || header.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t expected_targets_offset = header.target_count == 0 ? 0 : sizeof(SaccadeOverlayPacketHeader);
    const uint64_t expected_styles_offset =
        header.target_count == 0
            ? 0
            : expected_targets_offset + static_cast<uint64_t>(header.target_count) * sizeof(SaccadeOverlayTarget);
    const uint64_t expected_packet_size =
        header.target_count == 0
            ? sizeof(SaccadeOverlayPacketHeader)
            : expected_styles_offset + static_cast<uint64_t>(header.style_count) * sizeof(SaccadeOverlayStyle);
    if (header.targets_offset != expected_targets_offset || header.styles_offset != expected_styles_offset ||
        expected_packet_size != packet.size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const uint8_t* targets = header.target_count == 0 ? nullptr : packet.data + expected_targets_offset;
    const uint8_t* styles = header.style_count == 0 ? nullptr : packet.data + expected_styles_offset;

    for (uint32_t index = 0; index < header.style_count; ++index) {
        if (!style_valid(load_indexed<SaccadeOverlayStyle>(styles, index))) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (uint32_t index = 0; index < header.target_count; ++index) {
        const SaccadeOverlayTarget target = load_indexed<SaccadeOverlayTarget>(targets, index);
        if (target.style_index >= header.style_count ||
            !target_valid(target, load_indexed<SaccadeOverlayStyle>(styles, target.style_index))) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }

    *out_view = {header, targets, styles};
    return SACCADE_OK;
}

SaccadeResult expand_static(const PacketView& packet, ExpandedInstanceSpan output, size_t* out_count) noexcept {
    if (out_count == nullptr ||
        (packet.header.target_count != 0 && (packet.targets == nullptr || packet.styles == nullptr)) ||
        packet.header.target_count > SACCADE_OVERLAY_MAX_TARGETS) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t required = static_cast<size_t>(packet.header.target_count) * instances_per_target;
    *out_count = required;
    if (output.capacity < required) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (required != 0 && (output.rects == nullptr || output.metadata == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    size_t destination = 0;
    for (uint32_t target_index = 0; target_index < packet.header.target_count; ++target_index) {
        const SaccadeOverlayTarget target = target_at(packet, target_index);
        const SaccadeOverlayStyle style = style_at(packet, target.style_index);
        const uint16_t stroke = std::min({style.target_stroke_q3, static_cast<uint16_t>(target.width_q3 / 2U),
                                          static_cast<uint16_t>(target.height_q3 / 2U)});
        const uint16_t x = target.x_q3;
        const uint16_t y = target.y_q3;
        const uint16_t bottom_y = static_cast<uint16_t>(y + target.height_q3 - stroke);
        const uint16_t right_x = static_cast<uint16_t>(x + target.width_q3 - stroke);

        const SaccadeOverlayInstanceMeta outline_metadata =
            saccade_overlay_instance_meta_make(target_index, target.style_index, SACCADE_OVERLAY_INSTANCE_OUTLINE);
        output.rects[destination] = rect(x, y, target.width_q3, stroke);
        output.metadata[destination++] = outline_metadata;
        output.rects[destination] = rect(x, bottom_y, target.width_q3, stroke);
        output.metadata[destination++] = outline_metadata;
        output.rects[destination] = rect(x, y, stroke, target.height_q3);
        output.metadata[destination++] = outline_metadata;
        output.rects[destination] = rect(right_x, y, stroke, target.height_q3);
        output.metadata[destination++] = outline_metadata;

        const uint16_t label_width =
            static_cast<uint16_t>(static_cast<uint32_t>(style.label_padding_x_q3) * 2U +
                                  static_cast<uint32_t>(style.glyph_advance_q3) * target.glyph_count);
        output.rects[destination] = rect(target.label_x_q3, target.label_y_q3, label_width, style.label_height_q3);
        output.metadata[destination++] =
            saccade_overlay_instance_meta_make(target_index, target.style_index, SACCADE_OVERLAY_INSTANCE_LABEL);
    }
    return SACCADE_OK;
}

SaccadeResult expand_active(const PacketView& packet, uint32_t active_target_index, ExpandedInstanceSpan output,
                            size_t* out_count) noexcept {
    if (out_count == nullptr || (packet.header.target_count != 0 && packet.targets == nullptr) ||
        (active_target_index != SACCADE_OVERLAY_ACTIVE_TARGET_NONE &&
         active_target_index >= packet.header.target_count)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const bool has_active = active_target_index != SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
    *out_count = has_active ? 1U : 0U;
    if (!has_active) {
        return SACCADE_OK;
    }
    if (output.capacity < 1) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (output.rects == nullptr || output.metadata == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const SaccadeOverlayTarget target = target_at(packet, active_target_index);
    output.rects[0] = rect(target.x_q3, target.y_q3, target.width_q3, target.height_q3);
    output.metadata[0] =
        saccade_overlay_instance_meta_make(active_target_index, target.style_index, SACCADE_OVERLAY_INSTANCE_ACTIVE);
    return SACCADE_OK;
}

} // namespace saccade::overlay

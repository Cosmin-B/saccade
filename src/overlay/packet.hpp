#ifndef SACCADE_OVERLAY_PACKET_HPP
#define SACCADE_OVERLAY_PACKET_HPP

#include <saccade/saccade_overlay.h>

#include <cstddef>
#include <cstdint>

namespace saccade::overlay {

struct PacketView {
    SaccadeOverlayPacketHeader header{};
    const uint8_t* targets = nullptr;
    const uint8_t* styles = nullptr;
};

struct ExpandedInstanceSpan {
    SaccadeOverlayRect* rects = nullptr;
    SaccadeOverlayInstanceMeta* metadata = nullptr;
    size_t capacity = 0;
};

SaccadeResult validate_packet(SaccadeSpanU8 packet, PacketView* out_view) noexcept;
SaccadeResult expand_static(const PacketView& packet, ExpandedInstanceSpan output, size_t* out_count) noexcept;
SaccadeResult expand_active(const PacketView& packet, uint32_t active_target_index, ExpandedInstanceSpan output,
                            size_t* out_count) noexcept;

} // namespace saccade::overlay

#endif

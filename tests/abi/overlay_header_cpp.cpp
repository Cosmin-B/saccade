#include <saccade/saccade_overlay.h>

#include <type_traits>

static_assert(sizeof(SaccadeOverlayPacketHeader) == 64, "packet header ABI");
static_assert(sizeof(SaccadeOverlayTarget) == 48, "target ABI");
static_assert(sizeof(SaccadeOverlayStyle) == 64, "style ABI");
static_assert(sizeof(SaccadeOverlayRect) == 8, "rect ABI");
static_assert(sizeof(SaccadeOverlayInstanceMeta) == 4, "metadata ABI");
static_assert(std::is_trivially_copyable_v<SaccadeOverlayTarget>);
static_assert(std::is_standard_layout_v<SaccadeOverlayRect>);

int main() {
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.reserved32 = 0;
    const SaccadeOverlayInstanceMeta metadata =
        saccade_overlay_instance_meta_make(7, 2, SACCADE_OVERLAY_INSTANCE_LABEL);
    return header.target_count == 0 && saccade_overlay_instance_meta_target(metadata) == 7 &&
                   saccade_overlay_instance_meta_style(metadata) == 2 &&
                   saccade_overlay_instance_meta_kind(metadata) == SACCADE_OVERLAY_INSTANCE_LABEL
               ? 0
               : 1;
}

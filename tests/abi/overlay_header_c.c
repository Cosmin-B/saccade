#include <saccade/saccade_overlay.h>

_Static_assert(sizeof(SaccadeOverlayPacketHeader) == 64, "packet header ABI");
_Static_assert(sizeof(SaccadeOverlayTarget) == 48, "target ABI");
_Static_assert(sizeof(SaccadeOverlayStyle) == 64, "style ABI");
_Static_assert(sizeof(SaccadeOverlayRect) == 8, "rect ABI");
_Static_assert(sizeof(SaccadeOverlayInstanceMeta) == 4, "metadata ABI");

int main(void) {
    SaccadeOverlayPacketHeader header = {0};
    header.struct_size = (uint32_t)sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.reserved32 = 0;
    SaccadeOverlayInstanceMeta metadata = saccade_overlay_instance_meta_make(7, 2, SACCADE_OVERLAY_INSTANCE_LABEL);
    return header.target_count == 0 && saccade_overlay_instance_meta_target(metadata) == 7 &&
                   saccade_overlay_instance_meta_style(metadata) == 2 &&
                   saccade_overlay_instance_meta_kind(metadata) == SACCADE_OVERLAY_INSTANCE_LABEL
               ? 0
               : 1;
}

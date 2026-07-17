#ifndef SACCADE_SACCADE_OVERLAY_H
#define SACCADE_SACCADE_OVERLAY_H

#include <saccade/saccade.h>

#define SACCADE_OVERLAY_PACKET_VERSION UINT32_C(0x00010001)
#define SACCADE_OVERLAY_MAX_TARGETS UINT32_C(10000)
#define SACCADE_OVERLAY_MAX_STYLES UINT32_C(16)
#define SACCADE_OVERLAY_GLYPHS_PER_TARGET UINT32_C(16)
#define SACCADE_OVERLAY_ACTIVE_TARGET_NONE UINT32_MAX
#define SACCADE_OVERLAY_GLYPH_NONE UINT8_MAX
#define SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET UINT32_C(0x00000001)
#define SACCADE_OVERLAY_STYLE_ANIMATED UINT32_C(0x00000001)

typedef uint32_t SaccadeOverlayInstanceKind;

enum { SACCADE_OVERLAY_INSTANCE_OUTLINE = 1, SACCADE_OVERLAY_INSTANCE_LABEL = 2, SACCADE_OVERLAY_INSTANCE_ACTIVE = 3 };

typedef uint32_t SaccadeOverlayInstanceMeta;

#define SACCADE_OVERLAY_META_TARGET_BITS UINT32_C(14)
#define SACCADE_OVERLAY_META_TARGET_MASK UINT32_C(0x00003FFF)
#define SACCADE_OVERLAY_META_STYLE_SHIFT UINT32_C(14)
#define SACCADE_OVERLAY_META_STYLE_MASK UINT32_C(0x0003C000)
#define SACCADE_OVERLAY_META_KIND_SHIFT UINT32_C(18)
#define SACCADE_OVERLAY_META_KIND_MASK UINT32_C(0x001C0000)

static inline SaccadeOverlayInstanceMeta saccade_overlay_instance_meta_make(uint32_t target_index, uint32_t style_index,
                                                                            uint32_t kind) {
    return (target_index & SACCADE_OVERLAY_META_TARGET_MASK) |
           ((style_index << SACCADE_OVERLAY_META_STYLE_SHIFT) & SACCADE_OVERLAY_META_STYLE_MASK) |
           ((kind << SACCADE_OVERLAY_META_KIND_SHIFT) & SACCADE_OVERLAY_META_KIND_MASK);
}

static inline uint32_t saccade_overlay_instance_meta_target(SaccadeOverlayInstanceMeta metadata) {
    return metadata & SACCADE_OVERLAY_META_TARGET_MASK;
}

static inline uint32_t saccade_overlay_instance_meta_style(SaccadeOverlayInstanceMeta metadata) {
    return (metadata & SACCADE_OVERLAY_META_STYLE_MASK) >> SACCADE_OVERLAY_META_STYLE_SHIFT;
}

static inline uint32_t saccade_overlay_instance_meta_kind(SaccadeOverlayInstanceMeta metadata) {
    return (metadata & SACCADE_OVERLAY_META_KIND_MASK) >> SACCADE_OVERLAY_META_KIND_SHIFT;
}

/* Packet geometry uses unsigned Q13.3 pixels local to one presentation surface. */
typedef struct SaccadeOverlayPacketHeader {
    uint32_t struct_size;
    uint32_t packet_version;
    uint32_t target_count;
    uint32_t target_stride;
    uint32_t style_count;
    uint32_t style_stride;
    uint32_t reserved32;
    uint32_t flags;
    uint64_t scene_epoch;
    uint64_t transform_epoch;
    uint64_t targets_offset;
    uint64_t styles_offset;
} SaccadeOverlayPacketHeader;

typedef struct SaccadeOverlayTarget {
    uint64_t target_id;
    uint16_t x_q3;
    uint16_t y_q3;
    uint16_t width_q3;
    uint16_t height_q3;
    uint16_t label_x_q3;
    uint16_t label_y_q3;
    uint16_t confidence_q16;
    uint8_t glyphs[SACCADE_OVERLAY_GLYPHS_PER_TARGET];
    uint8_t style_index;
    uint8_t glyph_count;
    uint16_t flags;
    uint16_t reserved;
} SaccadeOverlayTarget;

/* Colors use 0xRRGGBBAA byte order. Geometry fields use unsigned Q13.3 pixels. */
typedef struct SaccadeOverlayStyle {
    uint32_t target_outline_rgba8;
    uint32_t label_background_rgba8;
    uint32_t label_foreground_rgba8;
    uint32_t active_fill_rgba8;
    uint32_t active_outline_rgba8;
    uint16_t target_stroke_q3;
    uint16_t target_radius_q3;
    uint16_t label_height_q3;
    uint16_t label_radius_q3;
    uint16_t label_padding_x_q3;
    uint16_t glyph_width_q3;
    uint16_t glyph_height_q3;
    uint16_t glyph_advance_q3;
    uint16_t active_stroke_q3;
    uint16_t reserved16;
    uint32_t flags;
    uint32_t reserved32;
    uint64_t reserved[2];
} SaccadeOverlayStyle;

/* Geometry and metadata are separate compact GPU streams. */
typedef struct SaccadeOverlayRect {
    uint16_t x_q3;
    uint16_t y_q3;
    uint16_t width_q3;
    uint16_t height_q3;
} SaccadeOverlayRect;

#endif

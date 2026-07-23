#ifndef SACCADE_SACCADE_SCENE_H
#define SACCADE_SACCADE_SCENE_H

#include <stddef.h>
#include <stdint.h>

#define SACCADE_TARGET_PACKET_VERSION UINT32_C(0x00010002)
#define SACCADE_TARGET_PACKET_MAX_TARGETS UINT32_C(10000)
#define SACCADE_TARGET_PACKET_MAX_TEXT_BYTES UINT32_C(16384)

/* INCOMPLETE marks a partial set from a bounded or interrupted source. */
enum { SACCADE_TARGET_PACKET_INCOMPLETE = UINT32_C(1) << 0, SACCADE_TARGET_PACKET_TEXT_TRUNCATED = UINT32_C(1) << 1 };

typedef uint32_t SaccadeCoordinateSpace;

enum {
    SACCADE_COORDINATE_SPACE_MODEL_Q8 = 1,
    SACCADE_COORDINATE_SPACE_SOURCE_Q8 = 2,
    SACCADE_COORDINATE_SPACE_DESKTOP_Q8 = 3
};

typedef uint16_t SaccadeTargetRole;

enum {
    SACCADE_TARGET_ROLE_UNKNOWN = 0,
    SACCADE_TARGET_ROLE_BUTTON = 1,
    SACCADE_TARGET_ROLE_LINK = 2,
    SACCADE_TARGET_ROLE_TEXT = 3,
    SACCADE_TARGET_ROLE_TEXT_FIELD = 4,
    SACCADE_TARGET_ROLE_CHECKBOX = 5,
    SACCADE_TARGET_ROLE_RADIO = 6,
    SACCADE_TARGET_ROLE_MENU_ITEM = 7,
    SACCADE_TARGET_ROLE_SLIDER = 8,
    SACCADE_TARGET_ROLE_IMAGE = 9,
    SACCADE_TARGET_ROLE_WINDOW = 10
};

enum {
    SACCADE_TARGET_SOURCE_NEURAL = UINT16_C(1) << 0,
    SACCADE_TARGET_SOURCE_ACCESSIBILITY = UINT16_C(1) << 1,
    SACCADE_TARGET_SOURCE_PIXEL = UINT16_C(1) << 2,
    SACCADE_TARGET_SOURCE_GRID = UINT16_C(1) << 3
};

enum {
    SACCADE_TARGET_ACTIONABLE = UINT32_C(1) << 0,
    SACCADE_TARGET_DISABLED = UINT32_C(1) << 1,
    SACCADE_TARGET_OCCLUDED = UINT32_C(1) << 2,
    SACCADE_TARGET_SECURE = UINT32_C(1) << 3,
    SACCADE_TARGET_APPROXIMATE = UINT32_C(1) << 4,
    SACCADE_TARGET_TEXT_REDACTED = UINT32_C(1) << 5,
    SACCADE_TARGET_TEXT_TRUNCATED = UINT32_C(1) << 6
};

typedef uint32_t SaccadeTargetCapabilityBits;

enum {
    SACCADE_TARGET_CAPABILITY_POINTER_MOVE = UINT32_C(1) << 0,
    SACCADE_TARGET_CAPABILITY_BUTTON = UINT32_C(1) << 1,
    SACCADE_TARGET_CAPABILITY_SCROLL = UINT32_C(1) << 2,
    SACCADE_TARGET_CAPABILITY_DRAG_SOURCE = UINT32_C(1) << 3,
    SACCADE_TARGET_CAPABILITY_DROP_TARGET = UINT32_C(1) << 4,
    SACCADE_TARGET_CAPABILITY_TEXT = UINT32_C(1) << 5,
    SACCADE_TARGET_CAPABILITY_INVOKE = UINT32_C(1) << 6,
    SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE = UINT32_C(1) << 7,
    SACCADE_TARGET_CAPABILITY_TEXT_SELECT = UINT32_C(1) << 8
};

typedef struct SaccadeTargetPacketHeader {
    uint32_t struct_size;
    uint32_t packet_version;
    uint32_t target_count;
    uint32_t target_stride;
    uint32_t flags;
    SaccadeCoordinateSpace coordinate_space;
    /* Immutable publication identifier. */
    uint64_t scene_epoch;
    uint64_t frame_id;
    /* Monotonic capture timestamp in nanoseconds. Zero means unavailable. */
    uint64_t capture_time_ns;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
    /* Byte offset from the packet start to target_stride-spaced records. */
    uint64_t targets_offset;
    uint64_t total_size;
} SaccadeTargetPacketHeader;

typedef struct SaccadeTargetTextRef {
    /* Offset and size are measured in the packet's bounded UTF-8 text lane. */
    uint16_t offset;
    uint16_t size;
} SaccadeTargetTextRef;

typedef struct SaccadeTargetRecord {
    uint64_t target_id;
    uint64_t parent_id;
    uint64_t window_id;
    uint64_t display_id;
    int32_t x_q8;
    int32_t y_q8;
    int32_t width_q8;
    int32_t height_q8;
    int32_t safe_x_q8;
    int32_t safe_y_q8;
    uint32_t confidence_q16;
    SaccadeTargetRole role;
    uint16_t source_bits;
    SaccadeTargetCapabilityBits capability_bits;
    uint32_t flags;
    uint32_t order;
    SaccadeTargetTextRef text;
} SaccadeTargetRecord;

#endif

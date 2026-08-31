#ifndef SACCADE_SACCADE_INPUT_H
#define SACCADE_SACCADE_INPUT_H

#include <stddef.h>
#include <stdint.h>

#define SACCADE_INPUT_PLAN_VERSION UINT32_C(0x00010001)
#define SACCADE_INPUT_PLAN_MAX_TARGETS UINT32_C(64)
#define SACCADE_INPUT_PLAN_MAX_COMMANDS (SACCADE_INPUT_PLAN_MAX_TARGETS * UINT32_C(2) + UINT32_C(1))

typedef uint32_t SaccadeInputPermissionBits;

enum {
    SACCADE_INPUT_PERMISSION_POINTER = UINT32_C(1) << 0,
    SACCADE_INPUT_PERMISSION_KEYBOARD = UINT32_C(1) << 1,
    SACCADE_INPUT_PERMISSION_TEXT = UINT32_C(1) << 2,
    SACCADE_INPUT_PERMISSION_WINDOW = UINT32_C(1) << 3,
    SACCADE_INPUT_PERMISSION_CLIPBOARD = UINT32_C(1) << 4
};

typedef uint32_t SaccadeInputButtonBits;

enum {
    SACCADE_INPUT_BUTTON_LEFT = UINT32_C(1) << 0,
    SACCADE_INPUT_BUTTON_RIGHT = UINT32_C(1) << 1,
    SACCADE_INPUT_BUTTON_MIDDLE = UINT32_C(1) << 2
};

typedef uint32_t SaccadeInputModifierBits;

enum {
    SACCADE_INPUT_MODIFIER_SHIFT = UINT32_C(1) << 0,
    SACCADE_INPUT_MODIFIER_CONTROL = UINT32_C(1) << 1,
    SACCADE_INPUT_MODIFIER_ALT = UINT32_C(1) << 2,
    SACCADE_INPUT_MODIFIER_META = UINT32_C(1) << 3
};

typedef uint32_t SaccadeInputCommandKind;

enum {
    SACCADE_INPUT_COMMAND_POINTER_MOVE = 1,
    SACCADE_INPUT_COMMAND_BUTTON_DOWN = 2,
    SACCADE_INPUT_COMMAND_BUTTON_UP = 3,
    SACCADE_INPUT_COMMAND_CLICK = 4,
    SACCADE_INPUT_COMMAND_SCROLL = 5,
    SACCADE_INPUT_COMMAND_KEY_DOWN = 6,
    SACCADE_INPUT_COMMAND_KEY_UP = 7,
    SACCADE_INPUT_COMMAND_TEXT = 8,
    SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE = 9,
    SACCADE_INPUT_COMMAND_WAIT = 10
};

typedef uint32_t SaccadeInputCommandFlags;

enum {
    SACCADE_INPUT_COMMAND_ABSOLUTE = UINT32_C(1) << 0,
    SACCADE_INPUT_COMMAND_PHYSICAL_KEY = UINT32_C(1) << 1,
    SACCADE_INPUT_COMMAND_CONTINUOUS = UINT32_C(1) << 2
};

typedef uint32_t SaccadeInputPlanFlags;

enum {
    SACCADE_INPUT_PLAN_DRY_RUN = UINT32_C(1) << 0,
    SACCADE_INPUT_PLAN_STOP_ON_FAILURE = UINT32_C(1) << 1,
    SACCADE_INPUT_PLAN_RESTORE_POINTER = UINT32_C(1) << 2,
    SACCADE_INPUT_PLAN_FOLLOW_TARGETS = UINT32_C(1) << 3
};

typedef struct SaccadeInputCommand {
    SaccadeInputCommandKind kind;
    SaccadeInputCommandFlags flags;
    uint64_t target_id;
    int32_t x_q8;
    int32_t y_q8;
    int32_t delta_x_q8;
    int32_t delta_y_q8;
    uint32_t data0;
    uint32_t data1;
    uint64_t duration_ns;
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t data2;
    uint32_t reserved32;
} SaccadeInputCommand;

typedef struct SaccadeInputPlanHeader {
    uint32_t struct_size;
    uint32_t plan_version;
    uint32_t command_count;
    uint32_t command_stride;
    SaccadeInputPlanFlags flags;
    SaccadeInputPermissionBits required_permissions;
    SaccadeInputButtonBits expected_buttons;
    uint32_t reserved32;
    uint64_t plan_id;
    uint64_t scene_epoch;
    uint64_t frame_id;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t permission_epoch;
    uint64_t source_id;
    uint64_t focus_id;
    uint64_t window_id;
    uint64_t display_id;
    uint64_t deadline_ns;
    uint64_t commands_offset;
    uint64_t total_size;
    uint64_t reserved;
} SaccadeInputPlanHeader;

typedef struct SaccadePhysicalInputState {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t pointer_x_q8;
    int32_t pointer_y_q8;
    SaccadeInputButtonBits buttons;
    SaccadeInputModifierBits modifiers;
    uint64_t active_lease_id;
    uint64_t permission_epoch;
    uint64_t physical_sequence;
    uint64_t reserved[2];
} SaccadePhysicalInputState;

#endif

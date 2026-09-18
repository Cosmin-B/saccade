#ifndef SACCADE_SACCADE_AGENT_H
#define SACCADE_SACCADE_AGENT_H

#include <stddef.h>
#include <stdint.h>

#define SACCADE_AGENT_API_VERSION UINT32_C(0x00010002)
#define SACCADE_AGENT_MAX_MESSAGE_BYTES UINT32_C(1048576)
#define SACCADE_AGENT_MAX_TARGETS UINT32_C(10000)
#define SACCADE_AGENT_MAX_FILTERS UINT32_C(32)
#define SACCADE_AGENT_MAX_ACTIONS UINT32_C(64)
#define SACCADE_AGENT_MAX_PAYLOAD_BYTES UINT32_C(65536)
#define SACCADE_AGENT_MAX_TEXT_BYTES UINT32_C(16384)
#define SACCADE_AGENT_MAX_SELECTION_ITEMS UINT32_C(256)
#define SACCADE_AGENT_MAX_ACTION_DURATION_NS UINT64_C(60000000000)

typedef uint32_t SaccadeAgentMessageKind;

enum {
    SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST = 1,
    SACCADE_AGENT_MESSAGE_OBSERVE_COMPLETION = 2,
    SACCADE_AGENT_MESSAGE_QUERY_REQUEST = 3,
    SACCADE_AGENT_MESSAGE_QUERY_COMPLETION = 4,
    SACCADE_AGENT_MESSAGE_ACTION_BATCH = 5,
    SACCADE_AGENT_MESSAGE_ACTION_COMPLETION = 6,
    SACCADE_AGENT_MESSAGE_HELLO_REQUEST = 7,
    SACCADE_AGENT_MESSAGE_HELLO_COMPLETION = 8
};

enum {
    SACCADE_AGENT_MESSAGE_TRUNCATED = UINT32_C(1) << 0,
    SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE = UINT32_C(1) << 1,
    SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE = UINT32_C(1) << 2
};

typedef int32_t SaccadeAgentResult;

enum {
    SACCADE_AGENT_OK = 0,
    SACCADE_AGENT_ERROR_INVALID_MESSAGE = -1,
    SACCADE_AGENT_ERROR_CAPABILITY_DENIED = -2,
    SACCADE_AGENT_ERROR_STALE_GENERATION = -3,
    SACCADE_AGENT_ERROR_TARGET_NOT_FOUND = -4,
    SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE = -5,
    SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED = -6,
    SACCADE_AGENT_ERROR_PERMISSION_DENIED = -7,
    SACCADE_AGENT_ERROR_SECURE_SURFACE = -8,
    SACCADE_AGENT_ERROR_FOCUS_CHANGED = -9,
    SACCADE_AGENT_ERROR_INVALID_TRANSFORM = -10,
    SACCADE_AGENT_ERROR_TIMEOUT = -11,
    SACCADE_AGENT_ERROR_CANCELLED = -12,
    SACCADE_AGENT_ERROR_EXECUTOR_LOST = -13,
    SACCADE_AGENT_ERROR_BACKEND = -14,
    SACCADE_AGENT_ERROR_CAPACITY = -15,
    SACCADE_AGENT_ERROR_ACTIVATION_REQUIRED = -16,
    SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED = -17
};

typedef uint32_t SaccadeAgentCapabilityBits;

enum {
    SACCADE_AGENT_CAPABILITY_OBSERVE = UINT32_C(1) << 0,
    SACCADE_AGENT_CAPABILITY_POINTER = UINT32_C(1) << 1,
    SACCADE_AGENT_CAPABILITY_KEYBOARD = UINT32_C(1) << 2,
    SACCADE_AGENT_CAPABILITY_CLIPBOARD = UINT32_C(1) << 3,
    SACCADE_AGENT_CAPABILITY_WINDOW = UINT32_C(1) << 4,
    SACCADE_AGENT_CAPABILITY_SETTINGS = UINT32_C(1) << 5
};

typedef uint32_t SaccadeAgentScopeKind;

enum {
    SACCADE_AGENT_SCOPE_ACTIVE_WINDOW = 1,
    SACCADE_AGENT_SCOPE_DISPLAY = 2,
    SACCADE_AGENT_SCOPE_DESKTOP = 3,
    SACCADE_AGENT_SCOPE_RECT = 4,
    SACCADE_AGENT_SCOPE_WINDOW = 5
};

typedef uint32_t SaccadeAgentSourceMode;

enum { SACCADE_AGENT_SOURCE_PIXEL = 1, SACCADE_AGENT_SOURCE_SEMANTIC = 2, SACCADE_AGENT_SOURCE_GRID = 3, SACCADE_AGENT_SOURCE_FUSED = 4 };

typedef uint32_t SaccadeAgentFreshnessPolicy;

enum { SACCADE_AGENT_FRESHNESS_LATEST_VALID = 1, SACCADE_AGENT_FRESHNESS_AFTER_GENERATION = 2, SACCADE_AGENT_FRESHNESS_FORCE_REFRESH = 3 };

enum { SACCADE_AGENT_FRESHNESS_REQUIRE_DAMAGE_CHECK = UINT32_C(1) << 0, SACCADE_AGENT_FRESHNESS_REQUIRE_NEURAL_REFRESH = UINT32_C(1) << 1 };

/* The next four enums are wire copies of the scene target vocabulary in
   include/saccade/saccade_scene.h. Values must stay numerically identical.
   src/agent/service.cpp asserts every pair at compile time.
   SACCADE_AGENT_TARGET_KEYBOARD is agent-only and has no scene counterpart. */
typedef uint32_t SaccadeAgentTargetCapabilityBits;

enum {
    SACCADE_AGENT_TARGET_POINTER_MOVE = UINT32_C(1) << 0,
    SACCADE_AGENT_TARGET_CLICK = UINT32_C(1) << 1,
    SACCADE_AGENT_TARGET_SCROLL = UINT32_C(1) << 2,
    SACCADE_AGENT_TARGET_DRAG_SOURCE = UINT32_C(1) << 3,
    SACCADE_AGENT_TARGET_DROP_TARGET = UINT32_C(1) << 4,
    SACCADE_AGENT_TARGET_TEXT = UINT32_C(1) << 5,
    SACCADE_AGENT_TARGET_INVOKE = UINT32_C(1) << 6,
    SACCADE_AGENT_TARGET_WINDOW_ACTIVATE = UINT32_C(1) << 7,
    SACCADE_AGENT_TARGET_TEXT_SELECT = UINT32_C(1) << 8,
    SACCADE_AGENT_TARGET_KEYBOARD = UINT32_C(1) << 9
};

typedef uint32_t SaccadeAgentTargetFlags;

enum {
    SACCADE_AGENT_TARGET_ACTIONABLE = UINT32_C(1) << 0,
    SACCADE_AGENT_TARGET_DISABLED = UINT32_C(1) << 1,
    SACCADE_AGENT_TARGET_OCCLUDED = UINT32_C(1) << 2,
    SACCADE_AGENT_TARGET_SECURE = UINT32_C(1) << 3,
    SACCADE_AGENT_TARGET_APPROXIMATE = UINT32_C(1) << 4,
    SACCADE_AGENT_TARGET_TEXT_REDACTED = UINT32_C(1) << 5,
    SACCADE_AGENT_TARGET_TEXT_TRUNCATED = UINT32_C(1) << 6,
    /* Explicit-window action disposition. Exactly one is set for targets in
       an explicit WINDOW completion. */
    SACCADE_AGENT_TARGET_BACKGROUND_ACTIONABLE = UINT32_C(1) << 7,
    SACCADE_AGENT_TARGET_ACTIVATION_REQUIRED = UINT32_C(1) << 8,
    SACCADE_AGENT_TARGET_BACKGROUND_UNSUPPORTED = UINT32_C(1) << 9
};

typedef uint16_t SaccadeAgentTargetRole;

enum {
    SACCADE_AGENT_ROLE_UNKNOWN = 0,
    SACCADE_AGENT_ROLE_BUTTON = 1,
    SACCADE_AGENT_ROLE_LINK = 2,
    SACCADE_AGENT_ROLE_TEXT = 3,
    SACCADE_AGENT_ROLE_TEXT_FIELD = 4,
    SACCADE_AGENT_ROLE_CHECKBOX = 5,
    SACCADE_AGENT_ROLE_RADIO = 6,
    SACCADE_AGENT_ROLE_MENU_ITEM = 7,
    SACCADE_AGENT_ROLE_SLIDER = 8,
    SACCADE_AGENT_ROLE_IMAGE = 9,
    SACCADE_AGENT_ROLE_WINDOW = 10
};

typedef uint16_t SaccadeAgentTargetSourceBits;

enum {
    SACCADE_AGENT_TARGET_SOURCE_NEURAL = UINT16_C(1) << 0,
    SACCADE_AGENT_TARGET_SOURCE_ACCESSIBILITY = UINT16_C(1) << 1,
    SACCADE_AGENT_TARGET_SOURCE_PIXEL = UINT16_C(1) << 2,
    SACCADE_AGENT_TARGET_SOURCE_GRID = UINT16_C(1) << 3
};

typedef uint32_t SaccadeAgentQueryFilterFlags;

enum {
    SACCADE_AGENT_QUERY_STABLE_ID = UINT32_C(1) << 0,
    SACCADE_AGENT_QUERY_ROLE = UINT32_C(1) << 1,
    SACCADE_AGENT_QUERY_TEXT = UINT32_C(1) << 2,
    SACCADE_AGENT_QUERY_CAPABILITY = UINT32_C(1) << 3,
    SACCADE_AGENT_QUERY_SOURCE = UINT32_C(1) << 4,
    SACCADE_AGENT_QUERY_GEOMETRY = UINT32_C(1) << 5,
    SACCADE_AGENT_QUERY_CONFIDENCE = UINT32_C(1) << 6,
    SACCADE_AGENT_QUERY_RELATION = UINT32_C(1) << 7
};

typedef uint32_t SaccadeAgentTextMatch;

enum { SACCADE_AGENT_TEXT_EXACT = 1, SACCADE_AGENT_TEXT_PREFIX = 2, SACCADE_AGENT_TEXT_SUBSTRING = 3 };

typedef uint32_t SaccadeAgentRelation;

enum {
    SACCADE_AGENT_RELATION_SELF = 1,
    SACCADE_AGENT_RELATION_PARENT = 2,
    SACCADE_AGENT_RELATION_CHILD = 3,
    SACCADE_AGENT_RELATION_ANCESTOR = 4,
    SACCADE_AGENT_RELATION_DESCENDANT = 5,
    SACCADE_AGENT_RELATION_SIBLING = 6,
    SACCADE_AGENT_RELATION_CONTAINS = 7,
    SACCADE_AGENT_RELATION_CONTAINED_BY = 8,
    /* Reserved for a future version. Version 0.1 services reject queries using
       this relation with SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED. */
    SACCADE_AGENT_RELATION_NEAREST = 9
};

typedef uint32_t SaccadeAgentActionKind;

enum {
    SACCADE_AGENT_ACTION_POINTER_MOVE = 1,
    SACCADE_AGENT_ACTION_POINTER_HOVER = 2,
    SACCADE_AGENT_ACTION_CLICK = 3,
    SACCADE_AGENT_ACTION_HOLD = 4,
    SACCADE_AGENT_ACTION_DRAG_DROP = 5,
    SACCADE_AGENT_ACTION_SCROLL = 6,
    SACCADE_AGENT_ACTION_KEY = 7,
    SACCADE_AGENT_ACTION_TEXT = 8,
    SACCADE_AGENT_ACTION_WINDOW_ACTIVATE = 9,
    SACCADE_AGENT_ACTION_RELEASE = 10,
    SACCADE_AGENT_ACTION_TEXT_SELECT = 11,
    SACCADE_AGENT_ACTION_INVOKE = 12,
    SACCADE_AGENT_ACTION_KEY_CHORD = 13,
    SACCADE_AGENT_ACTION_WINDOW_CYCLE = 14,
    SACCADE_AGENT_ACTION_ABORT = 15,
    SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE = 16
};

typedef uint32_t SaccadeAgentActionFlags;

enum {
    SACCADE_AGENT_ACTION_EXPLICIT_POINTS = UINT32_C(1) << 0,
    SACCADE_AGENT_ACTION_CYCLE_BACKWARD = UINT32_C(1) << 1,
    SACCADE_AGENT_ACTION_ALLOW_ACTIVATION = UINT32_C(1) << 2,
    /* Resolve this action against the exact WINDOW scene named by the full
       generation/process/window preconditions, never the foreground scene. */
    SACCADE_AGENT_ACTION_EXPLICIT_WINDOW = UINT32_C(1) << 3
};

typedef uint32_t SaccadeAgentActionResultFlags;

enum {
    SACCADE_AGENT_ACTION_RESULT_BACKGROUND_ACCESSIBILITY = UINT32_C(1) << 0,
    SACCADE_AGENT_ACTION_RESULT_WOULD_ACTIVATE = UINT32_C(1) << 1,
    SACCADE_AGENT_ACTION_RESULT_WINDOW_ACTIVATED = UINT32_C(1) << 2,
    SACCADE_AGENT_ACTION_RESULT_CG_EVENT = UINT32_C(1) << 3
};

typedef uint32_t SaccadeAgentButtonBits;

enum {
    SACCADE_AGENT_BUTTON_LEFT = UINT32_C(1) << 0,
    SACCADE_AGENT_BUTTON_RIGHT = UINT32_C(1) << 1,
    SACCADE_AGENT_BUTTON_MIDDLE = UINT32_C(1) << 2
};

typedef uint32_t SaccadeAgentModifierBits;

enum {
    SACCADE_AGENT_MODIFIER_SHIFT = UINT32_C(1) << 0,
    SACCADE_AGENT_MODIFIER_CONTROL = UINT32_C(1) << 1,
    SACCADE_AGENT_MODIFIER_ALT = UINT32_C(1) << 2,
    SACCADE_AGENT_MODIFIER_META = UINT32_C(1) << 3
};

typedef uint32_t SaccadeAgentBatchPolicy;

enum { SACCADE_AGENT_BATCH_STOP_ON_FAILURE = 1, SACCADE_AGENT_BATCH_CONTINUE_ON_FAILURE = 2 };

enum { SACCADE_AGENT_BATCH_DRY_RUN = UINT32_C(1) << 0, SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION = UINT32_C(1) << 1 };

typedef uint32_t SaccadeAgentPreconditionFlags;

enum {
    SACCADE_AGENT_PRECONDITION_GENERATION = UINT32_C(1) << 0,
    SACCADE_AGENT_PRECONDITION_PROCESS = UINT32_C(1) << 1,
    SACCADE_AGENT_PRECONDITION_WINDOW = UINT32_C(1) << 2,
    SACCADE_AGENT_PRECONDITION_DISPLAY = UINT32_C(1) << 3,
    SACCADE_AGENT_PRECONDITION_TRANSFORM = UINT32_C(1) << 4,
    SACCADE_AGENT_PRECONDITION_PERMISSION = UINT32_C(1) << 5,
    SACCADE_AGENT_PRECONDITION_PHYSICAL_STATE = UINT32_C(1) << 6
};

enum { SACCADE_AGENT_PHYSICAL_SUSPENDED = UINT32_C(1) << 0, SACCADE_AGENT_PHYSICAL_USER_OVERRIDE = UINT32_C(1) << 1 };

typedef struct SaccadeAgentMessageHeader {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeAgentMessageKind message_kind;
    uint32_t flags;
} SaccadeAgentMessageHeader;

typedef struct SaccadeAgentHelloRequest {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentCapabilityBits requested_capability_bits;
    uint32_t flags;
    uint64_t client_nonce[2];
} SaccadeAgentHelloRequest;

typedef struct SaccadeAgentHelloCompletion {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentResult result;
    int32_t platform_error;
    SaccadeAgentCapabilityBits granted_capability_bits;
    uint32_t reserved;
    uint64_t service_nonce[2];
} SaccadeAgentHelloCompletion;

typedef struct SaccadeAgentRectQ8 {
    int32_t x_q8;
    int32_t y_q8;
    int32_t width_q8;
    int32_t height_q8;
} SaccadeAgentRectQ8;

typedef struct SaccadeAgentPointQ8 {
    int32_t x_q8;
    int32_t y_q8;
} SaccadeAgentPointQ8;

typedef struct SaccadeAgentScope {
    SaccadeAgentScopeKind kind;
    /* Source selection is a filter. Fused selects the published fused scene. */
    SaccadeAgentSourceMode source_mode;
    /* Display, active-window, and exact-window scopes use stable_id. Rect
       scopes use rect. Exact-window stable_id is a current public CGWindowID. */
    uint64_t stable_id;
    SaccadeAgentRectQ8 rect;
} SaccadeAgentScope;

typedef struct SaccadeAgentFreshness {
    /* The service currently supports LATEST_VALID and AFTER_GENERATION. */
    SaccadeAgentFreshnessPolicy policy;
    /* Freshness flags are reserved for policies that the current service rejects. */
    uint32_t flags;
    uint64_t after_generation;
    uint64_t timeout_ns;
} SaccadeAgentFreshness;

typedef struct SaccadeAgentGeneration {
    /* The returned generation is the published scene_epoch. */
    uint64_t generation;
    uint64_t scene_epoch;
    /* Capture frame used to build this scene. */
    uint64_t frame_id;
    /* Monotonic capture timestamp from the scene. Zero means unavailable. */
    uint64_t capture_time_ns;
    /* Process that owns the captured scene. It need not be foreground for an
       exact WINDOW observation. */
    uint64_t process_id;
    uint64_t window_id;
    uint64_t display_id;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t permission_epoch;
} SaccadeAgentGeneration;

typedef struct SaccadeAgentTarget {
    uint64_t target_id;
    uint64_t parent_id;
    uint64_t window_id;
    uint64_t display_id;
    SaccadeAgentRectQ8 bounds;
    SaccadeAgentPointQ8 safe_point;
    SaccadeAgentTargetCapabilityBits capability_bits;
    SaccadeAgentTargetFlags flags;
    SaccadeAgentTargetRole role;
    SaccadeAgentTargetSourceBits source_bits;
    uint32_t confidence_q16;
    /* Absolute byte offset from the completion start to UTF-8 target text. */
    uint32_t text_offset;
    uint32_t text_size;
    uint32_t order;
    uint32_t reserved;
} SaccadeAgentTarget;

typedef struct SaccadeAgentQueryFilter {
    SaccadeAgentQueryFilterFlags flags;
    SaccadeAgentTargetRole role;
    SaccadeAgentTargetSourceBits source_bits;
    SaccadeAgentTargetCapabilityBits required_capability_bits;
    SaccadeAgentTargetCapabilityBits forbidden_capability_bits;
    uint64_t target_id;
    uint64_t relation_target_id;
    SaccadeAgentRectQ8 geometry;
    uint32_t minimum_confidence_q16;
    SaccadeAgentTextMatch text_match;
    uint32_t text_offset;
    uint32_t text_size;
    SaccadeAgentRelation relation;
    uint32_t reserved;
} SaccadeAgentQueryFilter;

typedef struct SaccadeAgentObserveRequest {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentScope scope;
    SaccadeAgentFreshness freshness;
    SaccadeAgentCapabilityBits requested_capability_bits;
    uint32_t maximum_targets;
    uint32_t target_stride;
    uint32_t total_capacity;
} SaccadeAgentObserveRequest;

typedef struct SaccadeAgentObserveCompletion {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentResult result;
    int32_t platform_error;
    SaccadeAgentCapabilityBits granted_capability_bits;
    uint32_t target_count;
    uint32_t target_stride;
    /* Absolute byte offset from the completion start to target records. */
    uint32_t targets_offset;
    uint32_t total_size;
    uint32_t reserved;
    SaccadeAgentGeneration generation;
    SaccadeAgentScope scope;
} SaccadeAgentObserveCompletion;

typedef struct SaccadeAgentQueryRequest {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    uint64_t generation;
    SaccadeAgentScope scope;
    SaccadeAgentCapabilityBits requested_capability_bits;
    uint32_t maximum_results;
    uint32_t filter_count;
    uint32_t filter_stride;
    uint32_t filters_offset;
    uint32_t total_size;
    SaccadeAgentFreshness freshness;
} SaccadeAgentQueryRequest;

typedef struct SaccadeAgentQueryCompletion {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentResult result;
    int32_t platform_error;
    SaccadeAgentCapabilityBits granted_capability_bits;
    uint32_t target_count;
    uint32_t target_stride;
    /* Absolute byte offset from the completion start to target records. */
    uint32_t targets_offset;
    uint32_t total_size;
    uint32_t reserved;
    SaccadeAgentGeneration generation;
    SaccadeAgentScope scope;
} SaccadeAgentQueryCompletion;

typedef struct SaccadeAgentPreconditions {
    SaccadeAgentPreconditionFlags flags;
    SaccadeAgentButtonBits expected_buttons;
    SaccadeAgentModifierBits expected_modifiers;
    uint32_t reserved;
    uint64_t generation;
    uint64_t process_id;
    uint64_t window_id;
    uint64_t display_id;
    uint64_t transform_epoch;
    uint64_t permission_epoch;
    uint64_t physical_sequence;
} SaccadeAgentPreconditions;

typedef struct SaccadeAgentAction {
    SaccadeAgentActionKind kind;
    SaccadeAgentActionFlags flags;
    uint64_t target_id;
    uint64_t secondary_target_id;
    SaccadeAgentPointQ8 point;
    SaccadeAgentPointQ8 secondary_point;
    SaccadeAgentButtonBits button_bits;
    SaccadeAgentModifierBits modifiers;
    uint32_t key_usage;
    uint32_t repeat_count;
    int32_t delta_x_q8;
    int32_t delta_y_q8;
    /* Move time for pointer motion, drag, and text selection. Lease time for
       hold and continuous scroll. Zero selects immediate or open-ended input. */
    uint64_t duration_ns;
    uint32_t payload_offset;
    uint32_t payload_size;
} SaccadeAgentAction;

typedef struct SaccadeAgentActionBatch {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentCapabilityBits requested_capability_bits;
    SaccadeAgentBatchPolicy policy;
    uint64_t deadline_ns;
    SaccadeAgentPreconditions preconditions;
    uint32_t action_count;
    uint32_t action_stride;
    uint32_t actions_offset;
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t total_size;
} SaccadeAgentActionBatch;

typedef struct SaccadeAgentActionResult {
    uint32_t action_index;
    SaccadeAgentActionKind kind;
    SaccadeAgentResult result;
    int32_t platform_error;
    uint64_t resolved_target_id;
    uint64_t resolved_secondary_target_id;
    uint64_t validated_generation;
    uint32_t flags;
    uint32_t reserved;
} SaccadeAgentActionResult;

typedef struct SaccadeAgentPhysicalState {
    SaccadeAgentPointQ8 pointer;
    SaccadeAgentButtonBits buttons;
    SaccadeAgentModifierBits modifiers;
    uint64_t active_lease_id;
    uint64_t permission_epoch;
    uint64_t physical_sequence;
    uint32_t flags;
    uint32_t reserved;
} SaccadeAgentPhysicalState;

typedef struct SaccadeAgentActionCompletion {
    SaccadeAgentMessageHeader header;
    uint64_t request_id;
    SaccadeAgentResult result;
    int32_t platform_error;
    uint32_t completed_action_count;
    uint32_t failed_action_index;
    uint32_t action_result_count;
    uint32_t action_result_stride;
    uint32_t action_results_offset;
    uint32_t total_size;
    uint64_t validated_generation;
    SaccadeAgentPhysicalState physical_state;
    /* Populated when next-generation verification has completed. */
    SaccadeAgentGeneration next_generation;
} SaccadeAgentActionCompletion;

#endif

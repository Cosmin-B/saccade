#include "agent/service.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace saccade::agent {
namespace {

/* The agent protocol re-declares the scene target vocabulary as independent
   wire enums, and target() converts between them by plain integer assignment.
   Assert every mirrored constant so numeric drift fails the build.
   SACCADE_AGENT_TARGET_KEYBOARD is agent-only and has no scene counterpart. */
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_UNKNOWN) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_UNKNOWN));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_BUTTON) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_BUTTON));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_LINK) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_LINK));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_TEXT) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_TEXT));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_TEXT_FIELD) ==
              static_cast<uint16_t>(SACCADE_AGENT_ROLE_TEXT_FIELD));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_CHECKBOX) ==
              static_cast<uint16_t>(SACCADE_AGENT_ROLE_CHECKBOX));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_RADIO) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_RADIO));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_MENU_ITEM) ==
              static_cast<uint16_t>(SACCADE_AGENT_ROLE_MENU_ITEM));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_SLIDER) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_SLIDER));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_IMAGE) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_IMAGE));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_ROLE_WINDOW) == static_cast<uint16_t>(SACCADE_AGENT_ROLE_WINDOW));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_POINTER_MOVE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_POINTER_MOVE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_BUTTON) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_CLICK));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_SCROLL) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_SCROLL));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_DRAG_SOURCE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_DRAG_SOURCE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_DROP_TARGET) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_DROP_TARGET));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_TEXT) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_TEXT));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_INVOKE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_INVOKE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_WINDOW_ACTIVATE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_TEXT_SELECT) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_TEXT_SELECT));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_ACTIONABLE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_DISABLED) == static_cast<uint32_t>(SACCADE_AGENT_TARGET_DISABLED));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_OCCLUDED) == static_cast<uint32_t>(SACCADE_AGENT_TARGET_OCCLUDED));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_SECURE) == static_cast<uint32_t>(SACCADE_AGENT_TARGET_SECURE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_APPROXIMATE) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_APPROXIMATE));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_TEXT_REDACTED) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_TEXT_REDACTED));
static_assert(static_cast<uint32_t>(SACCADE_TARGET_TEXT_TRUNCATED) ==
              static_cast<uint32_t>(SACCADE_AGENT_TARGET_TEXT_TRUNCATED));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_SOURCE_NEURAL) ==
              static_cast<uint16_t>(SACCADE_AGENT_TARGET_SOURCE_NEURAL));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_SOURCE_ACCESSIBILITY) ==
              static_cast<uint16_t>(SACCADE_AGENT_TARGET_SOURCE_ACCESSIBILITY));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_SOURCE_PIXEL) ==
              static_cast<uint16_t>(SACCADE_AGENT_TARGET_SOURCE_PIXEL));
static_assert(static_cast<uint16_t>(SACCADE_TARGET_SOURCE_GRID) ==
              static_cast<uint16_t>(SACCADE_AGENT_TARGET_SOURCE_GRID));

template <typename T> bool load_record(SaccadeSpanU8 bytes, size_t offset, T* output) noexcept {
    if (output == nullptr || offset > bytes.size || sizeof(T) > bytes.size - offset) return false;
    std::memcpy(output, bytes.data + offset, sizeof(T));
    return true;
}

bool range_valid(size_t offset, size_t count, size_t stride, size_t total) noexcept {
    return stride != 0 && offset <= total && count <= (total - offset) / stride;
}

SaccadeAgentResult agent_result(SaccadeResult result) noexcept {
    switch (result) {
    case SACCADE_OK:
        return SACCADE_AGENT_OK;
    case SACCADE_ERROR_INVALID_ARGUMENT:
    case SACCADE_ERROR_STATE:
        return SACCADE_AGENT_ERROR_INVALID_MESSAGE;
    case SACCADE_ERROR_CAPACITY:
        return SACCADE_AGENT_ERROR_CAPACITY;
    case SACCADE_ERROR_STALE_HANDLE:
        return SACCADE_AGENT_ERROR_STALE_GENERATION;
    case SACCADE_ERROR_NOT_FOUND:
        return SACCADE_AGENT_ERROR_TARGET_NOT_FOUND;
    case SACCADE_ERROR_PERMISSION:
        return SACCADE_AGENT_ERROR_PERMISSION_DENIED;
    case SACCADE_ERROR_TIMEOUT:
        return SACCADE_AGENT_ERROR_TIMEOUT;
    case SACCADE_ERROR_UNSUPPORTED:
        return SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED;
    default:
        return SACCADE_AGENT_ERROR_BACKEND;
    }
}

SaccadeAgentGeneration generation(const scene::PacketView& scene, const interaction::InteractionState& state) noexcept {
    SaccadeAgentGeneration output{};
    output.generation = scene.header->scene_epoch;
    output.scene_epoch = scene.header->scene_epoch;
    output.frame_id = scene.header->frame_id;
    output.capture_time_ns = scene.header->capture_time_ns;
    output.process_id = state.focus_id;
    output.window_id = state.window_id;
    output.display_id = state.display_id;
    output.transform_epoch = scene.header->transform_epoch;
    output.topology_epoch = scene.header->topology_epoch;
    output.permission_epoch = state.permission_epoch;
    return output;
}

uint32_t source_message_flags(const scene::PacketView& scene) noexcept {
    return (scene.header->flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0 ? SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE : 0;
}

bool freshness_valid(const SaccadeAgentFreshness& freshness) noexcept {
    constexpr uint32_t flag_mask =
        SACCADE_AGENT_FRESHNESS_REQUIRE_DAMAGE_CHECK | SACCADE_AGENT_FRESHNESS_REQUIRE_NEURAL_REFRESH;
    return freshness.policy >= SACCADE_AGENT_FRESHNESS_LATEST_VALID &&
           freshness.policy <= SACCADE_AGENT_FRESHNESS_FORCE_REFRESH && (freshness.flags & ~flag_mask) == 0;
}

bool freshness_supported(const SaccadeAgentFreshness& freshness) noexcept {
    return (freshness.policy == SACCADE_AGENT_FRESHNESS_LATEST_VALID ||
            freshness.policy == SACCADE_AGENT_FRESHNESS_AFTER_GENERATION) &&
           freshness.flags == 0;
}

bool freshness_satisfied(const SaccadeAgentFreshness& freshness, uint64_t generation) noexcept {
    return freshness.policy == SACCADE_AGENT_FRESHNESS_LATEST_VALID || generation > freshness.after_generation;
}

SaccadeAgentScope resolve_scope(SaccadeAgentScope scope, const interaction::InteractionState& state) noexcept {
    if (scope.kind == SACCADE_AGENT_SCOPE_ACTIVE_WINDOW) {
        if (scope.stable_id == 0) scope.stable_id = state.window_id;
        scope.rect = {state.window_bounds.x, state.window_bounds.y, state.window_bounds.width,
                      state.window_bounds.height};
    }
    return scope;
}

SaccadeAgentTarget target(const SaccadeTargetRecord& source, uint32_t source_index, const SaccadeAgentScope& scope,
                          const interaction::InteractionState& state) noexcept {
    SaccadeAgentTarget output{};
    output.target_id = source.target_id;
    output.parent_id = source.parent_id;
    output.window_id = source.window_id;
    output.display_id = source.display_id;
    output.bounds = {source.x_q8, source.y_q8, source.width_q8, source.height_q8};
    output.safe_point = {source.safe_x_q8, source.safe_y_q8};
    output.capability_bits = source.capability_bits;
    output.flags = source.flags;
    output.role = source.role;
    output.source_bits = source.source_bits;
    output.confidence_q16 = source.confidence_q16;
    output.order = source.order;
    output.reserved = source_index;
    if (scope.kind == SACCADE_AGENT_SCOPE_ACTIVE_WINDOW && output.window_id == 0) {
        output.window_id = scope.stable_id;
        if (output.display_id == 0) output.display_id = state.display_id;
    }
    return output;
}

size_t append_target_text(const scene::PacketView& scene, uint32_t targets_offset, uint32_t target_count,
                          uint32_t target_stride, size_t capacity, SaccadeMutableSpanU8 output,
                          uint32_t* message_flags) noexcept {
    size_t text_offset = targets_offset + static_cast<size_t>(target_count) * target_stride;
    for (uint32_t index = 0; index < target_count; ++index) {
        const size_t offset = targets_offset + static_cast<size_t>(index) * target_stride;
        SaccadeAgentTarget value{};
        std::memcpy(&value, output.data + offset, sizeof(value));
        const SaccadeSpanU8 text = scene.target_text(value.reserved);
        if (text.size != 0) {
            if (text.size <= capacity - text_offset) {
                value.text_offset = static_cast<uint32_t>(text_offset);
                value.text_size = static_cast<uint32_t>(text.size);
                std::memcpy(output.data + text_offset, text.data, text.size);
                text_offset += text.size;
            } else {
                value.flags |= SACCADE_AGENT_TARGET_TEXT_TRUNCATED;
                *message_flags |= SACCADE_AGENT_MESSAGE_TRUNCATED;
            }
        }
        value.reserved = 0;
        std::memcpy(output.data + offset, &value, sizeof(value));
    }
    return text_offset;
}

SaccadeResult write_observe_error(const SaccadeAgentObserveRequest& request, SaccadeAgentResult result,
                                  SaccadeResult platform_error, SaccadeAgentCapabilityBits granted,
                                  SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (output.size < sizeof(SaccadeAgentObserveCompletion)) return SACCADE_ERROR_CAPACITY;
    SaccadeAgentObserveCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_COMPLETION;
    completion.request_id = request.request_id;
    completion.result = result;
    completion.platform_error = platform_error;
    completion.granted_capability_bits = granted;
    completion.total_size = static_cast<uint32_t>(sizeof(completion));
    completion.scope = request.scope;
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = sizeof(completion);
    return SACCADE_OK;
}

SaccadeResult write_query_error(const SaccadeAgentQueryRequest& request, SaccadeAgentResult result,
                                SaccadeResult platform_error, SaccadeAgentCapabilityBits granted,
                                SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (output.size < sizeof(SaccadeAgentQueryCompletion)) return SACCADE_ERROR_CAPACITY;
    SaccadeAgentQueryCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_QUERY_COMPLETION;
    completion.request_id = request.request_id;
    completion.result = result;
    completion.platform_error = platform_error;
    completion.granted_capability_bits = granted;
    completion.total_size = static_cast<uint32_t>(sizeof(completion));
    completion.scope = request.scope;
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = sizeof(completion);
    return SACCADE_OK;
}

SaccadeResult write_action_error(const SaccadeAgentActionBatch& batch, SaccadeAgentResult result,
                                 SaccadeResult platform_error, SaccadeMutableSpanU8 output,
                                 size_t* output_size) noexcept {
    if (output.size < sizeof(SaccadeAgentActionCompletion)) return SACCADE_ERROR_CAPACITY;
    SaccadeAgentActionCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_COMPLETION;
    completion.request_id = batch.request_id;
    completion.result = result;
    completion.platform_error = platform_error;
    completion.failed_action_index = UINT32_MAX;
    completion.total_size = static_cast<uint32_t>(sizeof(completion));
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = sizeof(completion);
    return SACCADE_OK;
}

bool intersects(const SaccadeAgentRectQ8& left, const SaccadeTargetRecord& right) noexcept {
    const int64_t left_right = static_cast<int64_t>(left.x_q8) + left.width_q8;
    const int64_t left_bottom = static_cast<int64_t>(left.y_q8) + left.height_q8;
    const int64_t right_right = static_cast<int64_t>(right.x_q8) + right.width_q8;
    const int64_t right_bottom = static_cast<int64_t>(right.y_q8) + right.height_q8;
    return left.width_q8 > 0 && left.height_q8 > 0 && right.width_q8 > 0 && right.height_q8 > 0 &&
           left.x_q8 < right_right && left_right > right.x_q8 && left.y_q8 < right_bottom && left_bottom > right.y_q8;
}

bool scope_matches(const SaccadeAgentScope& scope, const SaccadeTargetRecord& value,
                   const interaction::InteractionState& state) noexcept {
    const bool source_matches =
        scope.source_mode == 0 || scope.source_mode == SACCADE_AGENT_SOURCE_FUSED ||
        (scope.source_mode == SACCADE_AGENT_SOURCE_PIXEL &&
         (value.source_bits & (SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_PIXEL)) != 0) ||
        (scope.source_mode == SACCADE_AGENT_SOURCE_SEMANTIC &&
         (value.source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) != 0) ||
        (scope.source_mode == SACCADE_AGENT_SOURCE_GRID && (value.source_bits & SACCADE_TARGET_SOURCE_GRID) != 0);
    if (!source_matches) return false;
    switch (scope.kind) {
    case SACCADE_AGENT_SCOPE_DESKTOP:
        return true;
    case SACCADE_AGENT_SCOPE_ACTIVE_WINDOW:
        return value.window_id == (scope.stable_id != 0 ? scope.stable_id : state.window_id) ||
               (value.window_id == 0 && intersects(scope.rect, value));
    case SACCADE_AGENT_SCOPE_DISPLAY:
        return value.display_id == scope.stable_id;
    case SACCADE_AGENT_SCOPE_RECT:
        return intersects(scope.rect, value);
    default:
        return false;
    }
}

const SaccadeTargetRecord* find_target(const scene::PacketView& scene, uint64_t target_id) noexcept {
    for (uint32_t index = 0; index < scene.header->target_count; ++index) {
        if (scene.targets[index].target_id == target_id) return &scene.targets[index];
    }
    return nullptr;
}

bool descendant_of(const scene::PacketView& scene, const SaccadeTargetRecord& value, uint64_t ancestor_id) noexcept {
    uint64_t parent_id = value.parent_id;
    for (uint32_t depth = 0; depth < scene.header->target_count && parent_id != 0; ++depth) {
        if (parent_id == ancestor_id) return true;
        const SaccadeTargetRecord* parent = find_target(scene, parent_id);
        if (parent == nullptr || parent->parent_id == parent_id) return false;
        parent_id = parent->parent_id;
    }
    return false;
}

bool relation_matches(const scene::PacketView& scene, const SaccadeTargetRecord& value,
                      const SaccadeAgentQueryFilter& filter) noexcept {
    if ((filter.flags & SACCADE_AGENT_QUERY_RELATION) == 0) return true;
    const SaccadeTargetRecord* related = find_target(scene, filter.relation_target_id);
    if (related == nullptr) return false;
    switch (filter.relation) {
    case SACCADE_AGENT_RELATION_SELF:
        return value.target_id == related->target_id;
    case SACCADE_AGENT_RELATION_PARENT:
        return value.target_id == related->parent_id;
    case SACCADE_AGENT_RELATION_CHILD:
        return value.parent_id == related->target_id;
    case SACCADE_AGENT_RELATION_ANCESTOR:
        return descendant_of(scene, *related, value.target_id);
    case SACCADE_AGENT_RELATION_DESCENDANT:
        return descendant_of(scene, value, related->target_id);
    case SACCADE_AGENT_RELATION_SIBLING:
        return value.target_id != related->target_id && value.parent_id != 0 && value.parent_id == related->parent_id;
    case SACCADE_AGENT_RELATION_CONTAINS:
        return value.x_q8 <= related->x_q8 && value.y_q8 <= related->y_q8 &&
               static_cast<int64_t>(value.x_q8) + value.width_q8 >=
                   static_cast<int64_t>(related->x_q8) + related->width_q8 &&
               static_cast<int64_t>(value.y_q8) + value.height_q8 >=
                   static_cast<int64_t>(related->y_q8) + related->height_q8;
    case SACCADE_AGENT_RELATION_CONTAINED_BY:
        return related->x_q8 <= value.x_q8 && related->y_q8 <= value.y_q8 &&
               static_cast<int64_t>(related->x_q8) + related->width_q8 >=
                   static_cast<int64_t>(value.x_q8) + value.width_q8 &&
               static_cast<int64_t>(related->y_q8) + related->height_q8 >=
                   static_cast<int64_t>(value.y_q8) + value.height_q8;
    default:
        return false;
    }
}

bool text_matches(SaccadeSpanU8 value, SaccadeSpanU8 query, SaccadeAgentTextMatch match) noexcept {
    if (value.size == 0) return false;
    if (match == SACCADE_AGENT_TEXT_EXACT)
        return value.size == query.size && std::memcmp(value.data, query.data, query.size) == 0;
    if (match == SACCADE_AGENT_TEXT_PREFIX)
        return value.size >= query.size && std::memcmp(value.data, query.data, query.size) == 0;
    if (value.size < query.size) return false;
    for (size_t offset = 0; offset <= value.size - query.size; ++offset) {
        if (std::memcmp(value.data + offset, query.data, query.size) == 0) return true;
    }
    return false;
}

bool filter_matches(const scene::PacketView& scene, uint32_t target_index, const SaccadeTargetRecord& value,
                    const SaccadeAgentQueryFilter& filter, SaccadeSpanU8 request) noexcept {
    if ((filter.flags & SACCADE_AGENT_QUERY_TEXT) != 0 &&
        !text_matches(scene.target_text(target_index), {request.data + filter.text_offset, filter.text_size},
                      filter.text_match))
        return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_STABLE_ID) != 0 && value.target_id != filter.target_id) return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_ROLE) != 0 && value.role != filter.role) return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_SOURCE) != 0 && (value.source_bits & filter.source_bits) == 0) return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_CAPABILITY) != 0 &&
        ((value.capability_bits & filter.required_capability_bits) != filter.required_capability_bits ||
         (value.capability_bits & filter.forbidden_capability_bits) != 0))
        return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_GEOMETRY) != 0 && !intersects(filter.geometry, value)) return false;
    if ((filter.flags & SACCADE_AGENT_QUERY_CONFIDENCE) != 0 && value.confidence_q16 < filter.minimum_confidence_q16)
        return false;
    return relation_matches(scene, value, filter);
}

SaccadeAgentCapabilityBits required_capability(SaccadeAgentActionKind kind) noexcept {
    switch (kind) {
    case SACCADE_AGENT_ACTION_POINTER_MOVE:
    case SACCADE_AGENT_ACTION_POINTER_HOVER:
    case SACCADE_AGENT_ACTION_CLICK:
    case SACCADE_AGENT_ACTION_HOLD:
    case SACCADE_AGENT_ACTION_DRAG_DROP:
    case SACCADE_AGENT_ACTION_SCROLL:
    case SACCADE_AGENT_ACTION_INVOKE:
    case SACCADE_AGENT_ACTION_RELEASE:
    case SACCADE_AGENT_ACTION_TEXT_SELECT:
        return SACCADE_AGENT_CAPABILITY_POINTER;
    case SACCADE_AGENT_ACTION_KEY:
    case SACCADE_AGENT_ACTION_KEY_CHORD:
        return SACCADE_AGENT_CAPABILITY_KEYBOARD;
    case SACCADE_AGENT_ACTION_TEXT:
        return SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_POINTER;
    case SACCADE_AGENT_ACTION_WINDOW_ACTIVATE:
    case SACCADE_AGENT_ACTION_WINDOW_CYCLE:
        return SACCADE_AGENT_CAPABILITY_WINDOW;
    case SACCADE_AGENT_ACTION_ABORT:
        return SACCADE_AGENT_CAPABILITY_POINTER;
    case SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE:
        return SACCADE_AGENT_CAPABILITY_OBSERVE;
    default:
        return 0;
    }
}

bool action_kind(SaccadeAgentActionKind kind, interaction::ActionKind* output) noexcept {
    switch (kind) {
    case SACCADE_AGENT_ACTION_POINTER_MOVE:
    case SACCADE_AGENT_ACTION_POINTER_HOVER:
        *output = interaction::ActionKind::pointer_move;
        return true;
    case SACCADE_AGENT_ACTION_CLICK:
        *output = interaction::ActionKind::click;
        return true;
    case SACCADE_AGENT_ACTION_INVOKE:
        *output = interaction::ActionKind::invoke;
        return true;
    case SACCADE_AGENT_ACTION_HOLD:
        *output = interaction::ActionKind::hold;
        return true;
    case SACCADE_AGENT_ACTION_DRAG_DROP:
        *output = interaction::ActionKind::drag;
        return true;
    case SACCADE_AGENT_ACTION_SCROLL:
        *output = interaction::ActionKind::scroll;
        return true;
    case SACCADE_AGENT_ACTION_KEY:
    case SACCADE_AGENT_ACTION_KEY_CHORD:
        *output = interaction::ActionKind::key;
        return true;
    case SACCADE_AGENT_ACTION_TEXT:
        *output = interaction::ActionKind::text;
        return true;
    case SACCADE_AGENT_ACTION_WINDOW_ACTIVATE:
        *output = interaction::ActionKind::window_activate;
        return true;
    case SACCADE_AGENT_ACTION_RELEASE:
        *output = interaction::ActionKind::release;
        return true;
    case SACCADE_AGENT_ACTION_TEXT_SELECT:
        *output = interaction::ActionKind::text_select;
        return true;
    default:
        return false;
    }
}

bool action_supported(const SaccadeAgentAction& action) noexcept {
    if (action.duration_ns != 0) return false;
    constexpr uint32_t point_flags = SACCADE_AGENT_ACTION_EXPLICIT_POINTS;
    constexpr uint32_t cycle_flags = SACCADE_AGENT_ACTION_CYCLE_BACKWARD;
    switch (action.kind) {
    case SACCADE_AGENT_ACTION_POINTER_MOVE:
    case SACCADE_AGENT_ACTION_POINTER_HOVER:
    case SACCADE_AGENT_ACTION_CLICK:
    case SACCADE_AGENT_ACTION_HOLD:
    case SACCADE_AGENT_ACTION_DRAG_DROP:
    case SACCADE_AGENT_ACTION_SCROLL:
    case SACCADE_AGENT_ACTION_TEXT_SELECT:
    case SACCADE_AGENT_ACTION_INVOKE:
        return (action.flags & ~point_flags) == 0 && action.payload_size == 0;
    case SACCADE_AGENT_ACTION_KEY:
    case SACCADE_AGENT_ACTION_KEY_CHORD:
    case SACCADE_AGENT_ACTION_WINDOW_ACTIVATE:
    case SACCADE_AGENT_ACTION_RELEASE:
        return action.flags == 0 && action.payload_size == 0;
    case SACCADE_AGENT_ACTION_TEXT:
        return (action.flags & ~point_flags) == 0 && action.payload_size != 0;
    case SACCADE_AGENT_ACTION_WINDOW_CYCLE:
        return (action.flags & ~cycle_flags) == 0 && action.payload_size == 0;
    case SACCADE_AGENT_ACTION_ABORT:
    case SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE:
        return action.flags == 0 && action.target_id == 0 && action.secondary_target_id == 0 &&
               action.payload_size == 0;
    default:
        return false;
    }
}

} // namespace

SaccadeResult Service::initialize(ServiceConfig config) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.context == nullptr || config.acquire_scene == nullptr || config.read_state == nullptr ||
        config.execute_plan == nullptr || config.read_physical_state == nullptr || config.abort_input == nullptr ||
        config.cycle_window == nullptr || config.capability_bits == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    config_ = config;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult Service::process(SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                               SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (!initialized_ || request.data == nullptr || output.data == nullptr || output_size == nullptr || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output_size = 0;
    SaccadeAgentMessageHeader header{};
    if (!load_record(request, 0, &header) || header.api_version != SACCADE_AGENT_API_VERSION) {
        ++stats_.rejected_messages;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    ++stats_.requests;
    switch (header.message_kind) {
    case SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST: {
        SaccadeAgentObserveRequest value{};
        if (!load_record(request, 0, &value) || value.header.struct_size != sizeof(value))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        return observe(value, client_capabilities, output, output_size);
    }
    case SACCADE_AGENT_MESSAGE_QUERY_REQUEST: {
        SaccadeAgentQueryRequest value{};
        if (!load_record(request, 0, &value) || value.header.struct_size != sizeof(value))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        return query(request, value, client_capabilities, output, output_size);
    }
    case SACCADE_AGENT_MESSAGE_ACTION_BATCH: {
        SaccadeAgentActionBatch value{};
        if (!load_record(request, 0, &value) || value.header.struct_size != sizeof(value))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        return act(request, value, client_capabilities, now_ns, output, output_size);
    }
    default:
        ++stats_.rejected_messages;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
}

SaccadeResult Service::observe(const SaccadeAgentObserveRequest& request,
                               SaccadeAgentCapabilityBits client_capabilities, SaccadeMutableSpanU8 output,
                               size_t* output_size) noexcept {
    const SaccadeAgentCapabilityBits available = config_.capability_bits & client_capabilities;
    if (output.size < sizeof(SaccadeAgentObserveCompletion)) return SACCADE_ERROR_CAPACITY;
    if ((request.requested_capability_bits & ~available) != 0 ||
        (request.requested_capability_bits & SACCADE_AGENT_CAPABILITY_OBSERVE) == 0) {
        ++stats_.rejected_capabilities;
        return write_observe_error(request, SACCADE_AGENT_ERROR_CAPABILITY_DENIED, SACCADE_ERROR_PERMISSION, available,
                                   output, output_size);
    }
    if (request.maximum_targets == 0 || request.maximum_targets > SACCADE_AGENT_MAX_TARGETS ||
        request.target_stride < sizeof(SaccadeAgentTarget) || request.total_capacity > output.size ||
        request.total_capacity < sizeof(SaccadeAgentObserveCompletion))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!freshness_valid(request.freshness)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!freshness_supported(request.freshness))
        return write_observe_error(request, SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, SACCADE_ERROR_UNSUPPORTED,
                                   available, output, output_size);

    scene::PacketView scene{};
    interaction::InteractionState state{};
    SaccadeResult result = config_.acquire_scene(config_.context, &scene);
    if (result == SACCADE_OK) result = config_.read_state(config_.context, &state);
    if (result != SACCADE_OK)
        return write_observe_error(request, agent_result(result), result, available, output, output_size);
    if (!freshness_satisfied(request.freshness, scene.header->scene_epoch))
        return write_observe_error(request, SACCADE_AGENT_ERROR_TIMEOUT, SACCADE_ERROR_TIMEOUT, available, output,
                                   output_size);
    const size_t target_capacity =
        std::min<size_t>(request.maximum_targets,
                         (request.total_capacity - sizeof(SaccadeAgentObserveCompletion)) / request.target_stride);
    SaccadeAgentObserveCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_COMPLETION;
    completion.header.flags = source_message_flags(scene);
    completion.request_id = request.request_id;
    completion.result = SACCADE_AGENT_OK;
    completion.granted_capability_bits = available;
    completion.target_stride = request.target_stride;
    completion.targets_offset = static_cast<uint32_t>(sizeof(completion));
    completion.generation = generation(scene, state);
    completion.scope = resolve_scope(request.scope, state);
    for (uint32_t index = 0; index < scene.header->target_count; ++index) {
        const SaccadeTargetRecord& source = scene.targets[index];
        if (!scope_matches(completion.scope, source, state)) continue;
        if (completion.target_count == target_capacity) {
            completion.header.flags |= SACCADE_AGENT_MESSAGE_TRUNCATED;
            break;
        }
        const SaccadeAgentTarget value = target(source, index, completion.scope, state);
        const size_t offset =
            completion.targets_offset + static_cast<size_t>(completion.target_count) * completion.target_stride;
        std::memset(output.data + offset, 0, completion.target_stride);
        std::memcpy(output.data + offset, &value, sizeof(value));
        ++completion.target_count;
    }
    completion.total_size = static_cast<uint32_t>(
        append_target_text(scene, completion.targets_offset, completion.target_count, completion.target_stride,
                           request.total_capacity, output, &completion.header.flags));
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = completion.total_size;
    ++stats_.observations;
    return SACCADE_OK;
}

SaccadeResult Service::query(SaccadeSpanU8 bytes, const SaccadeAgentQueryRequest& request,
                             SaccadeAgentCapabilityBits client_capabilities, SaccadeMutableSpanU8 output,
                             size_t* output_size) noexcept {
    const SaccadeAgentCapabilityBits available = config_.capability_bits & client_capabilities;
    if (output.size < sizeof(SaccadeAgentQueryCompletion)) return SACCADE_ERROR_CAPACITY;
    if ((request.requested_capability_bits & ~available) != 0 ||
        (request.requested_capability_bits & SACCADE_AGENT_CAPABILITY_OBSERVE) == 0) {
        ++stats_.rejected_capabilities;
        return write_query_error(request, SACCADE_AGENT_ERROR_CAPABILITY_DENIED, SACCADE_ERROR_PERMISSION, available,
                                 output, output_size);
    }
    if (request.maximum_results == 0 || request.maximum_results > SACCADE_AGENT_MAX_TARGETS ||
        request.filter_count > SACCADE_AGENT_MAX_FILTERS || request.filter_stride < sizeof(SaccadeAgentQueryFilter) ||
        request.total_size > bytes.size ||
        (request.filter_count != 0 && request.filters_offset < sizeof(SaccadeAgentQueryRequest)) ||
        !range_valid(request.filters_offset, request.filter_count, request.filter_stride, request.total_size))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!freshness_valid(request.freshness)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!freshness_supported(request.freshness))
        return write_query_error(request, SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, SACCADE_ERROR_UNSUPPORTED, available,
                                 output, output_size);
    const size_t required =
        sizeof(SaccadeAgentQueryCompletion) + static_cast<size_t>(request.maximum_results) * sizeof(SaccadeAgentTarget);
    if (required > output.size) return SACCADE_ERROR_CAPACITY;
    const size_t filters_end =
        request.filters_offset + static_cast<size_t>(request.filter_count) * request.filter_stride;
    for (uint32_t index = 0; index < request.filter_count; ++index) {
        SaccadeAgentQueryFilter filter{};
        const size_t offset = request.filters_offset + static_cast<size_t>(index) * request.filter_stride;
        if (!load_record(bytes, offset, &filter)) return SACCADE_ERROR_INVALID_ARGUMENT;
        if ((filter.flags & SACCADE_AGENT_QUERY_RELATION) != 0 && filter.relation == SACCADE_AGENT_RELATION_NEAREST)
            return write_query_error(request, SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, SACCADE_ERROR_UNSUPPORTED,
                                     available, output, output_size);
        if ((filter.flags & SACCADE_AGENT_QUERY_RELATION) != 0 &&
            (filter.relation < SACCADE_AGENT_RELATION_SELF || filter.relation > SACCADE_AGENT_RELATION_NEAREST))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if ((filter.flags & SACCADE_AGENT_QUERY_TEXT) != 0 &&
            (filter.text_size == 0 || filter.text_size > SACCADE_AGENT_MAX_TEXT_BYTES ||
             filter.text_offset < filters_end || filter.text_offset > request.total_size ||
             filter.text_size > request.total_size - filter.text_offset ||
             (filter.text_match != SACCADE_AGENT_TEXT_EXACT && filter.text_match != SACCADE_AGENT_TEXT_PREFIX &&
              filter.text_match != SACCADE_AGENT_TEXT_SUBSTRING) ||
             !scene::valid_utf8({bytes.data + filter.text_offset, filter.text_size})))
            return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    scene::PacketView scene{};
    interaction::InteractionState state{};
    SaccadeResult result = config_.acquire_scene(config_.context, &scene);
    if (result == SACCADE_OK) result = config_.read_state(config_.context, &state);
    if (result != SACCADE_OK)
        return write_query_error(request, agent_result(result), result, available, output, output_size);
    if (!freshness_satisfied(request.freshness, scene.header->scene_epoch))
        return write_query_error(request, SACCADE_AGENT_ERROR_TIMEOUT, SACCADE_ERROR_TIMEOUT, available, output,
                                 output_size);
    if (request.generation != 0 && request.generation != scene.header->scene_epoch) {
        ++stats_.rejected_stale;
        return write_query_error(request, SACCADE_AGENT_ERROR_STALE_GENERATION, SACCADE_ERROR_STALE_HANDLE, available,
                                 output, output_size);
    }

    SaccadeAgentQueryCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_QUERY_COMPLETION;
    completion.header.flags = source_message_flags(scene);
    completion.request_id = request.request_id;
    completion.result = SACCADE_AGENT_OK;
    completion.granted_capability_bits = available;
    completion.target_stride = static_cast<uint32_t>(sizeof(SaccadeAgentTarget));
    completion.targets_offset = static_cast<uint32_t>(sizeof(completion));
    completion.generation = generation(scene, state);
    completion.scope = resolve_scope(request.scope, state);
    for (uint32_t target_index = 0; target_index < scene.header->target_count; ++target_index) {
        const SaccadeTargetRecord& source = scene.targets[target_index];
        if (!scope_matches(completion.scope, source, state)) continue;
        bool matched = true;
        for (uint32_t filter_index = 0; filter_index < request.filter_count; ++filter_index) {
            SaccadeAgentQueryFilter filter{};
            const size_t offset = request.filters_offset + static_cast<size_t>(filter_index) * request.filter_stride;
            if (!load_record(bytes, offset, &filter) || !filter_matches(scene, target_index, source, filter, bytes)) {
                matched = false;
                break;
            }
        }
        if (!matched) continue;
        if (completion.target_count == request.maximum_results) {
            completion.header.flags |= SACCADE_AGENT_MESSAGE_TRUNCATED;
            break;
        }
        const SaccadeAgentTarget value = target(source, target_index, completion.scope, state);
        const size_t offset =
            completion.targets_offset + static_cast<size_t>(completion.target_count) * completion.target_stride;
        std::memset(output.data + offset, 0, completion.target_stride);
        std::memcpy(output.data + offset, &value, sizeof(value));
        ++completion.target_count;
    }
    completion.total_size = static_cast<uint32_t>(append_target_text(scene, completion.targets_offset,
                                                                     completion.target_count, completion.target_stride,
                                                                     output.size, output, &completion.header.flags));
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = completion.total_size;
    ++stats_.queries;
    return SACCADE_OK;
}

SaccadeResult Service::act(SaccadeSpanU8 bytes, const SaccadeAgentActionBatch& batch,
                           SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns, SaccadeMutableSpanU8 output,
                           size_t* output_size) noexcept {
    const SaccadeAgentCapabilityBits available = config_.capability_bits & client_capabilities;
    if (output.size < sizeof(SaccadeAgentActionCompletion)) return SACCADE_ERROR_CAPACITY;
    if ((batch.requested_capability_bits & ~available) != 0) {
        ++stats_.rejected_capabilities;
        return write_action_error(batch, SACCADE_AGENT_ERROR_CAPABILITY_DENIED, SACCADE_ERROR_PERMISSION, output,
                                  output_size);
    }
    if (batch.action_count == 0 || batch.action_count > SACCADE_AGENT_MAX_ACTIONS ||
        batch.action_stride < sizeof(SaccadeAgentAction) || batch.total_size > bytes.size ||
        batch.actions_offset < sizeof(SaccadeAgentActionBatch) ||
        !range_valid(batch.actions_offset, batch.action_count, batch.action_stride, batch.total_size) ||
        (batch.payload_size != 0 &&
         batch.payload_offset < batch.actions_offset + static_cast<size_t>(batch.action_count) * batch.action_stride) ||
        batch.payload_offset > batch.total_size || batch.payload_size > batch.total_size - batch.payload_offset ||
        batch.payload_size > SACCADE_AGENT_MAX_PAYLOAD_BYTES || batch.deadline_ns <= now_ns)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    constexpr uint32_t batch_flag_mask = SACCADE_AGENT_BATCH_DRY_RUN | SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION;
    if ((batch.header.flags & ~batch_flag_mask) != 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    if ((batch.header.flags & SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION) != 0 &&
        (available & SACCADE_AGENT_CAPABILITY_OBSERVE) == 0)
        return write_action_error(batch, SACCADE_AGENT_ERROR_CAPABILITY_DENIED, SACCADE_ERROR_PERMISSION, output,
                                  output_size);
    const size_t completion_size = sizeof(SaccadeAgentActionCompletion) +
                                   static_cast<size_t>(batch.action_count) * sizeof(SaccadeAgentActionResult);
    if (completion_size > output.size) return SACCADE_ERROR_CAPACITY;

    scene::PacketView scene{};
    interaction::InteractionState state{};
    SaccadeResult result = config_.acquire_scene(config_.context, &scene);
    if (result == SACCADE_OK) result = config_.read_state(config_.context, &state);
    SaccadeAgentPhysicalState physical{};
    if (result == SACCADE_OK) result = config_.read_physical_state(config_.context, &physical);
    if (result != SACCADE_OK) return write_action_error(batch, agent_result(result), result, output, output_size);
    const SaccadeAgentPreconditions& preconditions = batch.preconditions;
    if (((preconditions.flags & SACCADE_AGENT_PRECONDITION_GENERATION) != 0 &&
         preconditions.generation != scene.header->scene_epoch) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_PROCESS) != 0 &&
         preconditions.process_id != state.focus_id) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_WINDOW) != 0 &&
         preconditions.window_id != state.window_id) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_DISPLAY) != 0 &&
         preconditions.display_id != state.display_id) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_TRANSFORM) != 0 &&
         preconditions.transform_epoch != scene.header->transform_epoch) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_PERMISSION) != 0 &&
         preconditions.permission_epoch != state.permission_epoch) ||
        ((preconditions.flags & SACCADE_AGENT_PRECONDITION_PHYSICAL_STATE) != 0 &&
         (preconditions.expected_buttons != physical.buttons ||
          preconditions.expected_modifiers != physical.modifiers ||
          preconditions.physical_sequence != physical.physical_sequence))) {
        ++stats_.rejected_stale;
        return write_action_error(batch, SACCADE_AGENT_ERROR_STALE_GENERATION, SACCADE_ERROR_STALE_HANDLE, output,
                                  output_size);
    }

    SaccadeAgentActionCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_COMPLETION;
    completion.request_id = batch.request_id;
    completion.result = SACCADE_AGENT_OK;
    completion.failed_action_index = UINT32_MAX;
    completion.action_result_stride = static_cast<uint32_t>(sizeof(SaccadeAgentActionResult));
    completion.action_results_offset = static_cast<uint32_t>(sizeof(completion));
    completion.validated_generation = scene.header->scene_epoch;
    const uint64_t initial_generation = scene.header->scene_epoch;
    const uint64_t initial_focus_id = state.focus_id;
    const uint64_t initial_window_id = state.window_id;
    const uint64_t initial_display_id = state.display_id;
    const uint64_t initial_permission_epoch = state.permission_epoch;
    const SaccadeAgentCapabilityBits requested = available & batch.requested_capability_bits;

    for (uint32_t index = 0; index < batch.action_count; ++index) {
        if (index != 0) {
            scene::PacketView current_scene{};
            result = config_.acquire_scene(config_.context, &current_scene);
            if (result == SACCADE_OK) result = config_.read_state(config_.context, &state);
            if (result == SACCADE_OK &&
                (current_scene.header->scene_epoch != initial_generation || state.focus_id != initial_focus_id ||
                 state.window_id != initial_window_id || state.display_id != initial_display_id ||
                 state.permission_epoch != initial_permission_epoch))
                result = SACCADE_ERROR_STALE_HANDLE;
            if (result != SACCADE_OK) {
                completion.result = agent_result(result);
                completion.failed_action_index = index;
                break;
            }
            scene = current_scene;
        }
        SaccadeAgentAction action{};
        const size_t action_offset = batch.actions_offset + static_cast<size_t>(index) * batch.action_stride;
        if (!load_record(bytes, action_offset, &action)) return SACCADE_ERROR_INVALID_ARGUMENT;
        SaccadeAgentActionResult action_result{};
        action_result.action_index = index;
        action_result.kind = action.kind;
        action_result.resolved_target_id = action.target_id;
        action_result.resolved_secondary_target_id = action.secondary_target_id;
        action_result.validated_generation = scene.header->scene_epoch;
        const SaccadeAgentCapabilityBits required = required_capability(action.kind);
        interaction::ActionKind kind{};
        const bool planned = action_kind(action.kind, &kind);
        result = SACCADE_OK;
        if (!action_supported(action)) {
            action_result.result = SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED;
            result = SACCADE_ERROR_UNSUPPORTED;
        } else if (required == 0 || (requested & required) != required) {
            action_result.result =
                required == 0 ? SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED : SACCADE_AGENT_ERROR_CAPABILITY_DENIED;
            result = required == 0 ? SACCADE_ERROR_UNSUPPORTED : SACCADE_ERROR_PERMISSION;
        } else if (action.kind == SACCADE_AGENT_ACTION_ABORT) {
            result = config_.abort_input(config_.context);
            action_result.result = agent_result(result);
        } else if (action.kind == SACCADE_AGENT_ACTION_WINDOW_CYCLE) {
            result = config_.cycle_window(config_.context, (action.flags & SACCADE_AGENT_ACTION_CYCLE_BACKWARD) != 0);
            action_result.result = agent_result(result);
        } else if (action.kind == SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE) {
            result = config_.read_physical_state(config_.context, &physical);
            action_result.result = agent_result(result);
        } else if (!planned) {
            action_result.result = SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED;
            result = SACCADE_ERROR_UNSUPPORTED;
        } else {
            std::array<uint64_t, 2> target_ids{action.target_id, action.secondary_target_id};
            std::array<geometry::PointQ8, 2> points{
                {{action.point.x_q8, action.point.y_q8}, {action.secondary_point.x_q8, action.secondary_point.y_q8}}};
            interaction::ActionRequest request{};
            request.kind = kind;
            request.button = action.button_bits;
            request.repeat_count = action.repeat_count == 0 ? 1U : action.repeat_count;
            request.key_usage = action.key_usage;
            request.modifiers = action.modifiers;
            request.delta_x_q8 = action.delta_x_q8;
            request.delta_y_q8 = action.delta_y_q8;
            request.duration_ns = action.duration_ns;
            if (kind != interaction::ActionKind::key && kind != interaction::ActionKind::release) {
                request.target_ids = target_ids.data();
                request.target_count =
                    kind == interaction::ActionKind::drag || kind == interaction::ActionKind::text_select ? 2U : 1U;
            }
            if ((action.flags & SACCADE_AGENT_ACTION_EXPLICIT_POINTS) != 0) {
                request.target_points = points.data();
                request.target_point_count = request.target_count;
            }
            if (action.payload_size != 0) {
                if (action.payload_offset > batch.payload_size ||
                    action.payload_size > batch.payload_size - action.payload_offset) {
                    result = SACCADE_ERROR_INVALID_ARGUMENT;
                } else {
                    request.text = {bytes.data + batch.payload_offset + action.payload_offset, action.payload_size};
                }
            }
            if (result == SACCADE_OK) {
                interaction::ActionContext context{};
                context.plan_id = next_plan_id_++;
                if (next_plan_id_ == 0) ++next_plan_id_;
                context.scene_epoch = scene.header->scene_epoch;
                context.transform_epoch = scene.header->transform_epoch;
                context.topology_epoch = scene.header->topology_epoch;
                context.permission_epoch = state.permission_epoch;
                context.focus_id = state.focus_id;
                context.window_id = state.window_id;
                context.display_id = state.display_id;
                context.now_ns = now_ns;
                context.deadline_ns = batch.deadline_ns;
                context.permissions = state.permissions;
                context.expected_buttons = state.expected_buttons;
                context.plan_flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
                if ((batch.header.flags & SACCADE_AGENT_BATCH_DRY_RUN) != 0)
                    context.plan_flags |= SACCADE_INPUT_PLAN_DRY_RUN;
                SaccadeSpanU8 plan{};
                result = planner_.build(scene, context, request, &plan_storage_, &plan);
                if (result == SACCADE_OK)
                    result = config_.execute_plan(config_.context, plan, state.permissions, now_ns);
            }
            action_result.result = agent_result(result);
        }
        const size_t result_offset =
            completion.action_results_offset + static_cast<size_t>(index) * completion.action_result_stride;
        std::memcpy(output.data + result_offset, &action_result, sizeof(action_result));
        ++completion.action_result_count;
        ++stats_.actions;
        if (result == SACCADE_OK) {
            ++completion.completed_action_count;
            continue;
        }
        completion.result = action_result.result;
        completion.failed_action_index = index;
        if (batch.policy != SACCADE_AGENT_BATCH_CONTINUE_ON_FAILURE) break;
    }
    if (config_.read_physical_state(config_.context, &completion.physical_state) != SACCADE_OK)
        completion.result = SACCADE_AGENT_ERROR_BACKEND;
    if (completion.result == SACCADE_AGENT_OK &&
        (batch.header.flags & SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION) != 0) {
        scene::PacketView next_scene{};
        interaction::InteractionState next_state{};
        result = config_.acquire_scene(config_.context, &next_scene);
        if (result == SACCADE_OK) result = config_.read_state(config_.context, &next_state);
        if (result == SACCADE_OK && next_scene.header->scene_epoch > initial_generation) {
            completion.header.flags |= SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE;
            completion.next_generation = generation(next_scene, next_state);
        } else {
            completion.result = result == SACCADE_OK ? SACCADE_AGENT_ERROR_TIMEOUT : agent_result(result);
            completion.platform_error = result == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : result;
        }
    }
    completion.total_size =
        completion.action_results_offset + completion.action_result_count * completion.action_result_stride;
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = completion.total_size;
    ++stats_.action_batches;
    if (completion.result != SACCADE_AGENT_OK) ++stats_.failures;
    return SACCADE_OK;
}

SaccadeResult Service::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    config_ = {};
    initialized_ = false;
    return SACCADE_OK;
}

} // namespace saccade::agent

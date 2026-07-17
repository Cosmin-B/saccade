#include "interaction/action_planner.hpp"

#include <cstdint>
#include <cstring>

namespace saccade::interaction {
namespace {

constexpr uint32_t button_mask = SACCADE_INPUT_BUTTON_LEFT | SACCADE_INPUT_BUTTON_RIGHT | SACCADE_INPUT_BUTTON_MIDDLE;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;
constexpr uint32_t permission_mask = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD |
                                     SACCADE_INPUT_PERMISSION_TEXT | SACCADE_INPUT_PERMISSION_WINDOW |
                                     SACCADE_INPUT_PERMISSION_CLIPBOARD;

const SaccadeTargetRecord* find_target(const scene::PacketView& scene, uint64_t target_id) noexcept {
    for (uint32_t index = 0; index < scene.header->target_count; ++index) {
        if (scene.targets[index].target_id == target_id) {
            return &scene.targets[index];
        }
    }
    return nullptr;
}

bool target_safe(const SaccadeTargetRecord& target, uint32_t capabilities) noexcept {
    return (target.flags & SACCADE_TARGET_ACTIONABLE) != 0 &&
           (target.flags & (SACCADE_TARGET_DISABLED | SACCADE_TARGET_OCCLUDED | SACCADE_TARGET_SECURE)) == 0 &&
           (target.capability_bits & capabilities) == capabilities;
}

bool point_inside_target(const SaccadeTargetRecord& target, geometry::PointQ8 point) noexcept {
    const int64_t right = static_cast<int64_t>(target.x_q8) + target.width_q8;
    const int64_t bottom = static_cast<int64_t>(target.y_q8) + target.height_q8;
    return point.x >= target.x_q8 && point.y >= target.y_q8 && static_cast<int64_t>(point.x) < right &&
           static_cast<int64_t>(point.y) < bottom;
}

uint32_t required_capability(ActionKind kind, bool destination) noexcept {
    switch (kind) {
    case ActionKind::pointer_move:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    case ActionKind::click:
    case ActionKind::hold:
        return SACCADE_TARGET_CAPABILITY_BUTTON;
    case ActionKind::invoke:
        return SACCADE_TARGET_CAPABILITY_INVOKE;
    case ActionKind::drag:
        return destination ? SACCADE_TARGET_CAPABILITY_DROP_TARGET : SACCADE_TARGET_CAPABILITY_DRAG_SOURCE;
    case ActionKind::scroll:
        return SACCADE_TARGET_CAPABILITY_SCROLL;
    case ActionKind::text:
        return SACCADE_TARGET_CAPABILITY_TEXT;
    case ActionKind::text_select:
        return SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    case ActionKind::window_activate:
        return SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
    case ActionKind::key:
    case ActionKind::release:
        return 0;
    }
    return 0;
}

bool one_button(uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0 && (value & ~button_mask) == 0;
}

bool request_shape_valid(const ActionRequest& request) noexcept {
    if (request.kind < ActionKind::pointer_move || request.kind > ActionKind::invoke ||
        request.target_count > maximum_action_targets || request.repeat_count == 0 || request.repeat_count > 16 ||
        (request.modifiers & ~modifier_mask) != 0) {
        return false;
    }

    if (request.reserved[0] != 0 || request.reserved[1] != 0 || request.reserved[2] != 0 || request.reserved[3] != 0 ||
        request.reserved[4] != 0 || request.reserved[5] != 0)
        return false;

    if ((request.target_points == nullptr) != (request.target_point_count == 0) ||
        (request.target_points != nullptr && request.target_point_count != request.target_count))
        return false;

    const bool has_targets = request.target_ids != nullptr && request.target_count != 0;

    switch (request.kind) {
    case ActionKind::key:
        return request.target_count == 0 && request.key_usage != 0 && request.text.size == 0;
    case ActionKind::release:
        return request.target_count == 0 && one_button(request.button) && request.text.size == 0;
    case ActionKind::drag:
    case ActionKind::text_select:
        return has_targets && request.target_count == 2 && one_button(request.button);
    case ActionKind::click:
        return has_targets && one_button(request.button);
    case ActionKind::invoke:
        return has_targets && request.target_count == 1 && one_button(request.button);
    case ActionKind::hold:
        return has_targets && request.target_count == 1 && one_button(request.button);
    case ActionKind::scroll:
        return has_targets && request.target_count == 1 && (request.delta_x_q8 != 0 || request.delta_y_q8 != 0);
    case ActionKind::text:
        return has_targets && request.target_count == 1 && request.text.data != nullptr && request.text.size != 0 &&
               request.text.size <= maximum_action_payload_bytes;
    case ActionKind::pointer_move:
    case ActionKind::window_activate:
        return has_targets && request.target_count == 1;
    }

    return false;
}

SaccadeInputCommand target_command(SaccadeInputCommandKind kind, const SaccadeTargetRecord& target) noexcept {
    SaccadeInputCommand command{};
    command.kind = kind;
    command.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    command.target_id = target.target_id;
    command.x_q8 = target.safe_x_q8;
    command.y_q8 = target.safe_y_q8;
    return command;
}

} // namespace

SaccadeResult ActionPlanner::build(const scene::PacketView& scene, const ActionContext& context,
                                   const ActionRequest& request, ActionPlanStorage* storage,
                                   SaccadeSpanU8* output) noexcept {
    if (storage == nullptr || output == nullptr || scene.header == nullptr || scene.targets == nullptr ||
        context.plan_id == 0 || context.permission_epoch == 0 || context.deadline_ns == 0 ||
        context.now_ns >= context.deadline_ns || (context.permissions & ~permission_mask) != 0 ||
        (context.expected_buttons & ~button_mask) != 0 || context.reserved != 0 || !request_shape_valid(request)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *output = {};

    if (scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        scene.header->scene_epoch != context.scene_epoch || scene.header->transform_epoch != context.transform_epoch ||
        scene.header->topology_epoch != context.topology_epoch) {
        ++stats_.rejected_stale;
        return SACCADE_ERROR_STALE_HANDLE;
    }

    std::array<SaccadeTargetRecord, maximum_action_targets> resolved_targets{};
    std::array<const SaccadeTargetRecord*, maximum_action_targets> targets{};

    for (uint32_t index = 0; index < request.target_count; ++index) {
        const SaccadeTargetRecord* source = find_target(scene, request.target_ids[index]);
        const uint32_t capability = required_capability(request.kind, request.kind == ActionKind::drag && index == 1);

        if (source == nullptr || !target_safe(*source, capability)) {
            ++stats_.rejected_unsafe;
            return SACCADE_ERROR_NOT_FOUND;
        }

        resolved_targets[index] = *source;
        if (request.target_points != nullptr) {
            const geometry::PointQ8 point = request.target_points[index];
            if (!request.allow_point_outside_target && !point_inside_target(*source, point)) {
                ++stats_.rejected_unsafe;
                return SACCADE_ERROR_NOT_FOUND;
            }
            resolved_targets[index].safe_x_q8 = point.x;
            resolved_targets[index].safe_y_q8 = point.y;
        }

        targets[index] = &resolved_targets[index];
    }

    auto* header = reinterpret_cast<SaccadeInputPlanHeader*>(storage->bytes.data());
    auto* commands = reinterpret_cast<SaccadeInputCommand*>(storage->bytes.data() + sizeof(SaccadeInputPlanHeader));

    *header = {};
    uint32_t command_count = 0;
    uint32_t payload_command_index = UINT32_MAX;
    uint32_t permissions = 0;

    auto append = [&](const SaccadeInputCommand& command) noexcept -> bool {
        if (command_count == SACCADE_INPUT_PLAN_MAX_COMMANDS) {
            return false;
        }
        commands[command_count++] = command;
        return true;
    };

    switch (request.kind) {
    case ActionKind::pointer_move:
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        {
            SaccadeInputCommand command = target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[0]);
            command.duration_ns = request.pointer_duration_ns;
            (void)append(command);
        }
        break;
    case ActionKind::click:
    case ActionKind::invoke:
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        for (uint32_t index = 0; index < request.target_count; ++index) {
            if (request.pointer_duration_ns != 0) {
                SaccadeInputCommand move = target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[index]);
                move.duration_ns = request.pointer_duration_ns;
                if (!append(move)) return SACCADE_ERROR_CAPACITY;
            }
            SaccadeInputCommand command = target_command(SACCADE_INPUT_COMMAND_CLICK, *targets[index]);
            command.data0 = request.button;
            command.data1 = request.repeat_count;
            command.data2 = request.modifiers;
            if (!append(command)) {
                return SACCADE_ERROR_CAPACITY;
            }
        }
        break;
    case ActionKind::hold: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        (void)append(target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[0]));
        SaccadeInputCommand command = target_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, *targets[0]);
        command.flags |= SACCADE_INPUT_COMMAND_CONTINUOUS;
        command.data0 = request.button;
        command.duration_ns = request.duration_ns;
        (void)append(command);
        break;
    }
    case ActionKind::drag: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        (void)append(target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[0]));
        SaccadeInputCommand down = target_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, *targets[0]);
        down.data0 = request.button;
        (void)append(down);
        SaccadeInputCommand move = target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[1]);
        move.duration_ns = request.duration_ns;
        (void)append(move);
        SaccadeInputCommand up = target_command(SACCADE_INPUT_COMMAND_BUTTON_UP, *targets[1]);
        up.data0 = request.button;
        (void)append(up);
        break;
    }
    case ActionKind::scroll: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        SaccadeInputCommand command = target_command(SACCADE_INPUT_COMMAND_SCROLL, *targets[0]);
        command.delta_x_q8 = request.delta_x_q8;
        command.delta_y_q8 = request.delta_y_q8;
        command.duration_ns = request.duration_ns;
        if (request.duration_ns != 0) {
            command.flags |= SACCADE_INPUT_COMMAND_CONTINUOUS;
        }
        (void)append(command);
        break;
    }
    case ActionKind::key: {
        permissions = SACCADE_INPUT_PERMISSION_KEYBOARD;
        SaccadeInputCommand down{};
        down.kind = SACCADE_INPUT_COMMAND_KEY_DOWN;
        down.flags = SACCADE_INPUT_COMMAND_PHYSICAL_KEY;
        down.data0 = request.key_usage;
        down.data1 = request.modifiers;
        (void)append(down);
        down.kind = SACCADE_INPUT_COMMAND_KEY_UP;
        (void)append(down);
        break;
    }
    case ActionKind::text: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_TEXT;
        SaccadeInputCommand focus = target_command(SACCADE_INPUT_COMMAND_CLICK, *targets[0]);
        focus.data0 = request.button;
        focus.data1 = 1;
        (void)append(focus);
        SaccadeInputCommand text = target_command(SACCADE_INPUT_COMMAND_TEXT, *targets[0]);
        text.payload_size = static_cast<uint32_t>(request.text.size);
        payload_command_index = command_count;
        (void)append(text);
        break;
    }
    case ActionKind::window_activate:
        permissions = SACCADE_INPUT_PERMISSION_WINDOW;
        (void)append(target_command(SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE, *targets[0]));
        break;
    case ActionKind::release: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        SaccadeInputCommand command{};
        command.kind = SACCADE_INPUT_COMMAND_BUTTON_UP;
        command.data0 = request.button;
        (void)append(command);
        break;
    }
    case ActionKind::text_select: {
        permissions = SACCADE_INPUT_PERMISSION_POINTER;
        (void)append(target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[0]));
        SaccadeInputCommand down = target_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, *targets[0]);
        down.data0 = request.button;
        (void)append(down);
        SaccadeInputCommand move = target_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, *targets[1]);
        move.duration_ns = request.duration_ns;
        (void)append(move);
        SaccadeInputCommand up = target_command(SACCADE_INPUT_COMMAND_BUTTON_UP, *targets[1]);
        up.data0 = request.button;
        (void)append(up);
        break;
    }
    }
    if (request.move_to_final_pointer && request.kind != ActionKind::hold &&
        request.kind != ActionKind::window_activate) {
        SaccadeInputCommand final{};
        final.kind = SACCADE_INPUT_COMMAND_POINTER_MOVE;
        final.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
        final.x_q8 = request.final_pointer.x;
        final.y_q8 = request.final_pointer.y;
        final.duration_ns = request.pointer_duration_ns;
        if (!append(final)) return SACCADE_ERROR_CAPACITY;
    }
    if ((context.permissions & permissions) != permissions) {
        ++stats_.rejected_permission;
        return SACCADE_ERROR_UNSUPPORTED;
    }

    const uint64_t commands_size = static_cast<uint64_t>(command_count) * sizeof(SaccadeInputCommand);
    const uint64_t payload_offset = sizeof(SaccadeInputPlanHeader) + commands_size;
    uint64_t total_size = payload_offset;
    if (request.kind == ActionKind::text) {
        commands[payload_command_index].payload_offset = static_cast<uint32_t>(payload_offset);
        std::memcpy(storage->bytes.data() + payload_offset, request.text.data, request.text.size);
        total_size += request.text.size;
    }
    header->struct_size = sizeof(*header);
    header->plan_version = SACCADE_INPUT_PLAN_VERSION;
    header->command_count = command_count;
    header->command_stride = sizeof(SaccadeInputCommand);
    header->flags = context.plan_flags;
    header->required_permissions = permissions;
    header->expected_buttons = context.expected_buttons;
    header->plan_id = context.plan_id;
    header->scene_epoch = scene.header->scene_epoch;
    header->frame_id = scene.header->frame_id;
    header->model_epoch = scene.header->model_epoch;
    header->session_epoch = scene.header->session_epoch;
    header->transform_epoch = scene.header->transform_epoch;
    header->topology_epoch = scene.header->topology_epoch;
    header->permission_epoch = context.permission_epoch;
    header->source_id = scene.header->source_id;
    header->focus_id = context.focus_id;
    header->window_id =
        request.target_count == 0 ? 0 : (targets[0]->window_id == 0 ? context.window_id : targets[0]->window_id);
    header->display_id =
        request.target_count == 0 ? 0 : (targets[0]->display_id == 0 ? context.display_id : targets[0]->display_id);
    header->deadline_ns = context.deadline_ns;
    header->commands_offset = sizeof(*header);
    header->total_size = total_size;
    input::PlanView validated{};
    if (input::validate_plan({storage->bytes.data(), static_cast<size_t>(total_size)}, &validated) != SACCADE_OK) {
        return SACCADE_ERROR_STATE;
    }
    *output = {storage->bytes.data(), static_cast<size_t>(total_size)};
    ++stats_.plans_built;
    stats_.commands_built += command_count;
    stats_.targets_resolved += request.target_count;
    return SACCADE_OK;
}

} // namespace saccade::interaction

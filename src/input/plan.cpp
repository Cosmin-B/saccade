#include "input/plan.hpp"

#include <cstdint>
#include <limits>

namespace saccade::input {
namespace {

constexpr uint32_t permission_mask = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD |
                                     SACCADE_INPUT_PERMISSION_TEXT | SACCADE_INPUT_PERMISSION_WINDOW |
                                     SACCADE_INPUT_PERMISSION_CLIPBOARD;
constexpr uint32_t button_mask = SACCADE_INPUT_BUTTON_LEFT | SACCADE_INPUT_BUTTON_RIGHT | SACCADE_INPUT_BUTTON_MIDDLE;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;
constexpr uint32_t command_flag_mask =
    SACCADE_INPUT_COMMAND_ABSOLUTE | SACCADE_INPUT_COMMAND_PHYSICAL_KEY | SACCADE_INPUT_COMMAND_CONTINUOUS;
constexpr uint32_t plan_flag_mask =
    SACCADE_INPUT_PLAN_DRY_RUN | SACCADE_INPUT_PLAN_STOP_ON_FAILURE | SACCADE_INPUT_PLAN_RESTORE_POINTER;

bool aligned(const void* pointer, size_t alignment) noexcept {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1U)) == 0;
}

bool one_button(uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0 && (value & ~button_mask) == 0;
}

bool payload_valid(const SaccadeInputCommand& command, const SaccadeInputPlanHeader& header,
                   uint64_t command_bytes) noexcept {
    if (command.kind != SACCADE_INPUT_COMMAND_TEXT) {
        return command.payload_offset == 0 && command.payload_size == 0;
    }
    if (command.payload_size == 0 || command.payload_offset < command_bytes) {
        return false;
    }
    return static_cast<uint64_t>(command.payload_offset) + command.payload_size <= header.total_size;
}

bool command_valid(const SaccadeInputCommand& command, const SaccadeInputPlanHeader& header, uint64_t command_bytes,
                   uint32_t* permissions) noexcept {
    if (command.kind < SACCADE_INPUT_COMMAND_POINTER_MOVE || command.kind > SACCADE_INPUT_COMMAND_WAIT ||
        (command.flags & ~command_flag_mask) != 0 || command.reserved32 != 0 ||
        !payload_valid(command, header, command_bytes)) {
        return false;
    }
    switch (command.kind) {
    case SACCADE_INPUT_COMMAND_POINTER_MOVE:
        *permissions |= SACCADE_INPUT_PERMISSION_POINTER;
        return (command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 && command.data0 == 0 && command.data1 == 0 &&
               command.data2 == 0;
    case SACCADE_INPUT_COMMAND_BUTTON_DOWN:
    case SACCADE_INPUT_COMMAND_BUTTON_UP:
        *permissions |= SACCADE_INPUT_PERMISSION_POINTER;
        return one_button(command.data0) && command.data1 == 0 && command.data2 == 0;
    case SACCADE_INPUT_COMMAND_CLICK:
        *permissions |= SACCADE_INPUT_PERMISSION_POINTER;
        return one_button(command.data0) && command.data1 >= 1 && command.data1 <= 16 &&
               (command.data2 & ~modifier_mask) == 0;
    case SACCADE_INPUT_COMMAND_SCROLL:
        *permissions |= SACCADE_INPUT_PERMISSION_POINTER;
        return (command.delta_x_q8 != 0 || command.delta_y_q8 != 0) && command.data0 == 0 && command.data1 == 0 &&
               command.data2 == 0;
    case SACCADE_INPUT_COMMAND_KEY_DOWN:
    case SACCADE_INPUT_COMMAND_KEY_UP:
        *permissions |= SACCADE_INPUT_PERMISSION_KEYBOARD;
        return command.data0 != 0 && (command.data1 & ~modifier_mask) == 0 && command.data2 == 0;
    case SACCADE_INPUT_COMMAND_TEXT:
        *permissions |= SACCADE_INPUT_PERMISSION_TEXT;
        return command.data0 == 0 && command.data1 == 0 && command.data2 == 0 && command.duration_ns == 0;
    case SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE:
        *permissions |= SACCADE_INPUT_PERMISSION_WINDOW;
        return command.target_id != 0 && command.data0 == 0 && command.data1 == 0 && command.data2 == 0 &&
               command.duration_ns == 0;
    case SACCADE_INPUT_COMMAND_WAIT:
        return command.duration_ns != 0 && command.data0 == 0 && command.data1 == 0 && command.data2 == 0;
    default:
        return false;
    }
}

} // namespace

SaccadeResult validate_plan(SaccadeSpanU8 bytes, PlanView* output) noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    if (bytes.data == nullptr || bytes.size < sizeof(SaccadeInputPlanHeader) ||
        !aligned(bytes.data, alignof(SaccadeInputPlanHeader))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const auto* header = reinterpret_cast<const SaccadeInputPlanHeader*>(bytes.data);
    if (header->struct_size != sizeof(*header) || header->plan_version != SACCADE_INPUT_PLAN_VERSION ||
        header->command_count == 0 || header->command_count > SACCADE_INPUT_PLAN_MAX_COMMANDS ||
        header->command_stride != sizeof(SaccadeInputCommand) || (header->flags & ~plan_flag_mask) != 0 ||
        header->required_permissions == 0 || (header->required_permissions & ~permission_mask) != 0 ||
        (header->expected_buttons & ~button_mask) != 0 || header->reserved32 != 0 || header->plan_id == 0 ||
        header->scene_epoch == 0 || header->frame_id == 0 || header->model_epoch == 0 || header->session_epoch == 0 ||
        header->transform_epoch == 0 || header->topology_epoch == 0 || header->permission_epoch == 0 ||
        header->source_id == 0 || header->deadline_ns == 0 ||
        header->commands_offset != sizeof(SaccadeInputPlanHeader) || header->total_size > bytes.size ||
        header->reserved != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t commands_size = static_cast<uint64_t>(header->command_count) * header->command_stride;
    if (commands_size > std::numeric_limits<uint64_t>::max() - header->commands_offset) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t command_bytes = header->commands_offset + commands_size;
    if (command_bytes > header->total_size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const auto* commands = reinterpret_cast<const SaccadeInputCommand*>(bytes.data + header->commands_offset);
    uint32_t used_permissions = 0;
    for (uint32_t index = 0; index < header->command_count; ++index) {
        if (!command_valid(commands[index], *header, command_bytes, &used_permissions)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }
    if ((used_permissions & header->required_permissions) != used_permissions) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    output->header = header;
    output->commands = commands;
    output->byte_size = static_cast<size_t>(header->total_size);
    return SACCADE_OK;
}

} // namespace saccade::input

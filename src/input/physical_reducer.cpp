#include "input/physical_reducer.hpp"

#include <algorithm>
#include <cstdint>

namespace saccade::input {
namespace {

bool add_key(std::array<uint32_t, maximum_held_keys>* keys, uint32_t* count, uint32_t key) noexcept {
    if (std::find(keys->begin(), keys->begin() + *count, key) != keys->begin() + *count) {
        return false;
    }
    if (*count == maximum_held_keys) {
        return false;
    }
    (*keys)[(*count)++] = key;
    return true;
}

bool remove_key(std::array<uint32_t, maximum_held_keys>* keys, uint32_t* count, uint32_t key) noexcept {
    const auto end = keys->begin() + *count;
    const auto found = std::find(keys->begin(), end, key);
    if (found == end) {
        return false;
    }
    *found = *(end - 1);
    --*count;
    return true;
}

bool apply_transition(const SaccadeInputCommand& command, uint32_t* buttons,
                      std::array<uint32_t, maximum_held_keys>* keys, uint32_t* key_count) noexcept {
    switch (command.kind) {
    case SACCADE_INPUT_COMMAND_BUTTON_DOWN:
        if ((*buttons & command.data0) != 0) {
            return false;
        }
        *buttons |= command.data0;
        return true;
    case SACCADE_INPUT_COMMAND_BUTTON_UP:
        if ((*buttons & command.data0) == 0) {
            return false;
        }
        *buttons &= ~command.data0;
        return true;
    case SACCADE_INPUT_COMMAND_KEY_DOWN:
        return add_key(keys, key_count, command.data0);
    case SACCADE_INPUT_COMMAND_KEY_UP:
        return remove_key(keys, key_count, command.data0);
    default:
        return true;
    }
}

} // namespace

SaccadeResult PhysicalInputReducer::initialize(uint64_t permission_epoch, int32_t pointer_x_q8,
                                               int32_t pointer_y_q8) noexcept {
    if (initialized_ || permission_epoch == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    permission_epoch_ = permission_epoch;
    pointer_x_q8_ = pointer_x_q8;
    pointer_y_q8_ = pointer_y_q8;
    physical_sequence_ = 1;
    initialized_ = true;
    return SACCADE_OK;
}

bool PhysicalInputReducer::preflight(const PlanView& plan) const noexcept {
    uint32_t buttons = buttons_;
    uint32_t key_count = held_key_count_;
    auto keys = held_keys_;
    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        if (!apply_transition(plan.commands[index], &buttons, &keys, &key_count)) {
            return false;
        }
    }
    return true;
}

SaccadeResult PhysicalInputReducer::begin(const PlanView& plan, uint32_t available_permissions,
                                          uint64_t now_ns) noexcept {
    if (!initialized_ || active_header_ != nullptr || plan.header == nullptr || plan.commands == nullptr) {
        return SACCADE_ERROR_STATE;
    }
    if (plan.header->permission_epoch != permission_epoch_ || plan.header->expected_buttons != buttons_ ||
        now_ns >= plan.header->deadline_ns) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if ((available_permissions & plan.header->required_permissions) != plan.header->required_permissions) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    if (!preflight(plan)) {
        return SACCADE_ERROR_STATE;
    }
    active_header_ = plan.header;
    active_commands_ = plan.commands;
    active_deadline_ns_ = plan.header->deadline_ns;
    active_dry_run_ = (plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) != 0;
    completed_commands_ = 0;
    ++stats_.plans_started;
    return SACCADE_OK;
}

bool PhysicalInputReducer::apply(const SaccadeInputCommand& command) noexcept {
    if (!apply_transition(command, &buttons_, &held_keys_, &held_key_count_)) {
        return false;
    }
    if ((command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 &&
        (command.kind == SACCADE_INPUT_COMMAND_POINTER_MOVE || command.kind == SACCADE_INPUT_COMMAND_CLICK ||
         command.kind == SACCADE_INPUT_COMMAND_SCROLL || command.kind == SACCADE_INPUT_COMMAND_BUTTON_DOWN ||
         command.kind == SACCADE_INPUT_COMMAND_BUTTON_UP)) {
        pointer_x_q8_ = command.x_q8;
        pointer_y_q8_ = command.y_q8;
    }
    return true;
}

SaccadeResult PhysicalInputReducer::advance(uint32_t completed_commands) noexcept {
    if (!initialized_ || active_header_ == nullptr || completed_commands < completed_commands_ ||
        completed_commands > active_header_->command_count) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    while (completed_commands_ < completed_commands) {
        if (!active_dry_run_ && !apply(active_commands_[completed_commands_])) {
            return SACCADE_ERROR_STATE;
        }
        ++completed_commands_;
        ++stats_.commands_completed;
    }
    if (completed_commands_ == active_header_->command_count) {
        const uint64_t plan_id = active_header_->plan_id;
        active_header_ = nullptr;
        active_commands_ = nullptr;
        completed_commands_ = 0;
        active_lease_id_ = (buttons_ != 0 || held_key_count_ != 0) ? plan_id : 0;
        if (active_lease_id_ == 0) {
            active_deadline_ns_ = 0;
        }
        active_dry_run_ = false;
        ++stats_.plans_completed;
    }
    return SACCADE_OK;
}

void PhysicalInputReducer::collect_release(SyntheticRelease* output) noexcept {
    *output = {};
    output->buttons = buttons_;
    output->modifiers = modifiers_;
    output->held_key_count = held_key_count_;
    std::copy_n(held_keys_.begin(), held_key_count_, output->held_keys.begin());
    if (buttons_ != 0 || modifiers_ != 0 || held_key_count_ != 0) {
        ++stats_.releases;
    }
    buttons_ = 0;
    modifiers_ = 0;
    held_key_count_ = 0;
    active_lease_id_ = 0;
    active_deadline_ns_ = 0;
    active_header_ = nullptr;
    active_commands_ = nullptr;
    completed_commands_ = 0;
    active_dry_run_ = false;
}

SaccadeResult PhysicalInputReducer::abort(SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    collect_release(output);
    ++stats_.aborts;
    return SACCADE_OK;
}

SaccadeResult PhysicalInputReducer::backend_failure(SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    collect_release(output);
    ++stats_.backend_failures;
    return SACCADE_OK;
}

SaccadeResult PhysicalInputReducer::expire(uint64_t now_ns, SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (active_deadline_ns_ == 0 || now_ns < active_deadline_ns_) {
        *output = {};
        return SACCADE_ERROR_NOT_FOUND;
    }
    collect_release(output);
    ++stats_.timeouts;
    return SACCADE_OK;
}

SaccadeResult PhysicalInputReducer::physical_override(int32_t pointer_x_q8, int32_t pointer_y_q8,
                                                      SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    collect_release(output);
    pointer_x_q8_ = pointer_x_q8;
    pointer_y_q8_ = pointer_y_q8;
    ++physical_sequence_;
    ++stats_.physical_overrides;
    return SACCADE_OK;
}

SaccadeResult PhysicalInputReducer::permission_lost(uint64_t new_permission_epoch, SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr || new_permission_epoch == 0 || new_permission_epoch == permission_epoch_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    collect_release(output);
    permission_epoch_ = new_permission_epoch;
    ++stats_.permission_losses;
    return SACCADE_OK;
}

SaccadeResult PhysicalInputReducer::shutdown(SyntheticRelease* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    collect_release(output);
    initialized_ = false;
    permission_epoch_ = 0;
    return SACCADE_OK;
}

SaccadePhysicalInputState PhysicalInputReducer::state() const noexcept {
    SaccadePhysicalInputState output{};
    output.struct_size = sizeof(output);
    output.api_version = SACCADE_API_VERSION;
    output.pointer_x_q8 = pointer_x_q8_;
    output.pointer_y_q8 = pointer_y_q8_;
    output.buttons = buttons_;
    output.modifiers = modifiers_;
    output.active_lease_id = active_lease_id_;
    output.permission_epoch = permission_epoch_;
    output.physical_sequence = physical_sequence_;
    return output;
}

} // namespace saccade::input

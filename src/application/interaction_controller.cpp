#include "application/interaction_controller.hpp"

#include <cstring>

namespace saccade::application {
namespace {

bool mode_valid(interaction::SelectionMode mode) noexcept {
    return mode >= interaction::SelectionMode::single && mode <= interaction::SelectionMode::path;
}

bool profile_valid(const InteractionProfile& profile) noexcept {
    return profile.timeout_ns != 0 && profile.scroll_vertical_q8 != 0 && profile.scroll_vertical_q8 != INT32_MIN &&
           profile.scroll_horizontal_q8 != 0 && profile.scroll_horizontal_q8 != INT32_MIN &&
           mode_valid(profile.initial_mode) && profile.final_pointer >= PointerFinalPosition::target &&
           profile.final_pointer <= PointerFinalPosition::anchor;
}

uint64_t continuous_scroll_duration(const InteractionProfile& profile) noexcept {
    return profile.scroll_duration_ns == 0 ? default_continuous_scroll_lease_ns : profile.scroll_duration_ns;
}

int32_t positive_magnitude(int32_t value) noexcept {
    return value < 0 ? -value : value;
}

int32_t negative_magnitude(int32_t value) noexcept {
    return value < 0 ? value : -value;
}

bool action_for_command(Command command, const InteractionProfile& profile, interaction::SelectionMode current_mode,
                        SessionAction* action, interaction::SelectionMode* mode) noexcept {
    *action = {};
    *mode = interaction::SelectionMode::single;

    switch (command) {
    case Command::pointer_move:
    case Command::hover:
        action->kind = interaction::ActionKind::pointer_move;
        return true;
    case Command::free_pointer:
        action->kind = interaction::ActionKind::pointer_move;
        action->defer_execution = true;
        return true;
    case Command::left_click:
        action->kind = interaction::ActionKind::click;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        *mode = current_mode;
        return true;
    case Command::right_click:
        action->kind = interaction::ActionKind::click;
        action->button = SACCADE_INPUT_BUTTON_RIGHT;
        *mode = current_mode;
        return true;
    case Command::middle_click:
        action->kind = interaction::ActionKind::click;
        action->button = SACCADE_INPUT_BUTTON_MIDDLE;
        *mode = current_mode;
        return true;
    case Command::double_click:
        action->kind = interaction::ActionKind::click;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        action->repeat_count = 2;
        *mode = current_mode;
        return true;
    case Command::hold:
        action->kind = interaction::ActionKind::hold;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        action->duration_ns = profile.hold_duration_ns;
        return true;
    case Command::drag:
        action->kind = interaction::ActionKind::drag;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        action->duration_ns = profile.drag_duration_ns;
        *mode = interaction::SelectionMode::dual;
        return true;
    case Command::scroll_vertical:
        action->kind = interaction::ActionKind::scroll;
        action->delta_y_q8 = profile.scroll_vertical_q8;
        action->duration_ns = profile.scroll_duration_ns;
        return true;
    case Command::scroll_horizontal:
        action->kind = interaction::ActionKind::scroll;
        action->delta_x_q8 = profile.scroll_horizontal_q8;
        action->duration_ns = profile.scroll_duration_ns;
        return true;
    case Command::scroll_up:
    case Command::scroll_up_continuous:
        action->kind = interaction::ActionKind::scroll;
        action->delta_y_q8 = negative_magnitude(profile.scroll_vertical_q8);
        action->duration_ns = command == Command::scroll_up_continuous ? continuous_scroll_duration(profile) : 0;
        return true;
    case Command::scroll_down:
    case Command::scroll_down_continuous:
        action->kind = interaction::ActionKind::scroll;
        action->delta_y_q8 = positive_magnitude(profile.scroll_vertical_q8);
        action->duration_ns = command == Command::scroll_down_continuous ? continuous_scroll_duration(profile) : 0;
        return true;
    case Command::scroll_left:
    case Command::scroll_left_continuous:
        action->kind = interaction::ActionKind::scroll;
        action->delta_x_q8 = negative_magnitude(profile.scroll_horizontal_q8);
        action->duration_ns = command == Command::scroll_left_continuous ? continuous_scroll_duration(profile) : 0;
        return true;
    case Command::scroll_right:
    case Command::scroll_right_continuous:
        action->kind = interaction::ActionKind::scroll;
        action->delta_x_q8 = positive_magnitude(profile.scroll_horizontal_q8);
        action->duration_ns = command == Command::scroll_right_continuous ? continuous_scroll_duration(profile) : 0;
        return true;
    case Command::select_text:
        action->kind = interaction::ActionKind::text_select;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        action->duration_ns = profile.drag_duration_ns;
        *mode = interaction::SelectionMode::dual;
        return true;
    case Command::type_text:
        action->kind = interaction::ActionKind::text;
        action->button = SACCADE_INPUT_BUTTON_LEFT;
        return true;
    case Command::window_activate:
        action->kind = interaction::ActionKind::window_activate;
        return true;
    default:
        return false;
    }
}

bool mode_for_command(Command command, interaction::SelectionMode* mode) noexcept {
    switch (command) {
    case Command::mode_single:
        *mode = interaction::SelectionMode::single;
        return true;
    case Command::mode_dual:
        *mode = interaction::SelectionMode::dual;
        return true;
    case Command::mode_multi:
        *mode = interaction::SelectionMode::multi;
        return true;
    case Command::mode_path:
        *mode = interaction::SelectionMode::path;
        return true;
    default:
        return false;
    }
}

bool adjustment_command(Command command) noexcept {
    return (command >= Command::target_position_next && command <= Command::nudge_down) ||
           (command >= Command::target_position_1 && command <= Command::target_position_9);
}

} // namespace

SaccadeResult InteractionController::initialize(SessionEngine* session, InteractionProfile profile,
                                                InteractionStateSource state_source,
                                                InteractionControllerSink sink) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;

    if (session == nullptr || state_source.read == nullptr || !profile_valid(profile) ||
        (sink.input_lease_active == nullptr) != (sink.neutralize_input == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    session_ = session;
    profile_ = profile;
    state_source_ = state_source;
    sink_ = sink;
    selection_mode_ = profile.initial_mode;
    initialized_ = true;

    return SACCADE_OK;
}

SaccadeResult InteractionController::set_text(SaccadeSpanU8 text) noexcept {
    if (!initialized_ || text.size > text_.size() || (text.size != 0 && text.data == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    if (text.size != 0) std::memcpy(text_.data(), text.data, text.size);
    text_size_ = static_cast<uint32_t>(text.size);

    return SACCADE_OK;
}

SaccadeResult InteractionController::set_profile(InteractionProfile profile) noexcept {
    if (!initialized_ || !profile_valid(profile)) return SACCADE_ERROR_INVALID_ARGUMENT;

    if (session_->active()) return SACCADE_ERROR_BUSY;

    profile_ = profile;
    selection_mode_ = profile.initial_mode;

    return SACCADE_OK;
}

uint64_t InteractionController::next_plan_id() noexcept {
    const uint64_t result = next_plan_id_;
    ++next_plan_id_;

    if (next_plan_id_ == 0) ++next_plan_id_;

    return result;
}

SaccadeResult InteractionController::begin_action(const SessionAction& source, interaction::SelectionMode mode,
                                                  uint64_t timestamp_ns, InteractionCommandResult* output) noexcept {
    if (session_->active() || timestamp_ns == 0 || !mode_valid(mode)) {
        return SACCADE_ERROR_STATE;
    }

    InteractionState state{};
    const SaccadeResult read = state_source_.read(state_source_.context, &state);

    if (read != SACCADE_OK) return read;

    if (state.permission_epoch == 0 || state.permissions == 0) {
        return SACCADE_ERROR_PERMISSION;
    }

    SessionConfig config{};
    config.mode = mode;
    config.hints = profile_.hints;
    config.action.plan_id = next_plan_id();
    config.action.permission_epoch = state.permission_epoch;
    config.action.focus_id = state.focus_id;
    config.action.now_ns = timestamp_ns;
    config.action.deadline_ns = timestamp_ns + profile_.timeout_ns;

    if (config.action.deadline_ns <= timestamp_ns) return SACCADE_ERROR_CAPACITY;

    config.action.permissions = state.permissions;
    config.action.expected_buttons = state.expected_buttons;
    config.request = source;
    config.request.modifiers = source.modifiers | profile_.click_modifiers;
    config.request.pointer_duration_ns = static_cast<uint64_t>(profile_.pointer_duration_ms) * UINT64_C(1'000'000);

    if (profile_.final_pointer != PointerFinalPosition::target) {
        config.request.move_to_final_pointer = true;
        config.request.final_pointer = profile_.final_pointer == PointerFinalPosition::original
                                           ? geometry::PointQ8{state.pointer_x_q8, state.pointer_y_q8}
                                           : profile_.pointer_anchor;
    }

    if (config.request.kind == interaction::ActionKind::text) {
        if (text_size_ == 0) return SACCADE_ERROR_STATE;
        config.request.text = {text_.data(), text_size_};
    }

    const SaccadeResult begun = session_->begin_latest(config);

    if (begun != SACCADE_OK) return begun;

    last_action_ = config.request;
    last_mode_ = mode;
    has_last_action_ = true;
    output->action_started = true;

    ++stats_.actions_started;

    return SACCADE_OK;
}

SaccadeResult InteractionController::change_mode(interaction::SelectionMode mode,
                                                 InteractionCommandResult* output) noexcept {
    if (!mode_valid(mode)) return SACCADE_ERROR_INVALID_ARGUMENT;

    if (session_->active()) {
        const SaccadeResult cancelled = session_->cancel(interaction::SelectionCancelReason::user);
        if (cancelled != SACCADE_OK) return cancelled;
        ++stats_.sessions_cancelled;
    }

    selection_mode_ = mode;
    if (has_last_action_) last_mode_ = mode;
    output->mode_changed = true;

    ++stats_.modes_changed;

    return SACCADE_OK;
}

SaccadeResult InteractionController::forward(Command command, uint64_t timestamp_ns,
                                             InteractionCommandResult* output) noexcept {
    if (sink_.forward == nullptr) return SACCADE_ERROR_NOT_FOUND;

    const SaccadeResult result = sink_.forward(sink_.context, command, timestamp_ns);

    if (result != SACCADE_OK) return result;

    output->forwarded = true;
    ++stats_.commands_forwarded;

    return SACCADE_OK;
}

SaccadeResult InteractionController::dispatch(Command command, uint64_t timestamp_ns,
                                              InteractionCommandResult* output) noexcept {
    if (!initialized_ || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;

    *output = {};
    ++stats_.commands;

    SaccadeResult result = SACCADE_OK;
    interaction::SelectionMode mode = interaction::SelectionMode::single;

    if (adjustment_command(command)) {
        switch (command) {
        case Command::target_position_next:
            result = session_->cycle_target_position();
            break;
        case Command::edge_snap_left:
            result = session_->snap_target(TargetSnapDirection::left);
            break;
        case Command::edge_snap_right:
            result = session_->snap_target(TargetSnapDirection::right);
            break;
        case Command::edge_snap_up:
            result = session_->snap_target(TargetSnapDirection::up);
            break;
        case Command::edge_snap_down:
            result = session_->snap_target(TargetSnapDirection::down);
            break;
        case Command::nudge_left:
            result = session_->nudge_target(-256, 0);
            break;
        case Command::nudge_right:
            result = session_->nudge_target(256, 0);
            break;
        case Command::nudge_up:
            result = session_->nudge_target(0, -256);
            break;
        case Command::nudge_down:
            result = session_->nudge_target(0, 256);
            break;
        case Command::target_position_1:
        case Command::target_position_2:
        case Command::target_position_3:
        case Command::target_position_4:
        case Command::target_position_5:
        case Command::target_position_6:
        case Command::target_position_7:
        case Command::target_position_8:
        case Command::target_position_9:
            result = session_->set_target_position(static_cast<uint32_t>(command) -
                                                   static_cast<uint32_t>(Command::target_position_1));
            break;
        default:
            result = SACCADE_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (result == SACCADE_OK) {
            output->target_adjusted = true;
            ++stats_.target_adjustments;
        }
    } else if (mode_for_command(command, &mode)) {
        result = change_mode(mode, output);
    } else if (command == Command::repeat_action) {
        result =
            has_last_action_ ? begin_action(last_action_, last_mode_, timestamp_ns, output) : SACCADE_ERROR_NOT_FOUND;
    } else {
        SessionAction action{};
        if (action_for_command(command, profile_, selection_mode_, &action, &mode)) {
            result = begin_action(action, mode, timestamp_ns, output);
        } else {
            result = forward(command, timestamp_ns, output);
        }
    }

    if (result != SACCADE_OK) ++stats_.failures;

    return result;
}

bool InteractionController::input_lease_active() const noexcept {
    return sink_.input_lease_active != nullptr && sink_.input_lease_active(sink_.context);
}

SaccadeResult InteractionController::observe_physical_input(uint64_t timestamp_ns) noexcept {
    if (!initialized_ || timestamp_ns == 0) return SACCADE_ERROR_INVALID_ARGUMENT;

    ++stats_.physical_inputs;

    SaccadeResult result = SACCADE_OK;

    if (session_->active()) {
        result = session_->cancel(interaction::SelectionCancelReason::user);
        if (result != SACCADE_OK) {
            ++stats_.failures;
            return result;
        }
        ++stats_.sessions_cancelled;
    }

    if (input_lease_active()) {
        result = sink_.neutralize_input(sink_.context);
        if (result == SACCADE_OK) ++stats_.input_neutralizations;
    }

    if (result != SACCADE_OK) ++stats_.failures;

    return result;
}

SaccadeResult InteractionController::tick(uint64_t timestamp_ns) noexcept {
    if (!initialized_ || timestamp_ns == 0) return SACCADE_ERROR_INVALID_ARGUMENT;

    if (!session_->active()) return SACCADE_OK;

    InteractionState state{};
    const SaccadeResult read = state_source_.read(state_source_.context, &state);

    if (read != SACCADE_OK) {
        (void)session_->cancel(interaction::SelectionCancelReason::permission_lost);
        ++stats_.sessions_cancelled;
        ++stats_.failures;
        return read;
    }

    if (state.permission_epoch == 0) {
        (void)session_->cancel(interaction::SelectionCancelReason::permission_lost);
        ++stats_.sessions_cancelled;
        ++stats_.failures;
        return SACCADE_ERROR_PERMISSION;
    }

    const SessionEpochs epochs{state.scene_epoch, state.transform_epoch, state.topology_epoch, state.permission_epoch,
                               state.focus_id};
    const SaccadeResult result = session_->tick(epochs, timestamp_ns);

    if (result != SACCADE_OK && result != SACCADE_ERROR_STALE_HANDLE) {
        ++stats_.failures;
    }

    return result;
}

SaccadeResult InteractionController::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;

    SaccadeResult result = SACCADE_OK;

    if (session_->active()) {
        result = session_->cancel(interaction::SelectionCancelReason::user);
        if (result == SACCADE_OK) ++stats_.sessions_cancelled;
    }

    if (result == SACCADE_OK && input_lease_active()) {
        result = sink_.neutralize_input(sink_.context);
        if (result == SACCADE_OK) ++stats_.input_neutralizations;
    }

    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }

    session_ = nullptr;
    profile_ = {};
    state_source_ = {};
    sink_ = {};
    last_action_ = {};
    text_size_ = 0;
    selection_mode_ = interaction::SelectionMode::single;
    last_mode_ = interaction::SelectionMode::single;
    initialized_ = false;
    has_last_action_ = false;

    return SACCADE_OK;
}

SaccadeResult start_interaction_command(void* context, Command command, uint64_t timestamp_ns) noexcept {
    auto* controller = static_cast<InteractionController*>(context);

    if (controller == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;

    InteractionCommandResult result{};
    return controller->dispatch(command, timestamp_ns, &result);
}

void observe_interaction_input(void* context, uint64_t timestamp_ns) noexcept {
    auto* controller = static_cast<InteractionController*>(context);
    if (controller != nullptr) (void)controller->observe_physical_input(timestamp_ns);
}

} // namespace saccade::application

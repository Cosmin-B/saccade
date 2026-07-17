#include "application/session.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace saccade::application {
namespace {

bool action_compatible(interaction::SelectionMode mode, interaction::ActionKind action) noexcept {
    if (action == interaction::ActionKind::key || action == interaction::ActionKind::release) {
        return false;
    }
    if (mode == interaction::SelectionMode::single) return true;
    if (mode == interaction::SelectionMode::dual) {
        return action == interaction::ActionKind::click || action == interaction::ActionKind::drag ||
               action == interaction::ActionKind::text_select;
    }
    return action == interaction::ActionKind::click;
}

interaction::SelectionContext selection_context(const interaction::ActionContext& context) noexcept {
    return {context.scene_epoch, context.transform_epoch, context.topology_epoch, context.focus_id,
            context.deadline_ns};
}

constexpr std::array<geometry::PointQ8, 9> target_positions{
    {{1, 1}, {0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}}};

uint32_t position_index(int32_t column, int32_t row) noexcept {
    for (uint32_t index = 0; index < target_positions.size(); ++index)
        if (target_positions[index].x == column && target_positions[index].y == row) return index;
    return 0;
}

} // namespace

SaccadeResult SessionEngine::initialize(scene::SceneStore* scenes, SessionStorage* storage,
                                        PlanExecutor executor) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (scenes == nullptr || storage == nullptr || executor.execute == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    scenes_ = scenes;
    storage_ = storage;
    executor_ = executor;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::begin(const SessionConfig& config) noexcept {
    if (!initialized_ || active_) return SACCADE_ERROR_STATE;
    scene::PacketView latest{};
    const SaccadeResult acquired = scenes_->acquire_latest(&latest);
    if (acquired != SACCADE_OK) return acquired;
    return begin_scene(config, latest);
}

SaccadeResult SessionEngine::begin_latest(SessionConfig config) noexcept {
    if (!initialized_ || active_) return SACCADE_ERROR_STATE;
    scene::PacketView latest{};
    const SaccadeResult acquired = scenes_->acquire_latest(&latest);
    if (acquired != SACCADE_OK) return acquired;
    config.action.scene_epoch = latest.header->scene_epoch;
    config.action.transform_epoch = latest.header->transform_epoch;
    config.action.topology_epoch = latest.header->topology_epoch;
    return begin_scene(config, latest);
}

SaccadeResult SessionEngine::begin_scene(const SessionConfig& config, const scene::PacketView& latest) noexcept {
    if (!action_compatible(config.mode, config.request.kind) || config.action.plan_id == 0 ||
        config.action.scene_epoch == 0 || config.action.transform_epoch == 0 || config.action.topology_epoch == 0 ||
        config.action.permission_epoch == 0 || config.action.deadline_ns == 0 ||
        config.action.now_ns >= config.action.deadline_ns || config.request.text.size > storage_->text.size() ||
        (config.request.text.size != 0 && config.request.text.data == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    if (latest.header->scene_epoch != config.action.scene_epoch ||
        latest.header->transform_epoch != config.action.transform_epoch ||
        latest.header->topology_epoch != config.action.topology_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }

    scene::PacketView owned_scene{};
    SaccadeResult result = copy_scene(latest, &owned_scene);
    if (result != SACCADE_OK) return result;

    result = hints_.freeze(owned_scene, config.hints, &storage_->hints);
    if (result != SACCADE_OK) return result;
    result = selection_.begin(owned_scene, config.mode, selection_context(config.action), &storage_->selection);
    if (result != SACCADE_OK) {
        (void)hints_.cancel();
        return result;
    }

    scene_ = owned_scene;
    action_context_ = config.action;
    action_ = config.request;
    deferred_execution_ = config.request.defer_execution;

    const uint32_t text_size = static_cast<uint32_t>(config.request.text.size);
    if (text_size != 0) {
        std::memcpy(storage_->text.data(), config.request.text.data, text_size);
        action_.text = {storage_->text.data(), text_size};
    }

    prefix_count_ = 0;
    target_position_ = UINT32_MAX;
    nudge_x_q8_ = 0;
    nudge_y_q8_ = 0;
    active_ = true;
    ++stats_.sessions_started;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::copy_scene(const scene::PacketView& source, scene::PacketView* output) noexcept {
    if (source.header == nullptr || source.targets == nullptr || output == nullptr || source.byte_size == 0 ||
        source.byte_size > storage_->scene.size()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::memcpy(storage_->scene.data(), source.header, source.byte_size);
    return scene::validate_packet({storage_->scene.data(), source.byte_size}, output);
}

SaccadeResult SessionEngine::refresh_latest(const SessionEpochs& current) noexcept {
    scene::PacketView latest{};
    SaccadeResult result = scenes_->acquire_latest(&latest);
    if (result != SACCADE_OK || latest.header->scene_epoch != current.scene_epoch ||
        latest.header->transform_epoch != current.transform_epoch ||
        latest.header->topology_epoch != current.topology_epoch) {
        ++stats_.refresh_failures;
        return SACCADE_ERROR_STALE_HANDLE;
    }

    scene::PacketView owned_scene{};
    result = copy_scene(latest, &owned_scene);
    if (result == SACCADE_OK) result = hints_.refresh_scene(owned_scene);
    if (result == SACCADE_OK) result = selection_.refresh_scene(owned_scene);
    if (result != SACCADE_OK) {
        ++stats_.refresh_failures;
        return SACCADE_ERROR_STALE_HANDLE;
    }

    scene_ = owned_scene;
    action_context_.scene_epoch = owned_scene.header->scene_epoch;
    ++stats_.scene_refreshes;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::cycle_target_position() noexcept {
    if (!active_) return SACCADE_ERROR_STATE;
    target_position_ =
        target_position_ == UINT32_MAX ? 0U : (target_position_ + 1U) % static_cast<uint32_t>(target_positions.size());
    nudge_x_q8_ = 0;
    nudge_y_q8_ = 0;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::set_target_position(uint32_t grid_index) noexcept {
    if (!active_ || grid_index >= target_positions.size()) return SACCADE_ERROR_INVALID_ARGUMENT;

    const int32_t column = static_cast<int32_t>(grid_index % 3U);
    const int32_t row = static_cast<int32_t>(grid_index / 3U);
    target_position_ = position_index(column, row);
    nudge_x_q8_ = 0;
    nudge_y_q8_ = 0;

    return SACCADE_OK;
}

SaccadeResult SessionEngine::snap_target(TargetSnapDirection direction) noexcept {
    if (!active_ || direction < TargetSnapDirection::left || direction > TargetSnapDirection::down)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    geometry::PointQ8 position =
        target_position_ == UINT32_MAX ? geometry::PointQ8{1, 1} : target_positions[target_position_];
    switch (direction) {
    case TargetSnapDirection::left:
        position.x = 0;
        break;
    case TargetSnapDirection::right:
        position.x = 2;
        break;
    case TargetSnapDirection::up:
        position.y = 0;
        break;
    case TargetSnapDirection::down:
        position.y = 2;
        break;
    }
    target_position_ = position_index(position.x, position.y);
    nudge_x_q8_ = 0;
    nudge_y_q8_ = 0;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::nudge_target(int32_t delta_x_q8, int32_t delta_y_q8) noexcept {
    if (!active_ || (delta_x_q8 == 0 && delta_y_q8 == 0)) return SACCADE_ERROR_INVALID_ARGUMENT;
    const int64_t x = static_cast<int64_t>(nudge_x_q8_) + delta_x_q8;
    const int64_t y = static_cast<int64_t>(nudge_y_q8_) + delta_y_q8;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return SACCADE_ERROR_CAPACITY;
    nudge_x_q8_ = static_cast<int32_t>(x);
    nudge_y_q8_ = static_cast<int32_t>(y);
    return SACCADE_OK;
}

geometry::PointQ8 SessionEngine::adjusted_point(const SaccadeTargetRecord& target) const noexcept {
    int64_t x = target.safe_x_q8;
    int64_t y = target.safe_y_q8;
    if (target_position_ != UINT32_MAX) {
        const geometry::PointQ8 position = target_positions[target_position_];
        const int64_t maximum_x = static_cast<int64_t>(target.width_q8) - 1;
        const int64_t maximum_y = static_cast<int64_t>(target.height_q8) - 1;
        const int64_t inset_x = std::min<int64_t>(2 * 256, maximum_x / 4);
        const int64_t inset_y = std::min<int64_t>(2 * 256, maximum_y / 4);
        const std::array<int64_t, 3> xs{target.x_q8 + inset_x, target.x_q8 + maximum_x / 2,
                                        target.x_q8 + maximum_x - inset_x};
        const std::array<int64_t, 3> ys{target.y_q8 + inset_y, target.y_q8 + maximum_y / 2,
                                        target.y_q8 + maximum_y - inset_y};
        x = xs[static_cast<size_t>(position.x)];
        y = ys[static_cast<size_t>(position.y)];
    }
    x += nudge_x_q8_;
    y += nudge_y_q8_;
    return {static_cast<int32_t>(std::clamp<int64_t>(x, INT32_MIN, INT32_MAX)),
            static_cast<int32_t>(std::clamp<int64_t>(y, INT32_MIN, INT32_MAX))};
}

SaccadeResult SessionEngine::resolve_prefix(SessionEvent* output) noexcept {
    interaction::HintMatch match{};
    const SaccadeResult resolved = hints_.resolve_prefix(storage_->prefix.data(), prefix_count_, &match);
    if (resolved != SACCADE_OK) return resolved;
    output->target_id = match.target_id;
    output->candidate_count = match.candidate_count;
    output->exact = match.exact;
    output->selected_count = selection_.view().target_count;
    return SACCADE_OK;
}

SaccadeResult SessionEngine::enter_symbol(uint16_t symbol, uint64_t now_ns, SessionEvent* output) noexcept {
    if (!active_ || output == nullptr || symbol == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};

    if (now_ns >= action_context_.deadline_ns) {
        (void)cancel(interaction::SelectionCancelReason::timeout);
        return SACCADE_ERROR_STALE_HANDLE;
    }

    if (prefix_count_ == storage_->prefix.size()) return SACCADE_ERROR_CAPACITY;
    storage_->prefix[prefix_count_++] = symbol;

    ++stats_.symbols_entered;
    SaccadeResult result = resolve_prefix(output);
    if (result != SACCADE_OK) {
        --prefix_count_;
        ++stats_.prefixes_rejected;
        return result;
    }

    if (!output->exact) return SACCADE_OK;

    result = selection_.select(output->target_id);
    if (result != SACCADE_OK) return result;
    prefix_count_ = 0;
    output->selected_count = selection_.view().target_count;
    if (selection_.view().state == interaction::SelectionState::complete && !deferred_execution_)
        return execute_selection(now_ns, output);
    return SACCADE_OK;
}

SaccadeResult SessionEngine::backspace(SessionEvent* output) noexcept {
    if (!active_ || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};

    if (prefix_count_ != 0) {
        --prefix_count_;
        if (prefix_count_ != 0) return resolve_prefix(output);
        output->selected_count = selection_.view().target_count;
        return SACCADE_OK;
    }
    if (deferred_execution_ && selection_.view().state == interaction::SelectionState::complete) {
        SaccadeResult result = selection_.reset();
        if (result == SACCADE_OK)
            result = selection_.begin(scene_, interaction::SelectionMode::single, selection_context(action_context_),
                                      &storage_->selection);
        output->selected_count = 0;
        return result;
    }
    const SaccadeResult result = selection_.backspace();
    output->selected_count = selection_.view().target_count;
    return result;
}

SaccadeResult SessionEngine::confirm(uint64_t now_ns, SessionEvent* output) noexcept {
    if (!active_ || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};

    if (now_ns >= action_context_.deadline_ns) {
        (void)cancel(interaction::SelectionCancelReason::timeout);
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!(deferred_execution_ && selection_.view().state == interaction::SelectionState::complete)) {
        const SaccadeResult confirmed = selection_.confirm();
        if (confirmed != SACCADE_OK) return confirmed;
    }
    output->selected_count = selection_.view().target_count;
    return execute_selection(now_ns, output);
}

SaccadeResult SessionEngine::execute_selection(uint64_t now_ns, SessionEvent* output) noexcept {
    const interaction::SelectionView selected = selection_.view();
    interaction::ActionRequest request{};
    request.kind = action_.kind;
    request.target_ids = selected.target_ids;
    request.target_count = selected.target_count;
    for (uint32_t index = 0; index < selected.target_count; ++index) {
        const SaccadeTargetRecord* target = nullptr;
        for (uint32_t candidate = 0; candidate < scene_.header->target_count; ++candidate) {
            if (scene_.targets[candidate].target_id == selected.target_ids[index]) {
                target = &scene_.targets[candidate];
                break;
            }
        }
        if (target == nullptr) return SACCADE_ERROR_STALE_HANDLE;
        storage_->action_points[index] = adjusted_point(*target);
    }
    request.target_points = storage_->action_points.data();
    request.target_point_count = selected.target_count;
    request.allow_point_outside_target = true;
    request.button = action_.button;
    request.repeat_count = action_.repeat_count;
    request.key_usage = action_.key_usage;
    request.modifiers = action_.modifiers;
    request.delta_x_q8 = action_.delta_x_q8;
    request.delta_y_q8 = action_.delta_y_q8;
    request.duration_ns = action_.duration_ns;
    request.pointer_duration_ns = action_.pointer_duration_ns;
    request.final_pointer = action_.final_pointer;
    request.move_to_final_pointer = action_.move_to_final_pointer;
    request.text = action_.text;
    action_context_.now_ns = now_ns;
    constexpr uint64_t execution_grace_ns = UINT64_C(1'000'000'000);
    const uint64_t pointer_steps =
        static_cast<uint64_t>(selected.target_count) + (request.move_to_final_pointer ? 1U : 0U);
    if (request.pointer_duration_ns != 0 && pointer_steps > UINT64_MAX / request.pointer_duration_ns)
        return SACCADE_ERROR_CAPACITY;
    const uint64_t pointer_duration = request.pointer_duration_ns * pointer_steps;
    if (request.duration_ns > UINT64_MAX - pointer_duration) return SACCADE_ERROR_CAPACITY;
    const uint64_t execution_duration = request.duration_ns + pointer_duration;
    if (execution_duration > UINT64_MAX - execution_grace_ns - now_ns) return SACCADE_ERROR_CAPACITY;
    action_context_.deadline_ns = now_ns + execution_grace_ns + execution_duration;
    SaccadeSpanU8 plan{};
    SaccadeResult result = planner_.build(scene_, action_context_, request, &storage_->plan, &plan);
    if (result == SACCADE_OK) {
        result = executor_.execute(executor_.context, plan, action_context_.permissions, now_ns);
    }
    if (result == SACCADE_OK) {
        output->action_executed = true;
        ++stats_.plans_executed;
        ++stats_.sessions_completed;
    } else {
        ++stats_.execution_failures;
    }
    reset_session();
    return result;
}

SaccadeResult SessionEngine::tick(const SessionEpochs& current, uint64_t now_ns) noexcept {
    if (!active_) return SACCADE_ERROR_STATE;
    if (current.permission_epoch != action_context_.permission_epoch) {
        ++stats_.stale_cancellations;
        (void)cancel(interaction::SelectionCancelReason::permission_lost);
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const bool refreshable_scene = current.scene_epoch != action_context_.scene_epoch &&
                                   current.transform_epoch == action_context_.transform_epoch &&
                                   current.topology_epoch == action_context_.topology_epoch &&
                                   current.focus_id == action_context_.focus_id && now_ns < action_context_.deadline_ns;
    if (refreshable_scene && refresh_latest(current) != SACCADE_OK) {
        if (selection_.view().state == interaction::SelectionState::collecting)
            (void)selection_.cancel(interaction::SelectionCancelReason::scene_changed);
        ++stats_.stale_cancellations;
        ++stats_.sessions_cancelled;
        reset_session();
        return SACCADE_ERROR_STALE_HANDLE;
    }

    const interaction::SelectionContext context{current.scene_epoch, current.transform_epoch, current.topology_epoch,
                                                current.focus_id, action_context_.deadline_ns};
    SaccadeResult result = SACCADE_OK;
    if (deferred_execution_ && selection_.view().state == interaction::SelectionState::complete) {
        result = now_ns >= action_context_.deadline_ns || current.scene_epoch != action_context_.scene_epoch ||
                         current.transform_epoch != action_context_.transform_epoch ||
                         current.topology_epoch != action_context_.topology_epoch ||
                         current.focus_id != action_context_.focus_id
                     ? SACCADE_ERROR_STALE_HANDLE
                     : SACCADE_OK;
    } else {
        result = selection_.validate(context, now_ns);
    }
    if (result == SACCADE_OK) return result;
    ++stats_.stale_cancellations;
    ++stats_.sessions_cancelled;
    reset_session();
    return result;
}

SaccadeResult SessionEngine::cancel(interaction::SelectionCancelReason reason) noexcept {
    if (!active_) return SACCADE_ERROR_STATE;
    if (selection_.view().state == interaction::SelectionState::collecting) {
        const SaccadeResult result = selection_.cancel(reason);
        if (result != SACCADE_OK) return result;
    }
    ++stats_.sessions_cancelled;
    reset_session();
    return SACCADE_OK;
}

void SessionEngine::reset_session() noexcept {
    if (hints_.labels() != nullptr) (void)hints_.cancel();
    if (selection_.view().state != interaction::SelectionState::idle) (void)selection_.reset();
    scene_ = {};
    action_context_ = {};
    action_ = {};
    deferred_execution_ = false;
    prefix_count_ = 0;
    target_position_ = UINT32_MAX;
    nudge_x_q8_ = 0;
    nudge_y_q8_ = 0;
    active_ = false;
}

SaccadeResult SessionEngine::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (active_) {
        const SaccadeResult cancelled = cancel(interaction::SelectionCancelReason::user);
        if (cancelled != SACCADE_OK) return cancelled;
    }
    scenes_ = nullptr;
    storage_ = nullptr;
    executor_ = {};
    initialized_ = false;
    return SACCADE_OK;
}

} // namespace saccade::application

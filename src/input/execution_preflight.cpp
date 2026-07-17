#include "input/execution_preflight.hpp"

namespace saccade::input {
namespace {

constexpr uint32_t unsafe_target_flags = SACCADE_TARGET_DISABLED | SACCADE_TARGET_OCCLUDED | SACCADE_TARGET_SECURE;

const SaccadeTargetRecord* find_target(const scene::PacketView& scene, uint64_t target_id) noexcept {
    for (uint32_t index = 0; index < scene.header->target_count; ++index) {
        if (scene.targets[index].target_id == target_id) return &scene.targets[index];
    }
    return nullptr;
}

bool scene_matches(const SaccadeInputPlanHeader& plan, const SaccadeTargetPacketHeader& scene) noexcept {
    return plan.scene_epoch == scene.scene_epoch && plan.frame_id == scene.frame_id &&
           plan.model_epoch == scene.model_epoch && plan.session_epoch == scene.session_epoch &&
           plan.transform_epoch == scene.transform_epoch && plan.topology_epoch == scene.topology_epoch &&
           plan.source_id == scene.source_id;
}

} // namespace

SaccadeResult validate_execution_preflight(const PlanView& plan, const ExecutionPreflightState& state,
                                           uint64_t now_ns) noexcept {
    if (plan.header == nullptr || plan.commands == nullptr || state.scene.header == nullptr ||
        state.scene.targets == nullptr || now_ns == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (now_ns >= plan.header->deadline_ns) return SACCADE_ERROR_TIMEOUT;
    if (!state.input_available || state.surface_secure || state.target_point_secure) return SACCADE_ERROR_PERMISSION;
    if (!scene_matches(*plan.header, *state.scene.header) || plan.header->focus_id != state.focus_id ||
        (state.validate_active_window && plan.header->window_id != state.window_id) ||
        plan.header->topology_epoch != state.topology_epoch ||
        plan.header->permission_epoch != state.permission_epoch ||
        (state.validate_initial_buttons && plan.header->expected_buttons != state.buttons)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (plan.header->window_id != 0 && !state.target_window_available) return SACCADE_ERROR_NOT_FOUND;

    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        const uint64_t target_id = plan.commands[index].target_id;
        if (target_id == 0) continue;
        const SaccadeTargetRecord* target = find_target(state.scene, target_id);
        if (target == nullptr || (target->flags & SACCADE_TARGET_ACTIONABLE) == 0 ||
            (target->flags & unsafe_target_flags) != 0) {
            return SACCADE_ERROR_NOT_FOUND;
        }
    }
    return SACCADE_OK;
}

} // namespace saccade::input

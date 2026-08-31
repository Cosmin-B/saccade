#include "agent/background_action_policy.hpp"

#include <saccade/saccade_agent.h>
#include <saccade/saccade_scene.h>

#include <cstdint>

namespace {

using saccade::agent::BackgroundActionBackend;
using saccade::agent::BackgroundActionPolicyInput;

enum class TestResult : int {
    success,
    semantic_background_failed,
    activation_required_failed,
    activation_allowed_failed,
    foreground_input_failed,
    foreground_semantic_failed,
    modifiers_failed,
    drag_failed,
    stale_identity_failed,
    mismatched_window_failed,
    missing_window_failed,
    dry_run_activation_failed,
    dry_run_background_failed,
    disabled_target_failed,
    secure_target_failed,
    implicit_activation_failed
};

constexpr uint64_t scene_process = 51;
constexpr uint64_t foreground_process = 62;
constexpr uint64_t scene_window = 73;

BackgroundActionPolicyInput explicit_visual_input() noexcept {
    BackgroundActionPolicyInput value{};
    value.kind = SACCADE_AGENT_ACTION_CLICK;
    value.flags = SACCADE_AGENT_ACTION_EXPLICIT_WINDOW;
    value.target_flags = SACCADE_AGENT_TARGET_ACTIONABLE;
    value.button_bits = SACCADE_AGENT_BUTTON_LEFT;
    value.repeat_count = 1;
    value.target_window_id = scene_window;
    value.scene_window_id = scene_window;
    value.scene_process_id = scene_process;
    value.foreground_process_id = foreground_process;
    value.explicit_window = true;
    value.identity_current = true;
    return value;
}

BackgroundActionPolicyInput explicit_semantic_input() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.kind = SACCADE_AGENT_ACTION_INVOKE;
    value.target_capabilities = SACCADE_AGENT_TARGET_INVOKE;
    value.target_sources = SACCADE_AGENT_TARGET_SOURCE_ACCESSIBILITY;
    return value;
}

bool semantic_target_uses_background_ax() noexcept {
    const auto decision = saccade::agent::choose_background_action(explicit_semantic_input());
    return decision.backend == BackgroundActionBackend::background_ax && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_background_ax;
}

bool explicit_visual_target_requires_activation() noexcept {
    const auto decision = saccade::agent::choose_background_action(explicit_visual_input());
    return decision.backend == BackgroundActionBackend::activation_required && decision.result == SACCADE_AGENT_ERROR_ACTIVATION_REQUIRED &&
           decision.result_flags == 0;
}

bool explicit_visual_target_uses_allowed_activation() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::activation_input && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_would_activate;
}

bool foreground_visual_target_uses_foreground_input() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.explicit_window = false;
    value.flags = 0;
    value.scene_process_id = foreground_process;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::foreground_input && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_cg_event;
}

bool foreground_semantic_target_does_not_take_background_path() noexcept {
    BackgroundActionPolicyInput value = explicit_semantic_input();
    value.explicit_window = false;
    value.flags = 0;
    value.scene_process_id = foreground_process;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::foreground_input && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_cg_event;
}

bool modified_semantic_action_does_not_use_background_ax() noexcept {
    BackgroundActionPolicyInput value = explicit_semantic_input();
    value.modifiers = SACCADE_AGENT_MODIFIER_SHIFT;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::activation_required && decision.result == SACCADE_AGENT_ERROR_ACTIVATION_REQUIRED &&
           (decision.result_flags & saccade::agent::background_action_result_background_ax) == 0;
}

bool drag_is_unsupported() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.kind = SACCADE_AGENT_ACTION_DRAG_DROP;
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::unsupported && decision.result == SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED &&
           decision.result_flags == 0;
}

bool stale_identity_is_refused() noexcept {
    BackgroundActionPolicyInput value = explicit_semantic_input();
    value.identity_current = false;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::stale && decision.result == SACCADE_AGENT_ERROR_STALE_GENERATION &&
           decision.result_flags == 0;
}

bool mismatched_target_window_is_refused() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.target_window_id = scene_window + 1;
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::stale && decision.result == SACCADE_AGENT_ERROR_STALE_GENERATION &&
           decision.result_flags == 0;
}

bool missing_scene_window_is_refused() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.scene_window_id = 0;
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::stale && decision.result == SACCADE_AGENT_ERROR_STALE_GENERATION &&
           decision.result_flags == 0;
}

bool activation_dry_run_reports_only_the_proposed_side_effect() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;
    value.batch_flags = SACCADE_AGENT_BATCH_DRY_RUN;

    const auto decision = saccade::agent::choose_background_action(value);
    const uint32_t forbidden =
        saccade::agent::background_action_result_window_activated | saccade::agent::background_action_result_cg_event;
    return decision.backend == BackgroundActionBackend::activation_input && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_would_activate && (decision.result_flags & forbidden) == 0;
}

bool background_dry_run_reports_ax_without_input_side_effects() noexcept {
    BackgroundActionPolicyInput value = explicit_semantic_input();
    value.batch_flags = SACCADE_AGENT_BATCH_DRY_RUN;

    const auto decision = saccade::agent::choose_background_action(value);
    const uint32_t forbidden = saccade::agent::background_action_result_window_activated |
                               saccade::agent::background_action_result_cg_event | saccade::agent::background_action_result_would_activate;
    return decision.backend == BackgroundActionBackend::background_ax && decision.result == SACCADE_AGENT_OK &&
           decision.result_flags == saccade::agent::background_action_result_background_ax && (decision.result_flags & forbidden) == 0;
}

bool disabled_target_is_refused_before_activation() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.flags |= SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;
    value.target_flags |= SACCADE_AGENT_TARGET_DISABLED;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::unsupported &&
           decision.result == SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE && decision.result_flags == 0;
}

bool secure_target_is_refused_before_axpress() noexcept {
    BackgroundActionPolicyInput value = explicit_semantic_input();
    value.target_flags |= SACCADE_AGENT_TARGET_SECURE;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::unsupported && decision.result == SACCADE_AGENT_ERROR_SECURE_SURFACE &&
           decision.result_flags == 0;
}

bool activation_flag_requires_an_explicit_window() noexcept {
    BackgroundActionPolicyInput value = explicit_visual_input();
    value.explicit_window = false;
    value.flags = SACCADE_AGENT_ACTION_ALLOW_ACTIVATION;
    value.scene_process_id = foreground_process;

    const auto decision = saccade::agent::choose_background_action(value);
    return decision.backend == BackgroundActionBackend::unsupported && decision.result == SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED &&
           decision.result_flags == 0;
}

} // namespace

int main() {
    if (!semantic_target_uses_background_ax())
        return static_cast<int>(TestResult::semantic_background_failed);
    if (!explicit_visual_target_requires_activation())
        return static_cast<int>(TestResult::activation_required_failed);
    if (!explicit_visual_target_uses_allowed_activation())
        return static_cast<int>(TestResult::activation_allowed_failed);
    if (!foreground_visual_target_uses_foreground_input())
        return static_cast<int>(TestResult::foreground_input_failed);
    if (!foreground_semantic_target_does_not_take_background_path())
        return static_cast<int>(TestResult::foreground_semantic_failed);
    if (!modified_semantic_action_does_not_use_background_ax())
        return static_cast<int>(TestResult::modifiers_failed);
    if (!drag_is_unsupported())
        return static_cast<int>(TestResult::drag_failed);
    if (!stale_identity_is_refused())
        return static_cast<int>(TestResult::stale_identity_failed);
    if (!mismatched_target_window_is_refused())
        return static_cast<int>(TestResult::mismatched_window_failed);
    if (!missing_scene_window_is_refused())
        return static_cast<int>(TestResult::missing_window_failed);
    if (!activation_dry_run_reports_only_the_proposed_side_effect())
        return static_cast<int>(TestResult::dry_run_activation_failed);
    if (!background_dry_run_reports_ax_without_input_side_effects())
        return static_cast<int>(TestResult::dry_run_background_failed);
    if (!disabled_target_is_refused_before_activation())
        return static_cast<int>(TestResult::disabled_target_failed);
    if (!secure_target_is_refused_before_axpress())
        return static_cast<int>(TestResult::secure_target_failed);
    if (!activation_flag_requires_an_explicit_window())
        return static_cast<int>(TestResult::implicit_activation_failed);
    return static_cast<int>(TestResult::success);
}

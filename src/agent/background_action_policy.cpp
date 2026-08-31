#include "agent/background_action_policy.hpp"

namespace saccade::agent {
namespace {

constexpr BackgroundActionDecision stale() noexcept {
    return {BackgroundActionBackend::stale, SACCADE_AGENT_ERROR_STALE_GENERATION, 0};
}

constexpr BackgroundActionDecision unsupported() noexcept {
    return {BackgroundActionBackend::unsupported, SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, 0};
}

constexpr BackgroundActionDecision inaccessible() noexcept {
    return {BackgroundActionBackend::unsupported, SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, 0};
}

constexpr BackgroundActionDecision secure_surface() noexcept {
    return {BackgroundActionBackend::unsupported, SACCADE_AGENT_ERROR_SECURE_SURFACE, 0};
}

constexpr BackgroundActionDecision activation_required() noexcept {
    return {BackgroundActionBackend::activation_required, SACCADE_AGENT_ERROR_ACTIVATION_REQUIRED, 0};
}

bool semantic_ax_eligible(const BackgroundActionPolicyInput& input) noexcept {
    const bool semantic = (input.target_sources & SACCADE_AGENT_TARGET_SOURCE_ACCESSIBILITY) != 0;
    const bool invokable = (input.target_capabilities & SACCADE_AGENT_TARGET_INVOKE) != 0;
    const bool action = input.kind == SACCADE_AGENT_ACTION_CLICK || input.kind == SACCADE_AGENT_ACTION_INVOKE;
    return semantic && invokable && action && input.modifiers == 0 && input.button_bits == SACCADE_AGENT_BUTTON_LEFT &&
           input.repeat_count == 1 && (input.flags & SACCADE_AGENT_ACTION_EXPLICIT_POINTS) == 0;
}

} // namespace

BackgroundActionDecision choose_background_action(const BackgroundActionPolicyInput& input) noexcept {
    constexpr uint32_t action_flag_mask =
        SACCADE_AGENT_ACTION_EXPLICIT_POINTS | SACCADE_AGENT_ACTION_ALLOW_ACTIVATION | SACCADE_AGENT_ACTION_EXPLICIT_WINDOW;
    constexpr uint32_t batch_flag_mask = SACCADE_AGENT_BATCH_DRY_RUN | SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION;
    if (!input.identity_current || input.target_window_id == 0 || input.scene_window_id == 0 || input.scene_process_id == 0 ||
        input.target_window_id != input.scene_window_id ||
        (!input.explicit_window && input.scene_process_id != input.foreground_process_id)) {
        return stale();
    }
    if (input.reserved != 0 || (input.flags & ~action_flag_mask) != 0 || (input.batch_flags & ~batch_flag_mask) != 0)
        return unsupported();
    for (uint8_t byte : input.padding)
        if (byte != 0)
            return unsupported();
    if (input.kind != SACCADE_AGENT_ACTION_CLICK && input.kind != SACCADE_AGENT_ACTION_INVOKE)
        return unsupported();
    const bool explicit_flag = (input.flags & SACCADE_AGENT_ACTION_EXPLICIT_WINDOW) != 0;
    if (explicit_flag != input.explicit_window ||
        (!input.explicit_window && (input.flags & SACCADE_AGENT_ACTION_ALLOW_ACTIVATION) != 0)) {
        return unsupported();
    }
    if ((input.target_flags & SACCADE_AGENT_TARGET_SECURE) != 0)
        return secure_surface();
    if ((input.target_flags & (SACCADE_AGENT_TARGET_ACTIONABLE | SACCADE_AGENT_TARGET_DISABLED)) !=
        SACCADE_AGENT_TARGET_ACTIONABLE) {
        return inaccessible();
    }

    if (input.explicit_window && semantic_ax_eligible(input)) {
        return {BackgroundActionBackend::background_ax, SACCADE_AGENT_OK, background_action_result_background_ax};
    }
    if (!input.explicit_window) {
        const uint32_t flags = (input.batch_flags & SACCADE_AGENT_BATCH_DRY_RUN) == 0 ? background_action_result_cg_event : 0;
        return {BackgroundActionBackend::foreground_input, SACCADE_AGENT_OK, flags};
    }
    if ((input.flags & SACCADE_AGENT_ACTION_ALLOW_ACTIVATION) == 0)
        return activation_required();
    return {BackgroundActionBackend::activation_input, SACCADE_AGENT_OK, background_action_result_would_activate};
}

} // namespace saccade::agent

#ifndef SACCADE_AGENT_BACKGROUND_ACTION_POLICY_HPP
#define SACCADE_AGENT_BACKGROUND_ACTION_POLICY_HPP

#include <saccade/saccade_agent.h>

#include <cstdint>

namespace saccade::agent {

enum class BackgroundActionBackend : uint32_t {
    foreground_input,
    background_ax,
    activation_input,
    activation_required,
    unsupported,
    stale
};

enum : uint32_t {
    background_action_result_background_ax = SACCADE_AGENT_ACTION_RESULT_BACKGROUND_ACCESSIBILITY,
    background_action_result_would_activate = SACCADE_AGENT_ACTION_RESULT_WOULD_ACTIVATE,
    background_action_result_window_activated = SACCADE_AGENT_ACTION_RESULT_WINDOW_ACTIVATED,
    background_action_result_cg_event = SACCADE_AGENT_ACTION_RESULT_CG_EVENT
};

struct BackgroundActionPolicyInput {
    SaccadeAgentActionKind kind = 0;
    SaccadeAgentActionFlags flags = 0;
    uint32_t batch_flags = 0;
    uint32_t target_capabilities = 0;
    uint32_t target_flags = 0;
    uint16_t target_sources = 0;
    uint16_t reserved = 0;
    SaccadeAgentButtonBits button_bits = 0;
    SaccadeAgentModifierBits modifiers = 0;
    uint32_t repeat_count = 0;
    uint64_t target_window_id = 0;
    uint64_t scene_window_id = 0;
    uint64_t scene_process_id = 0;
    uint64_t foreground_process_id = 0;
    bool explicit_window = false;
    bool identity_current = false;
    uint8_t padding[6]{};
};

struct BackgroundActionDecision {
    BackgroundActionBackend backend = BackgroundActionBackend::unsupported;
    SaccadeAgentResult result = SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED;
    uint32_t result_flags = 0;
};

BackgroundActionDecision choose_background_action(const BackgroundActionPolicyInput&) noexcept;

static_assert(sizeof(BackgroundActionPolicyInput) == 80);
static_assert(sizeof(BackgroundActionDecision) == 12);

} // namespace saccade::agent

#endif

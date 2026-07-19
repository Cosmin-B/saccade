#ifndef SACCADE_TOOLS_AGENT_RESULT_TEXT_HPP
#define SACCADE_TOOLS_AGENT_RESULT_TEXT_HPP

#include <saccade/saccade_agent.h>

namespace saccade::tools {

constexpr const char* agent_result_text(SaccadeAgentResult result) noexcept {
    switch (result) {
    case SACCADE_AGENT_OK:
        return "ok";
    case SACCADE_AGENT_ERROR_INVALID_MESSAGE:
        return "invalid message";
    case SACCADE_AGENT_ERROR_CAPABILITY_DENIED:
        return "capability denied";
    case SACCADE_AGENT_ERROR_STALE_GENERATION:
        return "stale generation";
    case SACCADE_AGENT_ERROR_TARGET_NOT_FOUND:
        return "target not found";
    case SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE:
        return "target inaccessible";
    case SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED:
        return "action unsupported";
    case SACCADE_AGENT_ERROR_PERMISSION_DENIED:
        return "permission denied";
    case SACCADE_AGENT_ERROR_SECURE_SURFACE:
        return "secure surface";
    case SACCADE_AGENT_ERROR_FOCUS_CHANGED:
        return "focus changed";
    case SACCADE_AGENT_ERROR_INVALID_TRANSFORM:
        return "invalid transform";
    case SACCADE_AGENT_ERROR_TIMEOUT:
        return "timeout";
    case SACCADE_AGENT_ERROR_CANCELLED:
        return "cancelled";
    case SACCADE_AGENT_ERROR_EXECUTOR_LOST:
        return "executor lost";
    case SACCADE_AGENT_ERROR_BACKEND:
        return "backend failure";
    case SACCADE_AGENT_ERROR_CAPACITY:
        return "capacity exhausted";
    default:
        return "unknown result";
    }
}

} // namespace saccade::tools

#endif

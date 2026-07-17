#include <saccade/saccade_agent.h>

_Static_assert(sizeof(SaccadeAgentMessageHeader) == 16, "message header ABI");
_Static_assert(sizeof(SaccadeAgentHelloRequest) == 48, "hello request ABI");
_Static_assert(sizeof(SaccadeAgentHelloCompletion) == 56, "hello completion ABI");
_Static_assert(sizeof(SaccadeAgentTarget) == 88, "target ABI");
_Static_assert(sizeof(SaccadeAgentQueryFilter) == 72, "query filter ABI");
_Static_assert(sizeof(SaccadeAgentAction) == 80, "action ABI");
_Static_assert(sizeof(SaccadeAgentActionCompletion) == 184, "completion ABI");

int main(void) {
    SaccadeAgentHelloRequest hello = {0};
    hello.header.struct_size = (uint32_t)sizeof(hello);
    hello.header.api_version = SACCADE_AGENT_API_VERSION;
    hello.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_REQUEST;
    SaccadeAgentObserveRequest request = {0};
    request.header.struct_size = (uint32_t)sizeof(request);
    request.header.api_version = SACCADE_AGENT_API_VERSION;
    request.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request.scope.kind = SACCADE_AGENT_SCOPE_ACTIVE_WINDOW;
    request.scope.source_mode = SACCADE_AGENT_SOURCE_FUSED;
    request.freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    request.maximum_targets = SACCADE_AGENT_MAX_TARGETS;
    request.target_stride = (uint32_t)sizeof(SaccadeAgentTarget);
    request.total_capacity = SACCADE_AGENT_MAX_MESSAGE_BYTES;
    return hello.header.struct_size == 48 && request.header.struct_size == 96 && request.target_stride == 88 ? 0 : 1;
}

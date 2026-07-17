#include <saccade/saccade_agent.h>

#include <cstddef>
#include <type_traits>

static_assert(sizeof(SaccadeAgentMessageHeader) == 16);
static_assert(sizeof(SaccadeAgentHelloRequest) == 48);
static_assert(sizeof(SaccadeAgentHelloCompletion) == 56);
static_assert(sizeof(SaccadeAgentRectQ8) == 16);
static_assert(sizeof(SaccadeAgentPointQ8) == 8);
static_assert(sizeof(SaccadeAgentScope) == 32);
static_assert(sizeof(SaccadeAgentFreshness) == 24);
static_assert(sizeof(SaccadeAgentGeneration) == 72);
static_assert(sizeof(SaccadeAgentTarget) == 88);
static_assert(sizeof(SaccadeAgentQueryFilter) == 72);
static_assert(sizeof(SaccadeAgentObserveRequest) == 96);
static_assert(sizeof(SaccadeAgentObserveCompletion) == 160);
static_assert(sizeof(SaccadeAgentQueryRequest) == 112);
static_assert(sizeof(SaccadeAgentQueryCompletion) == 160);
static_assert(sizeof(SaccadeAgentPreconditions) == 72);
static_assert(sizeof(SaccadeAgentAction) == 80);
static_assert(sizeof(SaccadeAgentActionBatch) == 136);
static_assert(sizeof(SaccadeAgentActionResult) == 48);
static_assert(sizeof(SaccadeAgentPhysicalState) == 48);
static_assert(sizeof(SaccadeAgentActionCompletion) == 184);

static_assert(offsetof(SaccadeAgentObserveRequest, header) == 0);
static_assert(offsetof(SaccadeAgentObserveCompletion, header) == 0);
static_assert(offsetof(SaccadeAgentQueryRequest, header) == 0);
static_assert(offsetof(SaccadeAgentQueryCompletion, header) == 0);
static_assert(offsetof(SaccadeAgentActionBatch, header) == 0);
static_assert(offsetof(SaccadeAgentActionCompletion, header) == 0);
static_assert(offsetof(SaccadeAgentObserveCompletion, generation) == 56);
static_assert(offsetof(SaccadeAgentQueryRequest, freshness) == 88);
static_assert(offsetof(SaccadeAgentActionBatch, preconditions) == 40);
static_assert(offsetof(SaccadeAgentActionBatch, actions_offset) == 120);
static_assert(offsetof(SaccadeAgentActionCompletion, physical_state) == 64);
static_assert(offsetof(SaccadeAgentActionCompletion, next_generation) == 112);

static_assert(std::is_standard_layout_v<SaccadeAgentTarget>);
static_assert(std::is_standard_layout_v<SaccadeAgentActionBatch>);
static_assert(std::is_trivially_copyable_v<SaccadeAgentQueryFilter>);
static_assert(std::is_trivially_copyable_v<SaccadeAgentActionCompletion>);

int main() {
    SaccadeAgentActionBatch batch{};
    batch.header.struct_size = sizeof(batch);
    batch.header.api_version = SACCADE_AGENT_API_VERSION;
    batch.header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_BATCH;
    batch.header.flags = SACCADE_AGENT_BATCH_DRY_RUN;
    batch.policy = SACCADE_AGENT_BATCH_STOP_ON_FAILURE;
    batch.action_stride = sizeof(SaccadeAgentAction);
    return batch.header.struct_size == 136 && batch.action_stride == 80 ? 0 : 1;
}

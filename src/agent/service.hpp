#ifndef SACCADE_AGENT_SERVICE_HPP
#define SACCADE_AGENT_SERVICE_HPP

#include "interaction/action_planner.hpp"
#include "interaction/interaction_state.hpp"
#include "scene/packet.hpp"

#include <saccade/saccade_agent.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::agent {

using AcquireSceneFn = SaccadeResult (*)(void*, scene::PacketView*) noexcept;
using ExecutePlanFn = SaccadeResult (*)(void*, SaccadeSpanU8, uint32_t, uint64_t) noexcept;
using ReadPhysicalStateFn = SaccadeResult (*)(void*, SaccadeAgentPhysicalState*) noexcept;
using AbortInputFn = SaccadeResult (*)(void*) noexcept;
using CycleWindowFn = SaccadeResult (*)(void*, bool backward) noexcept;

struct ServiceConfig {
    void* context = nullptr;
    AcquireSceneFn acquire_scene = nullptr;
    interaction::ReadInteractionStateFn read_state = nullptr;
    ExecutePlanFn execute_plan = nullptr;
    ReadPhysicalStateFn read_physical_state = nullptr;
    AbortInputFn abort_input = nullptr;
    CycleWindowFn cycle_window = nullptr;
    SaccadeAgentCapabilityBits capability_bits = 0;
};

struct ServiceStats {
    uint64_t requests = 0;
    uint64_t observations = 0;
    uint64_t queries = 0;
    uint64_t action_batches = 0;
    uint64_t actions = 0;
    uint64_t rejected_messages = 0;
    uint64_t rejected_capabilities = 0;
    uint64_t rejected_stale = 0;
    uint64_t failures = 0;
};

class Service final {
  public:
    SaccadeResult initialize(ServiceConfig) noexcept;
    SaccadeResult process(SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                          SaccadeMutableSpanU8 output, size_t* output_size) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] ServiceStats stats() const noexcept { return stats_; }

  private:
    SaccadeResult observe(const SaccadeAgentObserveRequest&, SaccadeAgentCapabilityBits, SaccadeMutableSpanU8,
                          size_t*) noexcept;
    SaccadeResult query(SaccadeSpanU8, const SaccadeAgentQueryRequest&, SaccadeAgentCapabilityBits,
                        SaccadeMutableSpanU8, size_t*) noexcept;
    SaccadeResult act(SaccadeSpanU8, const SaccadeAgentActionBatch&, SaccadeAgentCapabilityBits, uint64_t,
                      SaccadeMutableSpanU8, size_t*) noexcept;

    interaction::ActionPlanner planner_{};
    interaction::ActionPlanStorage plan_storage_{};
    ServiceConfig config_{};
    ServiceStats stats_{};
    uint64_t next_plan_id_ = 1;
    bool initialized_ = false;
};

static_assert(sizeof(ServiceStats) == 72);

} // namespace saccade::agent

#endif

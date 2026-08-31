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

using AcquireScopedSceneFn = SaccadeResult (*)(void*, const SaccadeAgentScope&, const SaccadeAgentFreshness&, scene::PacketView*,
                                               interaction::InteractionState*) noexcept;
using ExecutePlanFn = SaccadeResult (*)(void*, SaccadeSpanU8, uint32_t, uint64_t) noexcept;
using ReadPhysicalStateFn = SaccadeResult (*)(void*, SaccadeAgentPhysicalState*) noexcept;
using AbortInputFn = SaccadeResult (*)(void*) noexcept;
using CycleWindowFn = SaccadeResult (*)(void*, bool backward) noexcept;

struct BackgroundActionExecution {
    SaccadeAgentResult result = SACCADE_AGENT_ERROR_BACKEND;
    int32_t platform_error = 0;
    SaccadeAgentActionResultFlags result_flags = 0;
    uint32_t reserved = 0;
};

using ExecuteBackgroundPressFn = SaccadeResult (*)(void*, uint64_t request_id, const SaccadeAgentGeneration&,
                                                    uint64_t session_epoch, const SaccadeAgentTarget&, bool dry_run,
                                                    BackgroundActionExecution*) noexcept;
using PrepareWindowActivationFn = SaccadeResult (*)(void*, uint64_t request_id, uint64_t process_id, uint64_t window_id,
                                                    uint64_t source_generation, uint64_t deadline_ns, bool dry_run,
                                                    BackgroundActionExecution*) noexcept;

struct ServiceConfig {
    void* context = nullptr;
    AcquireScopedSceneFn acquire_scoped_scene = nullptr;
    ExecutePlanFn execute_plan = nullptr;
    ReadPhysicalStateFn read_physical_state = nullptr;
    AbortInputFn abort_input = nullptr;
    CycleWindowFn cycle_window = nullptr;
    SaccadeAgentCapabilityBits capability_bits = 0;
    ExecuteBackgroundPressFn execute_background_press = nullptr;
    PrepareWindowActivationFn prepare_window_activation = nullptr;
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
    void cancel_pending_requests() noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] ServiceStats stats() const noexcept { return stats_; }

  private:
    static constexpr size_t pending_response_capacity =
        sizeof(SaccadeAgentActionCompletion) + SACCADE_AGENT_MAX_ACTIONS * sizeof(SaccadeAgentActionResult);

    struct PendingVerification {
        std::array<uint8_t, pending_response_capacity> response{};
        std::array<SaccadeAgentAction, SACCADE_AGENT_MAX_ACTIONS> actions{};
        SaccadeAgentActionBatch batch{};
        SaccadeAgentScope scope{};
        uint64_t after_generation = 0;
        uint32_t response_size = 0;
        bool active = false;
        bool cancelled = false;
        uint8_t reserved[2]{};
    };

    enum class PendingFreshnessKind : uint32_t { none, observe, query };

    struct PendingFreshness {
        SaccadeAgentObserveRequest observe{};
        SaccadeAgentQueryRequest query{};
        uint64_t deadline_ns = 0;
        PendingFreshnessKind kind = PendingFreshnessKind::none;
        bool cancelled = false;
        uint8_t reserved[3]{};
    };

    SaccadeResult observe(const SaccadeAgentObserveRequest&, SaccadeAgentCapabilityBits, uint64_t, SaccadeMutableSpanU8,
                          size_t*) noexcept;
    SaccadeResult query(SaccadeSpanU8, const SaccadeAgentQueryRequest&, SaccadeAgentCapabilityBits, uint64_t, SaccadeMutableSpanU8,
                        size_t*) noexcept;
    SaccadeResult act(SaccadeSpanU8, const SaccadeAgentActionBatch&, SaccadeAgentCapabilityBits, uint64_t, SaccadeMutableSpanU8,
                      size_t*) noexcept;
    bool pending_matches(SaccadeSpanU8, const SaccadeAgentActionBatch&) const noexcept;
    SaccadeResult resume_verification(const SaccadeAgentActionBatch&, uint64_t, SaccadeMutableSpanU8, size_t*) noexcept;
    void retain_verification(SaccadeSpanU8, const SaccadeAgentActionBatch&, const SaccadeAgentScope&, uint64_t, SaccadeSpanU8) noexcept;

    interaction::ActionPlanner planner_{};
    interaction::ActionPlanStorage plan_storage_{};
    ServiceConfig config_{};
    ServiceStats stats_{};
    PendingVerification pending_verification_{};
    PendingFreshness pending_freshness_{};
    uint64_t next_plan_id_ = 1;
    bool initialized_ = false;
};

static_assert(sizeof(ServiceStats) == 72);
static_assert(sizeof(BackgroundActionExecution) == 16);

} // namespace saccade::agent

#endif

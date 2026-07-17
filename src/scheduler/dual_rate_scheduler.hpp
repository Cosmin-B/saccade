#ifndef SACCADE_SCHEDULER_DUAL_RATE_SCHEDULER_HPP
#define SACCADE_SCHEDULER_DUAL_RATE_SCHEDULER_HPP

#include <saccade/saccade.h>

#include <cstdint>

namespace saccade::scheduler {

constexpr uint64_t interaction_period_120hz_ns = UINT64_C(8'333'333);
constexpr uint64_t scene_period_30hz_ns = UINT64_C(33'333'333);
constexpr uint64_t scene_period_60hz_ns = UINT64_C(16'666'667);

struct DualRateConfig {
    uint64_t interaction_period_ns = interaction_period_120hz_ns;
    uint64_t scene_period_ns = scene_period_30hz_ns;
};

struct ScheduledWork {
    bool interaction_due = false;
    bool scene_start = false;
    uint16_t reserved = 0;
    uint32_t reserved2 = 0;
    uint64_t interaction_time_ns = 0;
    uint64_t scene_time_ns = 0;
};

struct DualRateStats {
    uint64_t interaction_deadlines = 0;
    uint64_t interaction_ticks = 0;
    uint64_t interaction_skipped = 0;
    uint64_t scene_deadlines = 0;
    uint64_t scene_started = 0;
    uint64_t scene_completed = 0;
    uint64_t scene_replaced = 0;
    uint64_t time_regressions = 0;
};

class DualRateScheduler final {
  public:
    SaccadeResult initialize(uint64_t now_ns, DualRateConfig = {}) noexcept;
    SaccadeResult advance(uint64_t now_ns, ScheduledWork*) noexcept;
    SaccadeResult complete_scene(ScheduledWork*) noexcept;
    SaccadeResult cancel_scene(bool discard_pending, ScheduledWork*) noexcept;

    [[nodiscard]] bool scene_running() const noexcept { return scene_running_; }

    [[nodiscard]] bool scene_pending() const noexcept { return scene_pending_; }

    [[nodiscard]] DualRateStats stats() const noexcept { return stats_; }

  private:
    DualRateConfig config_{};
    DualRateStats stats_{};
    uint64_t last_now_ns_ = 0;
    uint64_t next_interaction_ns_ = 0;
    uint64_t next_scene_ns_ = 0;
    uint64_t pending_scene_ns_ = 0;
    bool initialized_ = false;
    bool scene_running_ = false;
    bool scene_pending_ = false;
};

static_assert(sizeof(ScheduledWork) == 24);
static_assert(sizeof(DualRateStats) == 64);

} // namespace saccade::scheduler

#endif

#ifndef SACCADE_INPUT_PHYSICAL_REDUCER_HPP
#define SACCADE_INPUT_PHYSICAL_REDUCER_HPP

#include "input/plan.hpp"

#include <array>
#include <cstdint>

namespace saccade::input {

constexpr uint32_t maximum_held_keys = 16;

struct SyntheticRelease {
    uint32_t buttons = 0;
    uint32_t modifiers = 0;
    uint32_t held_key_count = 0;
    uint32_t reserved = 0;
    std::array<uint32_t, maximum_held_keys> held_keys{};
};

struct PhysicalReducerStats {
    uint64_t plans_started = 0;
    uint64_t commands_completed = 0;
    uint64_t plans_completed = 0;
    uint64_t aborts = 0;
    uint64_t physical_overrides = 0;
    uint64_t permission_losses = 0;
    uint64_t releases = 0;
    uint64_t backend_failures = 0;
    uint64_t timeouts = 0;
};

class PhysicalInputReducer final {
  public:
    SaccadeResult initialize(uint64_t permission_epoch, int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept;
    SaccadeResult begin(const PlanView&, uint32_t available_permissions, uint64_t now_ns) noexcept;
    SaccadeResult advance(uint32_t completed_commands) noexcept;
    SaccadeResult abort(SyntheticRelease*) noexcept;
    SaccadeResult backend_failure(SyntheticRelease*) noexcept;
    SaccadeResult expire(uint64_t now_ns, SyntheticRelease*) noexcept;
    SaccadeResult physical_override(int32_t pointer_x_q8, int32_t pointer_y_q8, SyntheticRelease*) noexcept;
    SaccadeResult permission_lost(uint64_t new_permission_epoch, SyntheticRelease*) noexcept;
    SaccadeResult shutdown(SyntheticRelease*) noexcept;

    [[nodiscard]] SaccadePhysicalInputState state() const noexcept;

    [[nodiscard]] PhysicalReducerStats stats() const noexcept { return stats_; }

  private:
    bool preflight(const PlanView&) const noexcept;
    bool apply(const SaccadeInputCommand&) noexcept;
    void collect_release(SyntheticRelease*) noexcept;

    const SaccadeInputPlanHeader* active_header_ = nullptr;
    const SaccadeInputCommand* active_commands_ = nullptr;
    PhysicalReducerStats stats_{};
    std::array<uint32_t, maximum_held_keys> held_keys_{};
    uint64_t permission_epoch_ = 0;
    uint64_t physical_sequence_ = 0;
    uint64_t active_lease_id_ = 0;
    uint64_t active_deadline_ns_ = 0;
    int32_t pointer_x_q8_ = 0;
    int32_t pointer_y_q8_ = 0;
    uint32_t buttons_ = 0;
    uint32_t modifiers_ = 0;
    uint32_t held_key_count_ = 0;
    uint32_t completed_commands_ = 0;
    bool initialized_ = false;
    bool active_dry_run_ = false;
};

static_assert(sizeof(SyntheticRelease) == 80);

} // namespace saccade::input

#endif

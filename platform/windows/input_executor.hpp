#ifndef SACCADE_PLATFORM_WINDOWS_INPUT_EXECUTOR_HPP
#define SACCADE_PLATFORM_WINDOWS_INPUT_EXECUTOR_HPP

#include "input/physical_reducer.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>

namespace saccade::platform::windows {

struct VirtualDesktop {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t topology_epoch = 0;
};

using SubmitInputFn = uint32_t (*)(void*, const INPUT*, uint32_t) noexcept;
using ActivateWindowFn = SaccadeResult (*)(void*, uint64_t) noexcept;
using PreflightInputFn = SaccadeResult (*)(void*, const input::PlanView&, uint32_t command_index,
                                           uint64_t now_ns) noexcept;

struct InputSink {
    void* context = nullptr;
    SubmitInputFn submit = nullptr;
    ActivateWindowFn activate_window = nullptr;
    PreflightInputFn preflight = nullptr;
};

struct InputExecutionResult {
    uint64_t plan_id = 0;
    uint32_t commands_completed = 0;
    uint32_t native_events = 0;
    uint32_t buttons = 0;
    uint32_t flags = 0;
};

enum : uint32_t { input_execution_pending = UINT32_C(1) << 0 };

struct InputExecutorStats {
    uint64_t plans = 0;
    uint64_t commands = 0;
    uint64_t native_events = 0;
    uint64_t submit_calls = 0;
    uint64_t failures = 0;
    uint64_t dry_runs = 0;
    uint64_t releases = 0;
    uint64_t timed_ticks = 0;
};

class InputExecutor final {
  public:
    SaccadeResult initialize(const VirtualDesktop&, const InputSink&, uint64_t permission_epoch, int32_t pointer_x_q8,
                             int32_t pointer_y_q8) noexcept;
    SaccadeResult execute(SaccadeSpanU8, uint32_t available_permissions, uint64_t now_ns,
                          InputExecutionResult*) noexcept;
    SaccadeResult advance(uint64_t now_ns, InputExecutionResult*) noexcept;
    SaccadeResult release_all() noexcept;
    SaccadeResult update_desktop(const VirtualDesktop&) noexcept;
    SaccadeResult physical_override(int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept;
    SaccadeResult permission_lost(uint64_t new_permission_epoch) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] input::PhysicalInputReducer& physical_state() noexcept { return physical_; }

    [[nodiscard]] bool synthetic_input_active() const noexcept {
        const SaccadePhysicalInputState state = physical_.state();
        return state.buttons != 0 || state.modifiers != 0 || state.active_lease_id != 0 ||
               timed_kind_ != TimedKind::none || pending_release_count_ != 0;
    }

    [[nodiscard]] InputExecutorStats stats() const noexcept { return stats_; }

  private:
    static constexpr uint32_t pending_release_capacity_ = 288;

    SaccadeResult append_pending_release(const INPUT&) noexcept;
    SaccadeResult queue_release(const input::SyntheticRelease&) noexcept;
    SaccadeResult retry_pending_releases() noexcept;
    SaccadeResult emit_release(const input::SyntheticRelease&) noexcept;
    SaccadeResult continue_execution(uint64_t, InputExecutionResult*) noexcept;

    enum class TimedKind : uint8_t { none, move, scroll, wait, hold };

    VirtualDesktop desktop_{};
    InputSink sink_{};
    input::PhysicalInputReducer physical_{};
    InputExecutorStats stats_{};
    std::array<INPUT, pending_release_capacity_> pending_releases_{};
    input::PlanStorage active_storage_{};
    input::PlanView active_plan_{};
    SaccadeSpanU8 active_bytes_{};
    uint64_t timed_start_ns_ = 0;
    uint64_t timed_end_ns_ = 0;
    uint64_t next_tick_ns_ = 0;
    int32_t timed_start_x_q8_ = 0;
    int32_t timed_start_y_q8_ = 0;
    uint32_t next_command_ = 0;
    uint32_t pending_release_count_ = 0;
    TimedKind timed_kind_ = TimedKind::none;
    bool initialized_ = false;
    bool shutdown_pending_ = false;
};

uint32_t submit_with_send_input(void*, const INPUT*, uint32_t) noexcept;
bool synthetic_input_active(void*) noexcept;
SaccadeResult neutralize_synthetic_input(void*) noexcept;

static_assert(sizeof(VirtualDesktop) == 24);
static_assert(sizeof(InputExecutionResult) == 24);
static_assert(sizeof(InputExecutorStats) == 64);

} // namespace saccade::platform::windows

#endif

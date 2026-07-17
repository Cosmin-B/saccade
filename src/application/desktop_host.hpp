#ifndef SACCADE_APPLICATION_DESKTOP_HOST_HPP
#define SACCADE_APPLICATION_DESKTOP_HOST_HPP

#include "application/hotkeys.hpp"

#include <cstdint>

namespace saccade::application {

using DispatchDesktopCommandFn = SaccadeResult (*)(void*, Command, uint64_t) noexcept;
using SetDesktopSuspendedFn = SaccadeResult (*)(void*, bool) noexcept;
using DesktopHostOperationFn = SaccadeResult (*)(void*) noexcept;
using ObserveDesktopInputFn = void (*)(void*, uint64_t) noexcept;

struct DesktopHostCallbacks {
    void* context = nullptr;
    DispatchDesktopCommandFn dispatch = nullptr;
    SetDesktopSuspendedFn set_suspended = nullptr;
    DesktopHostOperationFn neutralize_input = nullptr;
    ObserveDesktopInputFn observe_input = nullptr;
    DesktopHostOperationFn open_settings = nullptr;
    DesktopHostOperationFn restart = nullptr;
    DesktopHostOperationFn quit = nullptr;
};

struct DesktopHostStats {
    uint64_t commands = 0;
    uint64_t interaction_commands = 0;
    uint64_t physical_inputs = 0;
    uint64_t suspension_changes = 0;
    uint64_t settings_opens = 0;
    uint64_t restarts = 0;
    uint64_t quit_requests = 0;
    uint64_t neutralizations = 0;
    uint64_t failures = 0;
};

class DesktopHost final {
  public:
    SaccadeResult initialize(DesktopHostCallbacks) noexcept;
    SaccadeResult dispatch(const CommandEvent&) noexcept;
    void observe_physical_input(uint64_t timestamp_ns) noexcept;
    SaccadeResult set_suspended(bool) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool suspended() const noexcept { return suspended_; }

    [[nodiscard]] SaccadeResult last_fault() const noexcept { return last_fault_; }

    [[nodiscard]] DesktopHostStats stats() const noexcept { return stats_; }

  private:
    SaccadeResult run(DesktopHostOperationFn) noexcept;
    SaccadeResult fail(SaccadeResult) noexcept;

    DesktopHostCallbacks callbacks_{};
    DesktopHostStats stats_{};
    SaccadeResult last_fault_ = SACCADE_OK;
    bool initialized_ = false;
    bool suspended_ = false;
};

void dispatch_desktop_command(void*, const CommandEvent&) noexcept;
void observe_desktop_input(void*, uint64_t timestamp_ns) noexcept;

static_assert(sizeof(DesktopHostStats) == 72);

} // namespace saccade::application

#endif

#ifndef SACCADE_PLATFORM_MACOS_INPUT_MONITOR_HPP
#define SACCADE_PLATFORM_MACOS_INPUT_MONITOR_HPP

#include "application/hotkeys.hpp"

#include <ApplicationServices/ApplicationServices.h>

#include <saccade/saccade.h>

#include <array>
#include <cstdint>
#include <pthread.h>

namespace saccade::platform::macos {

enum class PhysicalInputKind : uint32_t { pointer = 1, button = 2, scroll = 3, key = 4, modifier = 5 };

struct PhysicalInputEvent {
    uint64_t timestamp_ns = 0;
    int32_t pointer_x_q8 = 0;
    int32_t pointer_y_q8 = 0;
    PhysicalInputKind kind = PhysicalInputKind::pointer;
    uint32_t flags = 0;
};

using PhysicalInputFn = void (*)(void*, const PhysicalInputEvent&) noexcept;

struct InputMonitorSink {
    void* context = nullptr;
    PhysicalInputFn input = nullptr;
    application::KeyFn key = nullptr;
};

struct InputMonitorStats {
    uint64_t events = 0;
    uint64_t pointer_events = 0;
    uint64_t button_events = 0;
    uint64_t scroll_events = 0;
    uint64_t key_events = 0;
    uint64_t modifier_events = 0;
    uint64_t injected_ignored = 0;
    uint64_t tap_reenabled = 0;
    uint64_t session_keys = 0;
};

class InputMonitor final {
  public:
    InputMonitor() noexcept = default;
    ~InputMonitor();

    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;
    InputMonitor(InputMonitor&&) = delete;
    InputMonitor& operator=(InputMonitor&&) = delete;

    SaccadeResult initialize(InputMonitorSink) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] InputMonitorStats stats() const noexcept { return stats_; }

  private:
    static CGEventRef tap_callback(CGEventTapProxy, CGEventType, CGEventRef, void*) noexcept;
    [[nodiscard]] bool owns_thread() const noexcept;
    bool dispatch(CGEventRef) noexcept;

    InputMonitorSink sink_{};
    InputMonitorStats stats_{};
    CFMachPortRef tap_ = nullptr;
    CFRunLoopSourceRef source_ = nullptr;
    CFRunLoopRef run_loop_ = nullptr;
    pthread_t owner_{};
    uint32_t timebase_numer_ = 0;
    uint32_t timebase_denom_ = 0;
    std::array<bool, 256> session_pressed_{};
    bool initialized_ = false;
};

bool physical_input_event_from_cg_event(CGEventRef, uint32_t timebase_numer, uint32_t timebase_denom,
                                        PhysicalInputEvent*) noexcept;

static_assert(sizeof(PhysicalInputEvent) == 24);
static_assert(sizeof(InputMonitorStats) == 72);

} // namespace saccade::platform::macos

#endif

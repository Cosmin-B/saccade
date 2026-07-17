#ifndef SACCADE_APPLICATION_SESSION_KEY_ROUTER_HPP
#define SACCADE_APPLICATION_SESSION_KEY_ROUTER_HPP

#include "application/desktop_runtime.hpp"

#include <array>
#include <cstdint>

namespace saccade::application {

struct SessionKeyRoute {
    SaccadeResult result = SACCADE_OK;
    bool handled = false;
    bool session_ended = false;
    uint8_t reserved[2]{};
};

struct SessionKeyRouterStats {
    uint64_t keys = 0;
    uint64_t symbols = 0;
    uint64_t physical_symbols = 0;
    uint64_t logical_symbols = 0;
    uint64_t controls = 0;
    uint64_t rejected = 0;
    uint64_t passed_through = 0;
};

using SessionCommandFn = SaccadeResult (*)(void*, Command, uint64_t timestamp_ns) noexcept;

struct SessionCommandSink {
    void* context = nullptr;
    SessionCommandFn command = nullptr;
};

class SessionKeyRouter final {
  public:
    SaccadeResult initialize(DesktopRuntime*, SessionCommandSink) noexcept;
    SaccadeResult replace(const HotkeyBinding*, uint32_t count) noexcept;
    SaccadeResult route(const KeyEvent&, SessionKeyRoute*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] SessionKeyRouterStats stats() const noexcept { return stats_; }

  private:
    std::array<HotkeyBinding, maximum_hotkey_bindings> bindings_{};
    DesktopRuntime* runtime_ = nullptr;
    SessionCommandSink sink_{};
    SessionKeyRouterStats stats_{};
    uint32_t binding_count_ = 0;
    bool initialized_ = false;
};

bool route_session_key(void*, const KeyEvent&) noexcept;

static_assert(sizeof(SessionKeyRoute) == 8);
static_assert(sizeof(SessionKeyRouterStats) == 56);

} // namespace saccade::application

#endif

#ifndef SACCADE_APPLICATION_WINDOW_NAVIGATOR_HPP
#define SACCADE_APPLICATION_WINDOW_NAVIGATOR_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t window_navigation_capacity = 256;

enum class WindowDirection : uint32_t { left = 0, right = 1, up = 2, down = 3 };

struct WindowSnapshot {
    std::array<SaccadeWindowInfo, window_navigation_capacity> windows{};
    uint32_t count = 0;
    uint32_t reserved = 0;
};

class WindowNavigator final {
  public:
    SaccadeResult collect(SaccadeAccessibilityProviderDesc, uint64_t excluded_process_id, WindowSnapshot*) noexcept;
    SaccadeResult cycle(const WindowSnapshot&, uint64_t current_window_id, bool forward, uint64_t*) const noexcept;
    SaccadeResult behind(const WindowSnapshot&, uint64_t current_window_id, uint64_t*) const noexcept;
    SaccadeResult directional(const WindowSnapshot&, uint64_t current_window_id, WindowDirection,
                              uint64_t*) const noexcept;
};

static_assert(sizeof(WindowSnapshot) == sizeof(SaccadeWindowInfo) * window_navigation_capacity + 8);

} // namespace saccade::application

#endif

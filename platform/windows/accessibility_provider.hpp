#ifndef SACCADE_PLATFORM_WINDOWS_ACCESSIBILITY_PROVIDER_HPP
#define SACCADE_PLATFORM_WINDOWS_ACCESSIBILITY_PROVIDER_HPP

#include <saccade/saccade_backend.h>

#include <cstdint>

namespace saccade::platform::windows {

struct AccessibilityProviderStats {
    uint64_t requests = 0;
    uint64_t completed = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    uint64_t targets = 0;
    uint64_t copied_bytes = 0;
    uint64_t waits = 0;
    uint64_t window_refreshes = 0;
};

class AccessibilityProvider final {
  public:
    struct Impl;

    AccessibilityProvider() noexcept;
    ~AccessibilityProvider();

    AccessibilityProvider(const AccessibilityProvider&) = delete;
    AccessibilityProvider& operator=(const AccessibilityProvider&) = delete;
    AccessibilityProvider(AccessibilityProvider&&) = delete;
    AccessibilityProvider& operator=(AccessibilityProvider&&) = delete;

    SaccadeResult initialize() noexcept;
    SaccadeResult shutdown() noexcept;
    [[nodiscard]] SaccadeAccessibilityProviderDesc descriptor() noexcept;
    SaccadeResult read_stats(AccessibilityProviderStats*) const noexcept;
    SaccadeResult read_last_native_error(int32_t*) const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    Impl* state_ = nullptr;
    bool initialized_ = false;
};

static_assert(sizeof(AccessibilityProviderStats) == 64);

} // namespace saccade::platform::windows

#endif

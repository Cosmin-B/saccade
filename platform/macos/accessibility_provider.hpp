#ifndef SACCADE_PLATFORM_MACOS_ACCESSIBILITY_PROVIDER_HPP
#define SACCADE_PLATFORM_MACOS_ACCESSIBILITY_PROVIDER_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

struct AccessibilityProviderStats {
    uint64_t requests = 0;
    uint64_t completed = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    uint64_t targets = 0;
    uint64_t elements = 0;
    uint64_t copied_bytes = 0;
    uint64_t incomplete = 0;
    uint64_t waits = 0;
    uint64_t window_refreshes = 0;
};

class AccessibilityProvider final {
  public:
    struct Impl;
    static constexpr size_t storage_size = 128 * 1024;
    static constexpr size_t storage_alignment = 128;

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
    [[nodiscard]] bool permission_granted() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(storage_alignment) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(AccessibilityProviderStats) == 80);

} // namespace saccade::platform::macos

#endif

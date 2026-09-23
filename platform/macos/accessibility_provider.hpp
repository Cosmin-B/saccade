#ifndef SACCADE_PLATFORM_MACOS_ACCESSIBILITY_PROVIDER_HPP
#define SACCADE_PLATFORM_MACOS_ACCESSIBILITY_PROVIDER_HPP

#include <saccade/saccade_backend.h>
#include <saccade/saccade_agent.h>

#include <ApplicationServices/ApplicationServices.h>

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

struct AccessibilityGenerationKey {
    uint64_t session_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t process_id = 0;
    uint64_t window_id = 0;
};

struct AccessibilityPressRequest {
    AccessibilityGenerationKey generation{};
    uint64_t target_id = 0;
    SaccadeRectI32 bounds_q8{};
};

struct AccessibilityPressStatus {
    SaccadeTicketHandle ticket = 0;
    uint32_t state = SACCADE_TICKET_FAILED;
    SaccadeAgentResult result = SACCADE_AGENT_ERROR_BACKEND;
    int32_t native_error = 0;
    uint32_t attempt_count = 0;
    uint32_t reserved = 0;
};

using AccessibilityPerformPress = AXError (*)(void*, AXUIElementRef) noexcept;
using AccessibilityBeforePerformPress = void (*)(void*) noexcept;

struct AccessibilityActionHooks {
    // Copied during initialize. The context must outlive shutdown. The element is borrowed only for this worker-thread call.
    void* context = nullptr;
    AccessibilityPerformPress perform_press = nullptr;
    AccessibilityBeforePerformPress before_perform_press = nullptr;
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

    SaccadeResult initialize(const AccessibilityActionHooks* = nullptr) noexcept;
    SaccadeResult shutdown() noexcept;
    [[nodiscard]] SaccadeAccessibilityProviderDesc descriptor() noexcept;
    SaccadeResult read_stats(AccessibilityProviderStats*) const noexcept;
    SaccadeResult read_last_native_error(int32_t*) const noexcept;
    [[nodiscard]] bool permission_granted() const noexcept;

    // Owner-thread lifecycle. The exact query epochs plus resolved PID/window form the key. Snapshot release does not retire locators.
    SaccadeResult promote_action_generation(const AccessibilityGenerationKey&) noexcept;
    SaccadeResult retire_action_generation(const AccessibilityGenerationKey&) noexcept;
    SaccadeResult retire_action_session(uint64_t session_epoch) noexcept;

    // One bounded AXPress request is executed on the provider worker and polled by the owner.
    SaccadeResult request_press(const AccessibilityPressRequest&, SaccadeTicketHandle*) noexcept;
    SaccadeResult cancel_press(SaccadeTicketHandle) noexcept;
    SaccadeResult wait_press(SaccadeTicketHandle, uint64_t timeout_ns, AccessibilityPressStatus*) noexcept;
    SaccadeResult poll_press(SaccadeTicketHandle, AccessibilityPressStatus*) noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(storage_alignment) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(AccessibilityProviderStats) == 80);

} // namespace saccade::platform::macos

#endif

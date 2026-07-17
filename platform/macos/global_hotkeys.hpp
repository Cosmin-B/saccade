#ifndef SACCADE_PLATFORM_MACOS_GLOBAL_HOTKEYS_HPP
#define SACCADE_PLATFORM_MACOS_GLOBAL_HOTKEYS_HPP

#include "application/hotkeys.hpp"

#include <Carbon/Carbon.h>

#include <array>
#include <cstdint>
#include <pthread.h>

namespace saccade::platform::macos {

class GlobalHotkeys final {
  public:
    GlobalHotkeys() noexcept = default;
    ~GlobalHotkeys();

    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;
    GlobalHotkeys(GlobalHotkeys&&) = delete;
    GlobalHotkeys& operator=(GlobalHotkeys&&) = delete;

    SaccadeResult initialize(application::CommandSink) noexcept;
    SaccadeResult replace(const application::HotkeyBinding*, uint32_t count) noexcept;
    SaccadeResult set_suspended(bool) noexcept;
    SaccadeResult dispatch_registered_id(uint32_t registration_id, uint64_t timestamp_ns) noexcept;
    SaccadeResult dispatch_physical(uint32_t physical_key, uint32_t modifiers, uint64_t timestamp_ns) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool suspended() const noexcept { return suspended_; }

    [[nodiscard]] uint32_t binding_count() const noexcept { return binding_count_; }

    [[nodiscard]] application::HotkeyStats stats() const noexcept { return stats_; }

  private:
    struct BindingSlot {
        application::HotkeyBinding binding_{};
        EventHotKeyRef native_ = nullptr;
    };

    [[nodiscard]] bool owns_thread() const noexcept;
    SaccadeResult register_binding(uint32_t index) noexcept;
    SaccadeResult unregister_binding(uint32_t index) noexcept;
    void unregister_all() noexcept;

    std::array<BindingSlot, application::maximum_hotkey_bindings> bindings_{};
    application::CommandSink sink_{};
    application::HotkeyStats stats_{};
    EventHandlerRef handler_ = nullptr;
    pthread_t owner_{};
    uint32_t binding_count_ = 0;
    bool initialized_ = false;
    bool suspended_ = false;
};

} // namespace saccade::platform::macos

#endif

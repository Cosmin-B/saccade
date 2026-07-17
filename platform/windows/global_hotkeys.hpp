#ifndef SACCADE_PLATFORM_WINDOWS_GLOBAL_HOTKEYS_HPP
#define SACCADE_PLATFORM_WINDOWS_GLOBAL_HOTKEYS_HPP

#include "application/hotkeys.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>

namespace saccade::platform::windows {

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
    SaccadeResult dispatch_physical(uint32_t physical_key, uint32_t modifiers, uint64_t timestamp_ns) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool suspended() const noexcept { return suspended_; }

    [[nodiscard]] uint32_t binding_count() const noexcept { return binding_count_; }

    [[nodiscard]] application::HotkeyStats stats() const noexcept { return stats_; }

  private:
    static LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data) noexcept;
    static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) noexcept;
    LRESULT handle_keyboard(WPARAM message, const KBDLLHOOKSTRUCT&) noexcept;
    [[nodiscard]] bool owns_thread() const noexcept;
    [[nodiscard]] uint64_t timestamp_ns() const noexcept;

    std::array<application::HotkeyBinding, application::maximum_hotkey_bindings> bindings_{};
    std::array<bool, application::maximum_hotkey_bindings> pressed_{};
    std::array<bool, 256> session_pressed_{};
    application::CommandSink sink_{};
    application::HotkeyStats stats_{};
    HHOOK hook_ = nullptr;
    HHOOK mouse_hook_ = nullptr;
    uint64_t counter_frequency_ = 0;
    DWORD owner_thread_id_ = 0;
    uint32_t binding_count_ = 0;
    uint32_t modifier_state_ = 0;
    bool initialized_ = false;
    bool suspended_ = false;

    static thread_local GlobalHotkeys* callback_owner_;
};

} // namespace saccade::platform::windows

#endif

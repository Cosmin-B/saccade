#include "platform/windows/global_hotkeys.hpp"

#include "input/injected_marker.hpp"
#include "platform/windows/keyboard.hpp"

namespace saccade::platform::windows {
namespace {

constexpr uint64_t nanoseconds_per_second = UINT64_C(1'000'000'000);
constexpr uint64_t no_timestamp_ns = 0;
constexpr LRESULT hook_continue = 0;
constexpr LRESULT hook_consumed = 1;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;
constexpr uint32_t binding_flag_mask = application::hotkey_always_active | application::hotkey_session_only;

bool command_valid(application::Command command) noexcept {
    return command >= application::Command::pointer_move && command <= application::last_command;
}

bool binding_valid(const application::HotkeyBinding& binding) noexcept {
    KeyScan scan{};
    return command_valid(binding.command) && binding.physical_key != 0 &&
           scan_from_hid_usage(binding.physical_key, &scan) && modifier_from_scan(scan) == 0 &&
           (binding.modifiers & ~modifier_mask) == 0 && (binding.flags & ~binding_flag_mask) == 0 &&
           (binding.flags & (application::hotkey_always_active | application::hotkey_session_only)) !=
               (application::hotkey_always_active | application::hotkey_session_only);
}

bool bindings_valid(const application::HotkeyBinding* bindings, uint32_t count) noexcept {
    if ((bindings == nullptr) != (count == 0) || count > application::maximum_hotkey_bindings) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (!binding_valid(bindings[index])) return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].physical_key == bindings[index].physical_key &&
                bindings[previous].modifiers == bindings[index].modifiers)
                return false;
        }
    }
    return true;
}

bool key_down(WPARAM message) noexcept {
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}

bool key_up(WPARAM message) noexcept {
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}

uint16_t translated_symbol(const KBDLLHOOKSTRUCT& input, uint32_t modifiers) noexcept {
    if (input.vkCode > UINT8_MAX) return 0;
    std::array<BYTE, 256> state{};
    if (GetKeyboardState(state.data()) == 0) return 0;
    state[input.vkCode] = static_cast<BYTE>(state[input.vkCode] | 0x80U);
    state[VK_SHIFT] = (modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0 ? 0x80 : 0;
    state[VK_CONTROL] = (modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0 ? 0x80 : 0;
    state[VK_MENU] = (modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0 ? 0x80 : 0;
    std::array<wchar_t, 4> text{};
    constexpr UINT preserve_keyboard_state = 4;
    const int count = ToUnicodeEx(input.vkCode, input.scanCode, state.data(), text.data(),
                                  static_cast<int>(text.size()), preserve_keyboard_state, GetKeyboardLayout(0));
    return count == 1 && (text[0] < 0xd800 || text[0] > 0xdfff) ? static_cast<uint16_t>(text[0]) : 0;
}

} // namespace

thread_local GlobalHotkeys* GlobalHotkeys::callback_owner_ = nullptr;

GlobalHotkeys::~GlobalHotkeys() {
    if (initialized_ && owns_thread()) (void)shutdown();
}

bool GlobalHotkeys::owns_thread() const noexcept {
    return initialized_ && owner_thread_id_ == GetCurrentThreadId();
}

SaccadeResult GlobalHotkeys::initialize(application::CommandSink sink) noexcept {
    if (initialized_ || callback_owner_ != nullptr) return SACCADE_ERROR_ALREADY_EXISTS;
    if (sink.command == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart <= 0) return SACCADE_ERROR_BACKEND;
    callback_owner_ = this;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr) {
        callback_owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    HHOOK mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, GlobalHotkeys::mouse_hook, GetModuleHandleW(nullptr), 0);
    if (mouse_hook == nullptr) {
        (void)UnhookWindowsHookEx(hook);
        callback_owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    sink_ = sink;
    hook_ = hook;
    mouse_hook_ = mouse_hook;
    counter_frequency_ = static_cast<uint64_t>(frequency.QuadPart);
    owner_thread_id_ = GetCurrentThreadId();
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::replace(const application::HotkeyBinding* bindings, uint32_t count) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (!bindings_valid(bindings, count)) return SACCADE_ERROR_INVALID_ARGUMENT;
    bindings_.fill({});
    pressed_.fill(false);
    session_pressed_.fill(false);
    for (uint32_t index = 0; index < count; ++index)
        bindings_[index] = bindings[index];
    binding_count_ = count;
    ++stats_.registrations;
    ++stats_.replacements;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::set_suspended(bool value) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    suspended_ = value;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::dispatch_physical(uint32_t physical_key, uint32_t modifiers, uint64_t timestamp) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    ++stats_.events;
    for (uint32_t index = 0; index < binding_count_; ++index) {
        const application::HotkeyBinding& binding = bindings_[index];
        if ((binding.flags & application::hotkey_session_only) != 0) continue;
        if (binding.physical_key != physical_key || binding.modifiers != modifiers) continue;
        if (suspended_ && (binding.flags & application::hotkey_always_active) == 0) {
            ++stats_.suspended;
            return SACCADE_OK;
        }
        if (sink_.command_observed != nullptr) sink_.command_observed(sink_.context, timestamp);
        sink_.command(sink_.context,
                      {timestamp, binding.command, binding.physical_key, binding.modifiers, binding.flags});
        ++stats_.dispatched;
        return SACCADE_OK;
    }
    ++stats_.unknown;
    return SACCADE_ERROR_NOT_FOUND;
}

uint64_t GlobalHotkeys::timestamp_ns() const noexcept {
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0 || counter.QuadPart < 0) return no_timestamp_ns;
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t seconds = ticks / counter_frequency_;
    const uint64_t remainder = ticks % counter_frequency_;
    return seconds * nanoseconds_per_second + remainder * nanoseconds_per_second / counter_frequency_;
}

LRESULT GlobalHotkeys::handle_keyboard(WPARAM message, const KBDLLHOOKSTRUCT& input) noexcept {
    if (!key_down(message) && !key_up(message)) return hook_continue;
    if (input.scanCode > UINT16_MAX) return hook_continue;
    if ((input.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0) return hook_continue;
    if (input.dwExtraInfo == static_cast<ULONG_PTR>(input::injected_event_marker)) return hook_continue;
    const uint64_t timestamp = timestamp_ns();
    const KeyScan scan{static_cast<uint16_t>(input.scanCode), (input.flags & LLKHF_EXTENDED) != 0};
    const uint32_t modifier = modifier_from_scan(scan);
    if (modifier != 0) {
        if (key_down(message) && sink_.command_observed != nullptr) sink_.command_observed(sink_.context, timestamp);
        if (key_down(message))
            modifier_state_ |= modifier;
        else
            modifier_state_ &= ~modifier;
        return hook_continue;
    }
    uint32_t physical_key = 0;
    if (!hid_usage_from_scan(scan, &physical_key)) {
        if (key_down(message) && sink_.input_observed != nullptr) sink_.input_observed(sink_.context, timestamp);
        return hook_continue;
    }

    if (key_up(message)) {
        if (physical_key < session_pressed_.size() && session_pressed_[physical_key]) {
            session_pressed_[physical_key] = false;
            return hook_consumed;
        }
        bool consumed = false;
        for (uint32_t index = 0; index < binding_count_; ++index) {
            if (bindings_[index].physical_key == physical_key && pressed_[index]) {
                pressed_[index] = false;
                consumed = true;
            }
        }
        return consumed ? hook_consumed : hook_continue;
    }
    if (physical_key < session_pressed_.size() && session_pressed_[physical_key]) return hook_consumed;
    if (sink_.key != nullptr && sink_.key(sink_.context, {timestamp, physical_key, modifier_state_,
                                                          translated_symbol(input, modifier_state_), 0})) {
        session_pressed_[physical_key] = true;
        return hook_consumed;
    }
    for (uint32_t index = 0; index < binding_count_; ++index) {
        const application::HotkeyBinding& binding = bindings_[index];
        if ((binding.flags & application::hotkey_session_only) != 0) continue;
        if (binding.physical_key != physical_key || binding.modifiers != modifier_state_) continue;
        if (suspended_ && (binding.flags & application::hotkey_always_active) == 0) return hook_continue;
        if (pressed_[index]) return hook_consumed;
        pressed_[index] = true;
        (void)dispatch_physical(physical_key, modifier_state_, timestamp);
        return hook_consumed;
    }
    if (sink_.input_observed != nullptr) sink_.input_observed(sink_.context, timestamp);
    return hook_continue;
}

LRESULT CALLBACK GlobalHotkeys::keyboard_hook(int code, WPARAM message, LPARAM data) noexcept {
    GlobalHotkeys* owner = callback_owner_;
    if (code < 0 || owner == nullptr || data == 0) return CallNextHookEx(nullptr, code, message, data);
    const auto* input = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    const LRESULT handled = owner->handle_keyboard(message, *input);
    return handled != hook_continue ? handled : CallNextHookEx(owner->hook_, code, message, data);
}

LRESULT CALLBACK GlobalHotkeys::mouse_hook(int code, WPARAM message, LPARAM data) noexcept {
    GlobalHotkeys* owner = callback_owner_;
    if (code < 0 || owner == nullptr || data == 0) return CallNextHookEx(nullptr, code, message, data);
    const auto* input = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    if ((input->flags & LLMHF_INJECTED) == 0 &&
        input->dwExtraInfo != static_cast<ULONG_PTR>(input::injected_event_marker) &&
        owner->sink_.input_observed != nullptr)
        owner->sink_.input_observed(owner->sink_.context, owner->timestamp_ns());
    return CallNextHookEx(owner->mouse_hook_, code, message, data);
}

SaccadeResult GlobalHotkeys::shutdown() noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    const BOOL mouse_removed = mouse_hook_ == nullptr ? TRUE : UnhookWindowsHookEx(mouse_hook_);
    const BOOL keyboard_removed = hook_ == nullptr ? TRUE : UnhookWindowsHookEx(hook_);
    if (mouse_removed != 0) mouse_hook_ = nullptr;
    if (keyboard_removed != 0) hook_ = nullptr;
    if (mouse_removed == 0 || keyboard_removed == 0) {
        ++stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    callback_owner_ = nullptr;
    bindings_.fill({});
    pressed_.fill(false);
    session_pressed_.fill(false);
    binding_count_ = 0;
    modifier_state_ = 0;
    suspended_ = false;
    sink_ = {};
    initialized_ = false;
    ++stats_.unregisters;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

#include "platform/macos/global_hotkeys.hpp"
#include "platform/macos/keyboard.hpp"

#include "application/settings.hpp"

#include <IOKit/hid/IOHIDUsageTables.h>

#include <array>
#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    initialization_failed,
    registration_failed,
    dispatch_failed,
    command_override_failed,
    suspension_failed,
    session_only_failed,
    invalid_binding_failed,
    replacement_failed,
    shutdown_failed,
    language_failed
};

constexpr uint64_t first_timestamp_ns = 101;
constexpr uint64_t second_timestamp_ns = 102;
constexpr uint32_t first_registration_id = 1;
constexpr uint32_t second_registration_id = 2;
constexpr uint32_t session_registration_id = 3;
constexpr uint32_t unknown_registration_id = 4;
constexpr uint32_t binding_modifiers = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                       SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct Capture {
    std::array<saccade::application::CommandEvent, 8> events{};
    uint32_t count = 0;
    uint32_t physical_events = 0;
    uint64_t command_timestamp_ns = 0;
    uint32_t command_inputs = 0;
    bool command_input_pending = false;
    bool ordered = true;
};

void capture_command(void* context, const saccade::application::CommandEvent& event) noexcept {
    auto* capture = static_cast<Capture*>(context);
    capture->ordered = capture->ordered && capture->command_input_pending;
    capture->command_input_pending = false;
    if (capture->count != capture->events.size()) capture->events[capture->count++] = event;
}

void capture_physical_input(void* context, uint64_t) noexcept {
    auto* capture = static_cast<Capture*>(context);
    ++capture->physical_events;
}

void capture_command_input(void* context, uint64_t timestamp_ns) noexcept {
    auto* capture = static_cast<Capture*>(context);
    capture->command_timestamp_ns = timestamp_ns;
    ++capture->command_inputs;
    capture->command_input_pending = true;
}

} // namespace

int main() {
    const saccade::application::SettingsDocument settings = saccade::application::default_settings();
    saccade::application::HintSettings resolved{};
    uint64_t layout_token = 0;
    if (saccade::platform::macos::resolve_hint_language(settings.hints, &resolved, &layout_token) != SACCADE_OK ||
        layout_token == 0 || resolved.alphabet_count != settings.hints.alphabet_count || resolved.alphabet[0] == 0 ||
        resolved.physical_keys != settings.hints.physical_keys) {
        return result(TestResult::language_failed);
    }

    Capture capture{};
    saccade::platform::macos::GlobalHotkeys hotkeys;
    if (hotkeys.initialize({&capture, capture_command, capture_physical_input, nullptr, capture_command_input}) !=
        SACCADE_OK)
        return result(TestResult::initialization_failed);
    const std::array<saccade::application::HotkeyBinding, 3> bindings{
        {{saccade::application::Command::pointer_move, kHIDUsage_KeyboardF11, binding_modifiers, 0},
         {saccade::application::Command::suspend_toggle, kHIDUsage_KeyboardF12, binding_modifiers,
          saccade::application::hotkey_always_active},
         {saccade::application::Command::scope_toggle, kHIDUsage_KeyboardTab, 0,
          saccade::application::hotkey_session_only}}};
    if (hotkeys.replace(bindings.data(), static_cast<uint32_t>(bindings.size())) != SACCADE_OK ||
        hotkeys.binding_count() != bindings.size())
        return result(TestResult::registration_failed);
    if (hotkeys.dispatch_registered_id(first_registration_id, first_timestamp_ns) != SACCADE_OK || capture.count != 1 ||
        capture.events[0].command != saccade::application::Command::pointer_move ||
        capture.events[0].timestamp_ns != first_timestamp_ns)
        return result(TestResult::dispatch_failed);
    if (capture.physical_events != 0 || capture.command_inputs != 1 || !capture.ordered ||
        capture.command_timestamp_ns != first_timestamp_ns)
        return result(TestResult::command_override_failed);
    if (hotkeys.dispatch_physical(bindings[0].physical_key, bindings[0].modifiers, second_timestamp_ns) != SACCADE_OK ||
        capture.count != 2 || capture.command_inputs != 2 || !capture.ordered)
        return result(TestResult::dispatch_failed);
    if (hotkeys.set_suspended(true) != SACCADE_OK ||
        hotkeys.dispatch_registered_id(first_registration_id, second_timestamp_ns) != SACCADE_ERROR_NOT_FOUND ||
        hotkeys.dispatch_physical(bindings[0].physical_key, bindings[0].modifiers, second_timestamp_ns) !=
            SACCADE_ERROR_NOT_FOUND ||
        capture.count != 2 || capture.command_inputs != 2 ||
        hotkeys.dispatch_registered_id(second_registration_id, second_timestamp_ns) != SACCADE_OK ||
        capture.count != 3 || capture.command_inputs != 3 ||
        capture.events[2].command != saccade::application::Command::suspend_toggle ||
        hotkeys.dispatch_registered_id(unknown_registration_id, second_timestamp_ns) != SACCADE_ERROR_NOT_FOUND)
        return result(TestResult::suspension_failed);
    if (hotkeys.dispatch_registered_id(session_registration_id, second_timestamp_ns) != SACCADE_ERROR_NOT_FOUND ||
        hotkeys.dispatch_physical(bindings[2].physical_key, bindings[2].modifiers, second_timestamp_ns) !=
            SACCADE_ERROR_NOT_FOUND ||
        capture.count != 3 || capture.command_inputs != 3) {
        return result(TestResult::session_only_failed);
    }
    if (hotkeys.set_suspended(false) != SACCADE_OK ||
        hotkeys.dispatch_registered_id(first_registration_id, second_timestamp_ns) != SACCADE_OK ||
        capture.count != 4 || capture.command_inputs != 4 ||
        capture.events[3].command != saccade::application::Command::pointer_move ||
        hotkeys.set_suspended(true) != SACCADE_OK) {
        return result(TestResult::suspension_failed);
    }
    std::array<saccade::application::HotkeyBinding, 2> duplicate{bindings[0], bindings[1]};
    duplicate[1].physical_key = duplicate[0].physical_key;
    if (hotkeys.replace(duplicate.data(), static_cast<uint32_t>(duplicate.size())) != SACCADE_ERROR_INVALID_ARGUMENT ||
        hotkeys.binding_count() != bindings.size())
        return result(TestResult::invalid_binding_failed);
    const saccade::application::HotkeyBinding latest{saccade::application::Command::target_position_9,
                                                     kHIDUsage_KeyboardF11, binding_modifiers, 0};
    const std::array latest_bindings{latest, bindings[1]};
    if (hotkeys.replace(latest_bindings.data(), static_cast<uint32_t>(latest_bindings.size())) != SACCADE_OK ||
        hotkeys.binding_count() != latest_bindings.size() ||
        hotkeys.dispatch_registered_id(first_registration_id, second_timestamp_ns) != SACCADE_ERROR_NOT_FOUND ||
        hotkeys.dispatch_registered_id(second_registration_id, second_timestamp_ns) != SACCADE_OK ||
        hotkeys.set_suspended(false) != SACCADE_OK ||
        hotkeys.dispatch_registered_id(first_registration_id, second_timestamp_ns) != SACCADE_OK ||
        capture.count != 6 || capture.events[5].command != saccade::application::Command::target_position_9)
        return result(TestResult::replacement_failed);
    if (hotkeys.replace(nullptr, 0) != SACCADE_OK || hotkeys.binding_count() != 0)
        return result(TestResult::replacement_failed);
    return hotkeys.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

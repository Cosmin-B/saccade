#include "platform/macos/permission_status.hpp"

namespace saccade::platform::macos {
namespace {

PermissionState granted_state(bool granted) noexcept {
    return granted ? PermissionState::allowed : PermissionState::access_required;
}

const char* status_text(PermissionState state) noexcept {
    switch (state) {
    case PermissionState::access_required:
        return "Access required";
    case PermissionState::allowed:
        return "Allowed";
    case PermissionState::relaunch_required:
        return "Allowed — relaunch required";
    }
    return "Unavailable";
}

PermissionRow row(PermissionCapability capability, PermissionState state, const char* title, const char* purpose,
                  const char* privacy) noexcept {
    return {capability, state, title, purpose, privacy, status_text(state)};
}

} // namespace

PermissionPresentation present_permissions(const PermissionSnapshot& snapshot) noexcept {
    PermissionPresentation output{};
    const PermissionState input_state = !snapshot.listen_events          ? PermissionState::access_required
                                        : snapshot.input_monitor_running ? PermissionState::allowed
                                                                         : PermissionState::relaunch_required;
    output.rows = {row(PermissionCapability::screen_recording, granted_state(snapshot.screen_recording), "Screen Recording",
                       "Find visible controls on your displays.", "Frames stay on this Mac and are not recorded or transmitted."),
                   row(PermissionCapability::accessibility, granted_state(snapshot.accessibility), "Accessibility",
                       "Read roles and bounds for visible controls.", "Saccade does not log unrelated accessibility values."),
                   row(PermissionCapability::input_monitoring, input_state, "Input Monitoring",
                       "Know when a button, modifier, drag, or competing physical input is active.",
                       "Raw keystrokes and pointer paths are not logged, retained, or transmitted."),
                   row(PermissionCapability::post_events, granted_state(snapshot.post_events), "Action Readiness",
                       "Send only validated pointer and keyboard actions you request.",
                       "Uses CGEvent, not Apple Events Automation, and cannot enable itself.")};

    if (!snapshot.screen_recording)
        output.pointer_block = PointerActionBlockReason::screen_recording;
    else if (!snapshot.accessibility)
        output.pointer_block = PointerActionBlockReason::accessibility;
    else if (!snapshot.listen_events)
        output.pointer_block = PointerActionBlockReason::input_monitoring;
    else if (!snapshot.post_events)
        output.pointer_block = PointerActionBlockReason::post_events;
    else if (!snapshot.input_monitor_running)
        output.pointer_block = PointerActionBlockReason::monitor_starting;
    else
        output.pointer_block = PointerActionBlockReason::none;
    output.pointer_actions_ready = output.pointer_block == PointerActionBlockReason::none;
    return output;
}

const char* pointer_block_explanation(PointerActionBlockReason reason) noexcept {
    switch (reason) {
    case PointerActionBlockReason::none:
        return "Validated pointer and keyboard actions are ready.";
    case PointerActionBlockReason::screen_recording:
        return "Screen Recording is required to identify visible controls.";
    case PointerActionBlockReason::accessibility:
        return "Accessibility is required to verify the active window and controls.";
    case PointerActionBlockReason::input_monitoring:
        return "Input Monitoring is required to verify that physical input is idle.";
    case PointerActionBlockReason::post_events:
        return "Action access is required to send validated pointer and keyboard events.";
    case PointerActionBlockReason::monitor_starting:
        return "Input Monitoring is allowed, but Saccade must relaunch before actions are ready.";
    }
    return "Action readiness is unavailable.";
}

SystemSettingsPane permission_settings_pane(PermissionCapability capability) noexcept {
    switch (capability) {
    case PermissionCapability::screen_recording:
        return SystemSettingsPane::screen_recording;
    case PermissionCapability::accessibility:
    case PermissionCapability::post_events:
        return SystemSettingsPane::accessibility;
    case PermissionCapability::input_monitoring:
        return SystemSettingsPane::input_monitoring;
    }
    return SystemSettingsPane::accessibility;
}

const char* system_settings_url(SystemSettingsPane pane) noexcept {
    switch (pane) {
    case SystemSettingsPane::screen_recording:
        return "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
    case SystemSettingsPane::accessibility:
        return "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility";
    case SystemSettingsPane::input_monitoring:
        return "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent";
    }
    return nullptr;
}

} // namespace saccade::platform::macos

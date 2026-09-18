#include "platform/macos/permission_status.hpp"

#include <array>
#include <cstring>

using namespace saccade::platform::macos;

namespace {

bool equal(const char* left, const char* right) noexcept {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

} // namespace

int main() {
    PermissionSnapshot snapshot{};
    PermissionPresentation presentation = present_permissions(snapshot);
    if (presentation.rows[0].capability != PermissionCapability::screen_recording ||
        presentation.rows[0].state != PermissionState::access_required ||
        presentation.rows[1].capability != PermissionCapability::accessibility ||
        presentation.rows[2].capability != PermissionCapability::input_monitoring ||
        presentation.rows[3].capability != PermissionCapability::post_events ||
        presentation.pointer_block != PointerActionBlockReason::screen_recording || presentation.pointer_actions_ready)
        return 1;

    snapshot.screen_recording = true;
    presentation = present_permissions(snapshot);
    if (presentation.pointer_block != PointerActionBlockReason::accessibility ||
        !equal(pointer_block_explanation(presentation.pointer_block),
               "Accessibility is required to verify the active window and controls."))
        return 2;

    snapshot.accessibility = true;
    presentation = present_permissions(snapshot);
    if (presentation.pointer_block != PointerActionBlockReason::input_monitoring ||
        !equal(pointer_block_explanation(presentation.pointer_block),
               "Input Monitoring is required to verify that physical input is idle."))
        return 3;

    snapshot.listen_events = true;
    presentation = present_permissions(snapshot);
    if (presentation.rows[2].state != PermissionState::relaunch_required ||
        presentation.pointer_block != PointerActionBlockReason::post_events)
        return 4;

    snapshot.post_events = true;
    presentation = present_permissions(snapshot);
    if (presentation.pointer_block != PointerActionBlockReason::monitor_starting ||
        !equal(pointer_block_explanation(presentation.pointer_block),
               "Input Monitoring is allowed, but Saccade must relaunch before actions are ready."))
        return 5;

    snapshot.input_monitor_running = true;
    presentation = present_permissions(snapshot);
    if (!presentation.pointer_actions_ready || presentation.pointer_block != PointerActionBlockReason::none ||
        !equal(pointer_block_explanation(presentation.pointer_block), "Validated pointer and keyboard actions are ready."))
        return 6;

    constexpr std::array<const char*, 3> expected_urls{"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture",
                                                       "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility",
                                                       "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"};
    for (size_t index = 0; index < expected_urls.size(); ++index) {
        if (!equal(system_settings_url(static_cast<SystemSettingsPane>(index)), expected_urls[index]))
            return static_cast<int>(7 + index);
    }
    if (permission_settings_pane(PermissionCapability::post_events) != SystemSettingsPane::accessibility)
        return 10;

    for (const PermissionRow& row : presentation.rows) {
        if (row.title == nullptr || row.purpose == nullptr || row.privacy == nullptr || row.status == nullptr ||
            std::strstr(row.privacy, "not") == nullptr)
            return 11;
    }
    return 0;
}

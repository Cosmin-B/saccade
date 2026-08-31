#ifndef SACCADE_PLATFORM_MACOS_PERMISSION_STATUS_HPP
#define SACCADE_PLATFORM_MACOS_PERMISSION_STATUS_HPP

#include <array>
#include <cstdint>

namespace saccade::platform::macos {

enum class PermissionCapability : uint32_t { screen_recording = 0, accessibility, input_monitoring, post_events };
enum class PermissionState : uint32_t { access_required = 0, allowed, relaunch_required };
enum class SystemSettingsPane : uint32_t { screen_recording = 0, accessibility, input_monitoring };
enum class PointerActionBlockReason : uint32_t {
    none = 0,
    screen_recording,
    accessibility,
    input_monitoring,
    post_events,
    monitor_starting
};

struct PermissionSnapshot {
    bool screen_recording = false;
    bool accessibility = false;
    bool listen_events = false;
    bool post_events = false;
    bool input_monitor_running = false;
    uint8_t reserved[3]{};
};

struct PermissionRow {
    PermissionCapability capability = PermissionCapability::screen_recording;
    PermissionState state = PermissionState::access_required;
    const char* title = nullptr;
    const char* purpose = nullptr;
    const char* privacy = nullptr;
    const char* status = nullptr;
};

struct PermissionPresentation {
    std::array<PermissionRow, 4> rows{};
    PointerActionBlockReason pointer_block = PointerActionBlockReason::screen_recording;
    bool pointer_actions_ready = false;
    uint8_t reserved[3]{};
};

PermissionPresentation present_permissions(const PermissionSnapshot&) noexcept;
const char* pointer_block_explanation(PointerActionBlockReason) noexcept;
SystemSettingsPane permission_settings_pane(PermissionCapability) noexcept;
const char* system_settings_url(SystemSettingsPane) noexcept;

} // namespace saccade::platform::macos

#endif

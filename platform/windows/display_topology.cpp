#include "platform/windows/display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>
#include <limits>

namespace saccade::platform::windows {
namespace {

struct EnumerationContext {
    std::array<geometry::DisplaySurface, geometry::display_capacity> displays{};
    uint32_t count = 0;
    bool failed = false;
};

bool q8(int value, int32_t* output) noexcept {
    const int64_t scaled = static_cast<int64_t>(value) << geometry::coordinate_fraction_bits;
    if (scaled < INT32_MIN || scaled > INT32_MAX) {
        return false;
    }
    *output = static_cast<int32_t>(scaled);
    return true;
}

bool rect_q8(const RECT& rect, geometry::RectQ8* output) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top && q8(rect.left, &output->x) && q8(rect.top, &output->y) &&
           q8(rect.right - rect.left, &output->width) && q8(rect.bottom - rect.top, &output->height);
}

geometry::QuarterTurn rotation(const DEVMODEW& mode) noexcept {
    switch (mode.dmDisplayOrientation) {
    case DMDO_90:
        return geometry::QuarterTurn::clockwise_90;
    case DMDO_180:
        return geometry::QuarterTurn::clockwise_180;
    case DMDO_270:
        return geometry::QuarterTurn::clockwise_270;
    default:
        return geometry::QuarterTurn::clockwise_0;
    }
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* context = reinterpret_cast<EnumerationContext*>(parameter);
    if (context->count == geometry::display_capacity) {
        context->failed = true;
        return FALSE;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(monitor, &info) == FALSE ||
        EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, EDS_RAWMODE) == FALSE) {
        context->failed = true;
        return FALSE;
    }
    geometry::DisplaySurface surface{};
    surface.display_id = stable_display_id(info.szDevice);
    if (!rect_q8(info.rcMonitor, &surface.desktop_bounds) || !rect_q8(info.rcWork, &surface.work_bounds)) {
        context->failed = true;
        return FALSE;
    }
    const LONG width = info.rcMonitor.right - info.rcMonitor.left;
    const LONG height = info.rcMonitor.bottom - info.rcMonitor.top;
    if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(height) > std::numeric_limits<uint32_t>::max()) {
        context->failed = true;
        return FALSE;
    }
    surface.backing_width = static_cast<uint32_t>(width);
    surface.backing_height = static_cast<uint32_t>(height);
    surface.maximum_fps = mode.dmDisplayFrequency > 1U ? mode.dmDisplayFrequency : 60U;
    surface.rotation = rotation(mode);
    surface.flags = geometry::display_surface_active;
    if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
        surface.flags |= geometry::display_surface_main;
    }
    context->displays[context->count++] = surface;
    return TRUE;
}

bool per_monitor_aware() noexcept {
    const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
    return AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE ||
           AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) != FALSE;
}

} // namespace

uint64_t stable_display_id(const wchar_t* name) noexcept {
    if (name == nullptr) {
        return 0;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const wchar_t* cursor = name; *cursor != L'\0'; ++cursor) {
        const uint16_t value = static_cast<uint16_t>(*cursor);
        hash ^= value & UINT16_C(0x00ff);
        hash *= UINT64_C(1099511628211);
        hash ^= value >> 8U;
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0 ? 1 : hash;
}

SaccadeResult DisplayCollector::refresh(geometry::DisplayCatalog* catalog) noexcept {
    ++stats_.refresh_attempts;
    if (catalog == nullptr) {
        ++stats_.failures;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!per_monitor_aware()) {
        ++stats_.failures;
        return SACCADE_ERROR_STATE;
    }
    EnumerationContext context{};
    if (EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&context)) == FALSE ||
        context.failed || context.count == 0) {
        ++stats_.failures;
        return context.count == geometry::display_capacity ? SACCADE_ERROR_CAPACITY : SACCADE_ERROR_BACKEND;
    }
    const uint64_t previous_epoch = catalog->snapshot().epoch;
    const SaccadeResult result = catalog->publish(context.displays.data(), context.count, 0);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }
    stats_.last_display_count = context.count;
    stats_.topology_changes += catalog->snapshot().epoch != previous_epoch ? 1U : 0U;
    return SACCADE_OK;
}

SaccadeResult DisplayCollector::read_stats(DisplayCollectorStats* output) const noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = stats_;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

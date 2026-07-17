#include "geometry/display_catalog.hpp"
#include "platform/windows/display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

int main() {
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) {
        return 1;
    }
    saccade::geometry::DisplayCatalog catalog;
    saccade::platform::windows::DisplayCollector collector;
    if (collector.refresh(&catalog) != SACCADE_OK || catalog.snapshot().count == 0 || catalog.snapshot().epoch == 0) {
        return 2;
    }
    const uint64_t epoch = catalog.snapshot().epoch;
    if (collector.refresh(&catalog) != SACCADE_OK || catalog.snapshot().epoch != epoch) {
        return 3;
    }
    uint32_t primary_count = 0;
    for (uint32_t index = 0; index < catalog.snapshot().count; ++index) {
        const saccade::geometry::DisplaySurface& display = catalog.snapshot().displays[index];
        primary_count += (display.flags & saccade::geometry::display_surface_main) != 0 ? 1U : 0U;
        saccade::geometry::CoordinateTransform transform;
        if (saccade::geometry::make_desktop_to_surface_transform(display, epoch, &transform) != SACCADE_OK) {
            return 4;
        }
    }
    saccade::platform::windows::DisplayCollectorStats stats{};
    if (primary_count != 1 || collector.read_stats(&stats) != SACCADE_OK || stats.refresh_attempts != 2 ||
        stats.topology_changes != 1 || stats.last_display_count != catalog.snapshot().count) {
        return 5;
    }
    (void)SetThreadDpiAwarenessContext(previous);
    return 0;
}

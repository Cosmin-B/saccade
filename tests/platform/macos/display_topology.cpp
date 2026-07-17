#include "geometry/display_catalog.hpp"
#include "platform/macos/display_topology.hpp"

#import <AppKit/AppKit.h>

#include <cstdint>
#include <thread>

int main() {
    using saccade::geometry::CoordinateTransform;
    using saccade::geometry::DisplayCatalog;
    using saccade::geometry::DisplaySnapshot;
    using saccade::geometry::PointQ8;
    using saccade::platform::macos::DisplayCollector;

    @autoreleasepool {
        [NSApplication sharedApplication];
        if (NSScreen.screens.count == 0) {
            return 77;
        }

        DisplayCatalog catalog;
        DisplayCollector collector;
        if (collector.refresh(nullptr) != SACCADE_ERROR_INVALID_ARGUMENT || collector.refresh(&catalog) != SACCADE_OK) {
            return 1;
        }
        const DisplaySnapshot& snapshot = catalog.snapshot();
        if (snapshot.epoch == 0 || snapshot.count == 0 || snapshot.count > saccade::geometry::display_capacity) {
            return 2;
        }

        uint32_t main_count = 0;
        for (uint32_t index = 0; index < snapshot.count; ++index) {
            const saccade::geometry::DisplaySurface& display = snapshot.displays[index];
            main_count += (display.flags & saccade::geometry::display_surface_main) != 0 ? 1U : 0U;
            if (display.display_id == 0 || display.backing_width == 0 || display.backing_height == 0 ||
                display.maximum_fps == 0 ||
                !saccade::geometry::rect_contains(display.desktop_bounds, display.work_bounds) ||
                display.safe_insets.top < 0 || display.safe_insets.left < 0 || display.safe_insets.bottom < 0 ||
                display.safe_insets.right < 0) {
                return 3;
            }

            CoordinateTransform transform;
            if (saccade::geometry::make_desktop_to_surface_transform(display, snapshot.epoch, &transform) !=
                SACCADE_OK) {
                return 4;
            }
            PointQ8 mapped{};
            const PointQ8 bottom_right{display.desktop_bounds.x + display.desktop_bounds.width,
                                       display.desktop_bounds.y + display.desktop_bounds.height};
            if (transform.map_point(bottom_right, &mapped) != SACCADE_OK ||
                mapped.x !=
                    static_cast<int32_t>(display.backing_width << saccade::geometry::coordinate_fraction_bits) ||
                mapped.y !=
                    static_cast<int32_t>(display.backing_height << saccade::geometry::coordinate_fraction_bits)) {
                return 5;
            }
        }
        if (main_count != 1) {
            return 6;
        }

        const uint64_t first_epoch = snapshot.epoch;
        if (collector.refresh(&catalog) != SACCADE_OK || catalog.snapshot().epoch != first_epoch) {
            return 7;
        }
        saccade::platform::macos::DisplayCollectorStats stats{};
        if (collector.read_stats(nullptr) != SACCADE_ERROR_INVALID_ARGUMENT ||
            collector.read_stats(&stats) != SACCADE_OK) {
            return 8;
        }
        if (stats.refresh_attempts != 3 || stats.topology_changes != 1 || stats.failures != 1 ||
            stats.last_display_count != snapshot.count) {
            return 9;
        }

        SaccadeResult worker_result = SACCADE_OK;
        SaccadeResult worker_stats_result = SACCADE_OK;
        std::thread worker([&]() noexcept {
            saccade::platform::macos::DisplayCollectorStats worker_stats{};
            worker_result = collector.refresh(&catalog);
            worker_stats_result = collector.read_stats(&worker_stats);
        });
        worker.join();
        if (worker_result != SACCADE_ERROR_STATE || worker_stats_result != SACCADE_ERROR_STATE) {
            return 10;
        }
    }
    return 0;
}

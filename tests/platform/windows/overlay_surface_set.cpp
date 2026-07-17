#include "platform/windows/overlay_surface.hpp"
#include "backends/d3d12/graphics_device.hpp"

#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace {

using Microsoft::WRL::ComPtr;

SaccadeResult no_frame(void*, uint64_t, SaccadeOverlayFrameDesc*) noexcept {
    return SACCADE_ERROR_NOT_FOUND;
}

saccade::geometry::DisplaySurface display(uint64_t id, int32_t x, uint32_t width) noexcept {
    saccade::geometry::DisplaySurface value{};
    value.display_id = id;
    value.desktop_bounds = {x * 256, 0, static_cast<int32_t>(width * 256U), 256 * 256};
    value.work_bounds = value.desktop_bounds;
    value.backing_width = width;
    value.backing_height = 256;
    value.maximum_fps = 120;
    value.flags = saccade::geometry::display_surface_active | (id == 1 ? saccade::geometry::display_surface_main : 0U);
    return value;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) {
        return 2;
    }
    saccade::backend::d3d12::GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return 77;
    }
    static saccade::platform::windows::OverlaySurfaceSet surfaces;
    if (surfaces.initialize(graphics.device(), graphics.queue(), argv[1], {nullptr, no_frame, nullptr}) != SACCADE_OK) {
        return 3;
    }
    saccade::geometry::DisplaySnapshot snapshot{};
    snapshot.epoch = 1;
    snapshot.count = 2;
    snapshot.displays[0] = display(1, 0, 256);
    snapshot.displays[1] = display(2, 256, 256);
    if (surfaces.synchronize(snapshot) != SACCADE_OK || surfaces.start() != SACCADE_OK) {
        return 4;
    }
    saccade::platform::windows::OverlaySurfaceInfo first{};
    saccade::platform::windows::OverlaySurfaceInfo second{};
    if (surfaces.read_surface_info(1, &first) != SACCADE_OK || surfaces.read_surface_info(2, &second) != SACCADE_OK ||
        first.window_handle == second.window_handle || first.frame_latency_handle == second.frame_latency_handle ||
        (first.flags & saccade::platform::windows::overlay_surface_visible) == 0 ||
        surfaces.set_click_through(false) != SACCADE_OK) {
        return 5;
    }
    snapshot.epoch = 2;
    snapshot.count = 1;
    snapshot.displays[0] = display(1, 0, 320);
    if (surfaces.synchronize(snapshot) != SACCADE_OK || surfaces.synchronize(snapshot) != SACCADE_OK ||
        surfaces.read_surface_info(2, &second) != SACCADE_ERROR_NOT_FOUND ||
        surfaces.read_surface_info(1, &first) != SACCADE_OK || first.drawable_width != 320) {
        return 6;
    }
    saccade::platform::windows::OverlaySurfaceSetStats stats{};
    if (surfaces.read_stats(&stats) != SACCADE_OK || stats.synchronize_attempts != 3 || stats.topology_changes != 2 ||
        stats.surfaces_added != 2 || stats.surfaces_removed != 1 || stats.surfaces_updated != 1 ||
        stats.active_surfaces != 1 || stats.running != 1 || surfaces.stop() != SACCADE_OK ||
        surfaces.shutdown() != SACCADE_OK ||
        surfaces.initialize(graphics.device(), graphics.queue(), argv[1], {nullptr, no_frame, nullptr}) != SACCADE_OK ||
        surfaces.synchronize(snapshot) != SACCADE_OK || surfaces.start() != SACCADE_OK ||
        surfaces.stop() != SACCADE_OK || surfaces.shutdown() != SACCADE_OK) {
        return 7;
    }
    (void)SetThreadDpiAwarenessContext(previous);
    return 0;
}

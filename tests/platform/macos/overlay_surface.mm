#include "platform/macos/display_topology.hpp"
#include "platform/macos/overlay_surface.hpp"

#import <AppKit/AppKit.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

alignas(64) std::array<uint8_t, sizeof(SaccadeOverlayPacketHeader)> packet{};

SaccadeResult load_frame(void*, uint64_t display_id, SaccadeOverlayFrameDesc* output) noexcept {
    if (display_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = 1;
    header.transform_epoch = 1;
    std::memcpy(packet.data(), &header, sizeof(header));

    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = header.scene_epoch;
    frame.transform_epoch = header.transform_epoch;
    frame.packet = {packet.data(), packet.size()};
    *output = frame;
    return SACCADE_OK;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    using namespace saccade::platform::macos;

    @autoreleasepool {
        [NSApplication sharedApplication];
        if (NSScreen.screens.count == 0) {
            return 77;
        }
        saccade::geometry::DisplayCatalog catalog;
        DisplayCollector collector;
        if (collector.refresh(&catalog) != SACCADE_OK) {
            return 2;
        }
        const auto& snapshot = catalog.snapshot();
        const saccade::geometry::DisplaySurface* display = nullptr;
        for (uint32_t index = 0; index < snapshot.count; ++index) {
            if ((snapshot.displays[index].flags & saccade::geometry::display_surface_main) != 0) {
                display = &snapshot.displays[index];
                break;
            }
        }
        if (display == nullptr) {
            return 3;
        }

        OverlaySurface surface;
        OverlaySurfaceInfo info{};
        if (surface.read_info(&info) != SACCADE_ERROR_INVALID_ARGUMENT) {
            return 4;
        }
        SaccadeResult worker_initialize = SACCADE_OK;
        std::thread worker([&]() noexcept {
            worker_initialize = surface.initialize(
                *display, argv[1], saccade::backend::metal::PathPreference::automatic, {nullptr, load_frame, nullptr});
        });
        worker.join();
        if (worker_initialize != SACCADE_ERROR_STATE) {
            return 5;
        }
        const SaccadeResult initialized = surface.initialize(
            *display, argv[1], saccade::backend::metal::PathPreference::automatic, {nullptr, load_frame, nullptr});
        if (initialized == SACCADE_ERROR_UNSUPPORTED) {
            return 77;
        }
        if (initialized != SACCADE_OK ||
            surface.initialize(*display, argv[1], saccade::backend::metal::PathPreference::automatic,
                               {nullptr, load_frame, nullptr}) != SACCADE_ERROR_STATE) {
            return 6;
        }
        if (surface.read_info(&info) != SACCADE_OK || info.display_id != display->display_id ||
            info.drawable_width != display->backing_width || info.drawable_height != display->backing_height ||
            info.preferred_fps != std::min(display->maximum_fps, 120U) || info.maximum_drawable_count != 3 ||
            info.window_level != NSStatusWindowLevel ||
            (info.flags & (overlay_surface_initialized | overlay_surface_paused | overlay_surface_click_through |
                           overlay_surface_nonactivating | overlay_surface_all_spaces | overlay_surface_color_managed |
                           overlay_surface_display_paced)) !=
                (overlay_surface_initialized | overlay_surface_paused | overlay_surface_click_through |
                 overlay_surface_nonactivating | overlay_surface_all_spaces | overlay_surface_color_managed |
                 overlay_surface_display_paced) ||
            (info.flags & overlay_surface_visible) != 0) {
            return 7;
        }

        if (surface.set_click_through(false) != SACCADE_OK || surface.read_info(&info) != SACCADE_OK ||
            (info.flags & overlay_surface_click_through) != 0 || surface.set_click_through(true) != SACCADE_OK ||
            surface.update_display(*display) != SACCADE_OK) {
            return 8;
        }
        auto wrong_display = *display;
        ++wrong_display.display_id;
        if (surface.update_display(wrong_display) != SACCADE_ERROR_INVALID_ARGUMENT) {
            return 9;
        }

        SaccadeResult worker_start = SACCADE_OK;
        std::thread second_worker([&]() noexcept { worker_start = surface.start(); });
        second_worker.join();
        if (worker_start != SACCADE_ERROR_STATE || surface.start() != SACCADE_OK) {
            return 10;
        }
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
        if (surface.stop() != SACCADE_OK || surface.read_info(&info) != SACCADE_OK ||
            (info.flags & overlay_surface_visible) != 0 || (info.flags & overlay_surface_paused) == 0) {
            return 10;
        }

        OverlaySurfaceStats stats{};
        OverlaySurfaceMemoryStats memory{};
        saccade::backend::metal::Stats renderer_stats{};
        if (surface.read_stats(&stats) != SACCADE_OK || surface.read_renderer_stats(&renderer_stats) != SACCADE_OK ||
            stats.display_ticks == 0 || stats.rendered_frames == 0 || stats.rendered_frames > stats.display_ticks ||
            renderer_stats.slot_count != 3 || renderer_stats.presented_frames != stats.rendered_frames) {
            return 11;
        }
        if (surface.read_memory_stats(&memory) != SACCADE_OK || memory.drawable_width != display->backing_width ||
            memory.drawable_height != display->backing_height || memory.drawable_count != 3 ||
            memory.surface_host_bytes != sizeof(OverlaySurface) ||
            memory.drawable_bytes_estimate !=
                static_cast<uint64_t>(display->backing_width) * display->backing_height * 4U * 3U ||
            memory.total_known_and_estimated < memory.drawable_bytes_estimate) {
            return 21;
        }

        OverlaySurface metal3_surface;
        if (metal3_surface.initialize(*display, argv[1], saccade::backend::metal::PathPreference::metal3,
                                      {nullptr, load_frame, nullptr}) != SACCADE_OK ||
            metal3_surface.start() != SACCADE_OK) {
            return 12;
        }
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        if (metal3_surface.stop() != SACCADE_OK || metal3_surface.read_stats(&stats) != SACCADE_OK ||
            metal3_surface.read_renderer_stats(&renderer_stats) != SACCADE_OK || stats.rendered_frames == 0 ||
            renderer_stats.path != saccade::backend::metal::Path::metal3 ||
            renderer_stats.presented_frames != stats.rendered_frames) {
            return 13;
        }

        OverlaySurfaceSet surfaces;
        if (surfaces.initialize(argv[1], saccade::backend::metal::PathPreference::automatic,
                                {nullptr, load_frame, nullptr}) != SACCADE_OK ||
            surfaces.synchronize(snapshot) != SACCADE_OK) {
            return 14;
        }
        OverlaySurfaceSetStats set_stats{};
        if (surfaces.read_stats(&set_stats) != SACCADE_OK || set_stats.active_surfaces != snapshot.count ||
            set_stats.surfaces_added != snapshot.count || set_stats.topology_epoch != snapshot.epoch ||
            surfaces.read_surface_info(display->display_id, &info) != SACCADE_OK ||
            surfaces.synchronize(snapshot) != SACCADE_OK) {
            return 15;
        }
        saccade::geometry::DisplaySnapshot duplicate{};
        duplicate.epoch = snapshot.epoch + 1U;
        duplicate.count = 2;
        duplicate.flags = snapshot.flags;
        duplicate.displays[0] = *display;
        duplicate.displays[1] = *display;
        auto missing = duplicate;
        missing.count = 1;
        missing.displays[0].display_id = UINT64_C(0x100000000);
        if (surfaces.synchronize(duplicate) != SACCADE_ERROR_INVALID_ARGUMENT ||
            surfaces.synchronize(missing) != SACCADE_ERROR_NOT_FOUND || surfaces.read_stats(&set_stats) != SACCADE_OK ||
            set_stats.active_surfaces != snapshot.count || set_stats.topology_epoch != snapshot.epoch) {
            return 22;
        }
        if (surfaces.set_click_through(false) != SACCADE_OK ||
            surfaces.read_surface_info(display->display_id, &info) != SACCADE_OK ||
            (info.flags & overlay_surface_click_through) != 0 || surfaces.set_click_through(true) != SACCADE_OK ||
            surfaces.start() != SACCADE_OK) {
            return 16;
        }
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        if (surfaces.stop() != SACCADE_OK || surfaces.read_stats(&set_stats) != SACCADE_OK ||
            surfaces.read_surface_stats(display->display_id, &stats) != SACCADE_OK ||
            surfaces.read_surface_memory_stats(display->display_id, &memory) != SACCADE_OK ||
            surfaces.read_surface_renderer_stats(display->display_id, &renderer_stats) != SACCADE_OK ||
            set_stats.running != 0 || stats.rendered_frames == 0 || memory.drawable_count != 3 ||
            renderer_stats.presented_frames == 0) {
            return 17;
        }

        if (snapshot.count > 1) {
            saccade::geometry::DisplaySnapshot subset{};
            subset.epoch = snapshot.epoch + 1U;
            subset.count = 1;
            subset.flags = snapshot.flags;
            subset.displays[0] = *display;
            if (surfaces.synchronize(subset) != SACCADE_OK || surfaces.read_stats(&set_stats) != SACCADE_OK ||
                set_stats.active_surfaces != 1 || set_stats.surfaces_removed != snapshot.count - 1U) {
                return 18;
            }
            saccade::geometry::DisplaySnapshot restored = snapshot;
            restored.epoch = subset.epoch + 1U;
            if (surfaces.synchronize(restored) != SACCADE_OK || surfaces.read_stats(&set_stats) != SACCADE_OK ||
                set_stats.active_surfaces != snapshot.count) {
                return 19;
            }
        }

        SaccadeResult worker_sync = SACCADE_OK;
        std::thread third_worker([&]() noexcept { worker_sync = surfaces.synchronize(snapshot); });
        third_worker.join();
        if (worker_sync != SACCADE_ERROR_STATE) {
            return 20;
        }
    }
    return 0;
}

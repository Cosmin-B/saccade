#include "platform/macos/display_topology.hpp"
#include "platform/macos/coreml_image_bridge.hpp"
#include "platform/macos/scene_capture.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <thread>
#include <time.h>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) {
            return 64;
        }
        if (!CGPreflightScreenCaptureAccess()) {
            return 77;
        }
        [NSApplication sharedApplication];
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return 77;
        }

        saccade::geometry::DisplayCatalog catalog;
        saccade::platform::macos::DisplayCollector collector;
        if (collector.refresh(&catalog) != SACCADE_OK || catalog.snapshot().count == 0) {
            return 1;
        }
        const saccade::geometry::DisplaySnapshot original = catalog.snapshot();

        saccade::platform::macos::ScreenCaptureProvider provider;
        saccade::platform::macos::SceneCaptureSet uninitialized_captures;
        if (uninitialized_captures.initialize(&provider, 1536, 1024) != SACCADE_ERROR_STATE) {
            return 2;
        }
        if (provider.initialize((__bridge void*)device) != SACCADE_OK) {
            return 10;
        }
        saccade::platform::macos::SceneCaptureSet captures;
        if (captures.initialize(&provider, 1536, 0) != SACCADE_ERROR_INVALID_ARGUMENT ||
            captures.initialize(&provider, 1536, 1024) != SACCADE_OK ||
            captures.initialize(&provider, 1536, 1024) != SACCADE_ERROR_ALREADY_EXISTS ||
            captures.synchronize(original) != SACCADE_OK) {
            return 3;
        }

        saccade::platform::macos::SceneCaptureStats stats{};
        if (captures.read_stats(&stats) != SACCADE_OK || stats.topology_epoch != original.epoch ||
            stats.active_streams != original.count || stats.streams_added != original.count ||
            stats.leased_frames != 0 || captures.synchronize(original) != SACCADE_OK) {
            return 4;
        }
        SaccadeResult foreign_result = SACCADE_OK;
        std::thread foreign([&]() noexcept {
            saccade::platform::macos::SceneCaptureStats foreign_stats{};
            foreign_result = captures.read_stats(&foreign_stats);
        });
        foreign.join();
        if (foreign_result != SACCADE_ERROR_STATE) {
            return 5;
        }

        uint64_t display_id = 0;
        if (captures.display_at(0, &display_id) != SACCADE_OK || display_id == 0 ||
            captures.display_at(original.count, &display_id) != SACCADE_ERROR_NOT_FOUND) {
            return 6;
        }
        saccade::platform::macos::SceneCaptureFrame frame{};
        if (captures.acquire(display_id, &frame) != SACCADE_ERROR_STATE || captures.set_running(true) != SACCADE_OK) {
            return 6;
        }
        SaccadeResult acquired = SACCADE_ERROR_BUSY;
        const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + UINT64_C(3'000'000'000);
        const timespec pause{0, 1'000'000};
        while (acquired == SACCADE_ERROR_BUSY && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline) {
            acquired = captures.acquire(display_id, &frame);
            if (acquired == SACCADE_ERROR_BUSY) {
                (void)nanosleep(&pause, nullptr);
            }
        }
        if (acquired != SACCADE_OK || frame.display_id != display_id || frame.topology_epoch != original.epoch ||
            frame.frame.frame == 0 || frame.native.metal_texture == nullptr || frame.native.iosurface == nullptr ||
            frame.native.width != frame.frame.width || frame.native.height != frame.frame.height ||
            captures.acquire(display_id, &frame) != SACCADE_ERROR_BUSY) {
            return 7;
        }

        auto changed = original;
        changed.epoch = original.epoch + 1U;
        for (uint32_t index = 0; index < changed.count; ++index) {
            if (changed.displays[index].display_id == display_id) {
                ++changed.displays[index].backing_width;
                break;
            }
        }
        if (captures.synchronize(changed) != SACCADE_ERROR_BUSY || captures.release(frame) != SACCADE_OK ||
            captures.release(frame) != SACCADE_ERROR_STALE_HANDLE || captures.synchronize(changed) != SACCADE_OK) {
            return 8;
        }

        saccade::geometry::DisplaySnapshot empty{};
        empty.epoch = changed.epoch + 1U;
        empty.flags = original.flags;
        if (captures.synchronize(empty) != SACCADE_OK || captures.read_stats(&stats) != SACCADE_OK ||
            stats.active_streams != 0 || stats.streams_removed != static_cast<uint64_t>(original.count) + 1U) {
            return 8;
        }

        auto restored = original;
        restored.epoch = empty.epoch + 1U;
        if (captures.synchronize(restored) != SACCADE_OK || captures.read_stats(&stats) != SACCADE_OK ||
            stats.topology_epoch != restored.epoch || stats.active_streams != restored.count ||
            stats.streams_added != static_cast<uint64_t>(restored.count) * 2U + 1U || stats.frames_acquired != 1 ||
            stats.frames_released != 1 || stats.failures != 1) {
            return 9;
        }

        frame = {};
        acquired = SACCADE_ERROR_BUSY;
        const uint64_t bridge_deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + UINT64_C(3'000'000'000);
        while (acquired == SACCADE_ERROR_BUSY && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < bridge_deadline) {
            acquired = captures.acquire(display_id, &frame);
            if (acquired == SACCADE_ERROR_BUSY) {
                (void)nanosleep(&pause, nullptr);
            }
        }
        const saccade::geometry::DisplaySurface* display = nullptr;
        for (uint32_t index = 0; index < restored.count; ++index) {
            if (restored.displays[index].display_id == display_id) {
                display = &restored.displays[index];
                break;
            }
        }
        SaccadeRuntimeDesc runtime_desc{};
        runtime_desc.struct_size = sizeof(runtime_desc);
        runtime_desc.api_version = SACCADE_API_VERSION;
        SaccadeRuntimeHandle runtime = 0;
        if (acquired != SACCADE_OK || display == nullptr ||
            saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK) {
            return 11;
        }
        saccade::platform::macos::CoreMlImageBridge bridge;
        const saccade::platform::macos::CoreMlImageBridgeConfig bridge_config{
            runtime, (__bridge void*)device, argv[1], saccade::backend::metal::PathPreference::automatic, 320, 320, 0};
        if (bridge.initialize(bridge_config) != SACCADE_OK || bridge.begin(&captures, frame, *display) != SACCADE_OK) {
            return 12;
        }
        saccade::scheduler::NeuralFrame neural{};
        bool ready = false;
        while (!ready && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < bridge_deadline) {
            if (bridge.poll(&neural, &ready) != SACCADE_OK) {
                return 13;
            }
            if (!ready) (void)nanosleep(&pause, nullptr);
        }
        const auto& transform = neural.source_to_desktop.descriptor();
        if (!ready || neural.frame == 0 || neural.width != 320 || neural.height != 320 ||
            neural.source_id != frame.frame.source_id || neural.topology_epoch != frame.topology_epoch ||
            neural.transform_epoch != frame.frame.transform_epoch ||
            neural.capture_time_ns != frame.frame.timestamp_ns || transform.source.x < 0 || transform.source.y < 0 ||
            transform.source.width <= 0 || transform.source.height <= 0 ||
            transform.destination.x != display->desktop_bounds.x ||
            transform.destination.y != display->desktop_bounds.y ||
            transform.destination.width != display->desktop_bounds.width ||
            transform.destination.height != display->desktop_bounds.height || !bridge.busy() ||
            bridge.shutdown() != SACCADE_ERROR_BUSY) {
            return 14;
        }
        if (saccade_frame_release(runtime, neural.frame) != SACCADE_OK) {
            return 15;
        }
        neural.retire(neural.retire_context, neural.frame);
        neural = {};
        ready = false;
        if (bridge.begin_cached(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) != SACCADE_OK ||
            bridge.poll(&neural, &ready) != SACCADE_OK || !ready || neural.frame == 0 ||
            saccade_frame_release(runtime, neural.frame) != SACCADE_OK) {
            return 16;
        }
        neural.retire(neural.retire_context, neural.frame);
        const auto bridge_stats = bridge.stats();
        if (bridge.busy() || bridge_stats.submissions != 2 || bridge_stats.completions != 2 ||
            bridge_stats.runtime_imports != 2 || bridge_stats.capture_releases != 1 ||
            bridge_stats.output_retires != 2 || bridge_stats.cached_replays != 1 || bridge_stats.failures != 0 ||
            bridge.shutdown() != SACCADE_OK || saccade_runtime_destroy(runtime) != SACCADE_OK ||
            captures.shutdown() != SACCADE_OK) {
            return 17;
        }
    }
    return 0;
}

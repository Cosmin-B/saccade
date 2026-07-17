#include "platform/windows/display_topology.hpp"
#include "platform/windows/neural_bridge.hpp"
#include "platform/windows/scene_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <thread>

namespace {

enum class TestResult : int {
    success,
    unavailable = 77,
    topology_failed = 2,
    provider_failed,
    initialization_failed,
    synchronization_failed,
    ownership_failed,
    acquire_failed,
    bridge_failed,
    removal_failed,
    restoration_failed,
    shutdown_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) return result(TestResult::unavailable);
    saccade::geometry::DisplayCatalog catalog;
    saccade::platform::windows::DisplayCollector collector;
    if (collector.refresh(&catalog) != SACCADE_OK || catalog.snapshot().count == 0) {
        return result(TestResult::topology_failed);
    }
    const saccade::geometry::DisplaySnapshot original = catalog.snapshot();
    static saccade::platform::windows::ScreenCaptureProvider provider;
    if (provider.initialize() != SACCADE_OK) {
        return result(TestResult::provider_failed);
    }
    saccade::platform::windows::SceneCaptureSet captures;
    if (captures.initialize(&provider, 1536, 0) != SACCADE_ERROR_INVALID_ARGUMENT ||
        captures.initialize(&provider, 1536, 1024) != SACCADE_ERROR_UNSUPPORTED ||
        captures.initialize(&provider, 0, 0) != SACCADE_OK || captures.synchronize(original) != SACCADE_OK) {
        return result(TestResult::initialization_failed);
    }
    saccade::platform::windows::SceneCaptureStats stats{};
    if (captures.read_stats(&stats) != SACCADE_OK || stats.topology_epoch != original.epoch ||
        stats.active_streams != original.count || stats.streams_added != original.count ||
        captures.synchronize(original) != SACCADE_OK) {
        return result(TestResult::synchronization_failed);
    }
    SaccadeResult foreign_result = SACCADE_OK;
    std::thread foreign([&]() noexcept {
        saccade::platform::windows::SceneCaptureStats foreign_stats{};
        foreign_result = captures.read_stats(&foreign_stats);
    });
    foreign.join();
    if (foreign_result != SACCADE_ERROR_STATE) {
        return result(TestResult::ownership_failed);
    }
    uint64_t display_id = 0;
    if (captures.display_at(0, &display_id) != SACCADE_OK || display_id == 0 ||
        captures.display_at(original.count, &display_id) != SACCADE_ERROR_NOT_FOUND) {
        return result(TestResult::synchronization_failed);
    }
    saccade::platform::windows::SceneCaptureFrame frame{};
    if (captures.acquire(display_id, &frame) != SACCADE_ERROR_STATE || captures.set_running(true) != SACCADE_OK)
        return result(TestResult::synchronization_failed);
    SaccadeResult acquired = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < 300 && acquired == SACCADE_ERROR_BUSY; ++attempt) {
        Sleep(10);
        acquired = captures.acquire(display_id, &frame);
    }
    if (acquired != SACCADE_OK || frame.display_id != display_id || frame.topology_epoch != original.epoch ||
        frame.frame.frame == 0 || frame.native.d3d11_texture == nullptr || frame.native.width < frame.frame.width ||
        frame.native.height < frame.frame.height || captures.acquire(display_id, &frame) != SACCADE_ERROR_BUSY) {
        return result(TestResult::acquire_failed);
    }
    saccade::geometry::DisplaySnapshot empty{};
    empty.epoch = original.epoch + 1U;
    empty.flags = original.flags;
    const saccade::geometry::DisplaySurface* display = nullptr;
    for (uint32_t index = 0; index < original.count; ++index) {
        if (original.displays[index].display_id == display_id) {
            display = &original.displays[index];
            break;
        }
    }
    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    saccade::platform::windows::NeuralBridge bridge;
    saccade::scheduler::DesktopNeuralFrame neural{};
    if (captures.synchronize(empty) != SACCADE_ERROR_BUSY || display == nullptr ||
        saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK || bridge.initialize(runtime) != SACCADE_OK ||
        bridge.import(&captures, frame, *display, original.epoch, &neural) != SACCADE_OK || neural.frame == 0 ||
        neural.source_id != frame.frame.source_id || neural.scene_transform_epoch != original.epoch ||
        neural.topology_epoch != original.epoch || neural.capture_time_ns != frame.frame.timestamp_ns) {
        return result(TestResult::bridge_failed);
    }
    saccade::platform::windows::SceneCaptureFrame blocked{};
    if (captures.acquire(display_id, &blocked) != SACCADE_ERROR_BUSY || neural.retire == nullptr ||
        saccade_frame_release(runtime, neural.frame) != SACCADE_OK) {
        return result(TestResult::removal_failed);
    }
    neural.retire(neural.retire_context, neural.frame);
    if (saccade_runtime_destroy(runtime) != SACCADE_OK || captures.synchronize(empty) != SACCADE_OK ||
        captures.read_stats(&stats) != SACCADE_OK || stats.active_streams != 0 ||
        stats.streams_removed != original.count) {
        return result(TestResult::removal_failed);
    }
    auto restored = original;
    restored.epoch = empty.epoch + 1U;
    if (captures.synchronize(restored) != SACCADE_OK || captures.read_stats(&stats) != SACCADE_OK ||
        stats.topology_epoch != restored.epoch || stats.active_streams != restored.count ||
        stats.streams_added != static_cast<uint64_t>(restored.count) * 2U || stats.frames_acquired != 1 ||
        stats.frames_released != 1 || stats.failures != 1 || bridge.stats().imports != 1 ||
        bridge.stats().capture_releases != 1 || bridge.stats().failures != 0) {
        return result(TestResult::restoration_failed);
    }
    if (captures.shutdown() != SACCADE_OK) return result(TestResult::shutdown_failed);
    (void)SetThreadDpiAwarenessContext(previous);
    return result(TestResult::success);
}

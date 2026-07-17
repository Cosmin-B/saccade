#include "platform/macos/screen_capture.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <array>
#include <cstdint>

namespace {

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = static_cast<uint32_t>(sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    return value;
}

} // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return 77;
        }

        saccade::platform::macos::ScreenCaptureProvider provider;
        saccade::platform::macos::ScreenCaptureStats uninitialized_stats{};
        saccade::platform::macos::NativeCapturedFrame uninitialized_frame{};
        if (provider.descriptor().context != nullptr ||
            provider.read_stats(1, &uninitialized_stats) != SACCADE_ERROR_STATE ||
            provider.read_native_frame(1, 1, &uninitialized_frame) != SACCADE_ERROR_STATE) {
            return 1;
        }
        if (provider.initialize((__bridge void*)device) != SACCADE_OK ||
            provider.initialize((__bridge void*)device) != SACCADE_ERROR_ALREADY_EXISTS) {
            return 25;
        }
        SaccadeCaptureProviderDesc backend = provider.descriptor();
        if (backend.context == nullptr || backend.ops.enumerate_sources == nullptr || backend.ops.create == nullptr ||
            backend.ops.start == nullptr || backend.ops.acquire == nullptr || backend.ops.copy_damage == nullptr ||
            backend.ops.release == nullptr || backend.ops.stop == nullptr || backend.ops.destroy == nullptr ||
            backend.ops.memory_stats == nullptr) {
            return 2;
        }

        if (!CGPreflightScreenCaptureAccess()) {
            return 77;
        }

        SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
        SaccadeResult result = backend.ops.enumerate_sources(backend.context, 0, &source);
        if (result == SACCADE_ERROR_BACKEND) {
            return 77;
        }
        if (result != SACCADE_OK || source.stable_id == 0 || source.kind != SACCADE_CAPTURE_SOURCE_DISPLAY ||
            source.name.data == nullptr || source.name.size == 0) {
            return 3;
        }

        SaccadeCaptureStreamDesc stream_desc{};
        stream_desc.struct_size = static_cast<uint32_t>(sizeof(stream_desc));
        stream_desc.api_version = SACCADE_API_VERSION;
        stream_desc.source_id = source.stable_id;
        stream_desc.pixel_format = SACCADE_FORMAT_BGRA8;
        stream_desc.queue_capacity = 3;
        stream_desc.max_width = 0;
        stream_desc.max_height = 0;

        SaccadeCaptureStreamHandle stream = 0;
        if (backend.ops.create(backend.context, &stream_desc, &stream) != SACCADE_OK || stream == 0 ||
            backend.ops.start(backend.context, stream) != SACCADE_OK) {
            return 4;
        }

        SaccadeCapturedFrame frame = output_structure<SaccadeCapturedFrame>();
        result = backend.ops.acquire(backend.context, stream, UINT64_C(3'000'000'000), &frame);
        if (result != SACCADE_OK || frame.frame == 0 || frame.source_id != source.stable_id || frame.width == 0 ||
            frame.height == 0 || frame.pixel_format != SACCADE_FORMAT_BGRA8 || frame.damage_count > 64) {
            (void)backend.ops.stop(backend.context, stream);
            (void)backend.ops.destroy(backend.context, stream);
            return 5;
        }

        saccade::platform::macos::NativeCapturedFrame native{};
        if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK || native.pixel_buffer == nullptr ||
            native.iosurface == nullptr || native.metal_texture == nullptr || native.iosurface_id == 0 ||
            native.width != frame.width || native.height != frame.height) {
            return 6;
        }

        std::array<SaccadeRectI32, 64> damage{};
        uint32_t required = 0;
        if (backend.ops.copy_damage(backend.context, stream, frame.frame, damage.data(), damage.size(), &required) !=
                SACCADE_OK ||
            required != frame.damage_count) {
            return 7;
        }

        SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
        if (backend.ops.memory_stats(backend.context, stream, &memory) != SACCADE_OK || memory.device_imported == 0 ||
            memory.copied_bytes != 0 || memory.high_water_bytes < memory.device_imported) {
            return 8;
        }
        if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK ||
            backend.ops.release(backend.context, stream, frame.frame) != SACCADE_ERROR_STALE_HANDLE ||
            provider.read_native_frame(stream, frame.frame, &native) != SACCADE_ERROR_STALE_HANDLE) {
            return 9;
        }
        SaccadeCapturedFrame stopped_frame = output_structure<SaccadeCapturedFrame>();
        if (backend.ops.stop(backend.context, stream) != SACCADE_OK ||
            backend.ops.acquire(backend.context, stream, 0, &stopped_frame) != SACCADE_ERROR_STATE ||
            backend.ops.synchronize(backend.context, stream, UINT64_C(1'000'000'000)) != SACCADE_OK) {
            return 10;
        }

        saccade::platform::macos::ScreenCaptureStats stats{};
        if (provider.read_stats(stream, &stats) != SACCADE_OK || stats.callbacks == 0 || stats.published == 0 ||
            stats.acquired != 1 || stats.released != 1 || stats.copied_bytes != 0 || stats.imported_bytes != 0 ||
            stats.imported_high_water == 0 || stats.latest_callback_sequence < stats.callbacks ||
            stats.latest_status_sequence == 0 || stats.latest_status_sequence > stats.latest_callback_sequence ||
            stats.did_stop_with_error != 0) {
            return 11;
        }
        if (backend.ops.destroy(backend.context, stream) != SACCADE_OK ||
            backend.ops.destroy(backend.context, stream) != SACCADE_ERROR_STALE_HANDLE) {
            return 12;
        }

        const uint32_t native_width = frame.width;
        const uint32_t native_height = frame.height;
        const uint64_t native_bytes = memory.high_water_bytes;
        stream_desc.max_width = 1536;
        stream_desc.max_height = 1024;
        stream = 0;
        if (backend.ops.create(backend.context, &stream_desc, &stream) != SACCADE_OK ||
            backend.ops.start(backend.context, stream) != SACCADE_OK) {
            return 13;
        }
        frame = output_structure<SaccadeCapturedFrame>();
        if (backend.ops.acquire(backend.context, stream, UINT64_C(3'000'000'000), &frame) != SACCADE_OK ||
            frame.width > stream_desc.max_width || frame.height > stream_desc.max_height ||
            frame.width >= native_width || frame.height >= native_height) {
            return 14;
        }
        memory = output_structure<SaccadeMemoryStats>();
        if (backend.ops.memory_stats(backend.context, stream, &memory) != SACCADE_OK || memory.device_imported == 0 ||
            memory.device_imported >= native_bytes || memory.copied_bytes != 0 ||
            provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK || native.metal_texture == nullptr) {
            return 15;
        }
        if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK ||
            backend.ops.stop(backend.context, stream) != SACCADE_OK ||
            backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
            return 16;
        }

        stream_desc.source_id = source.stable_id;
        stream_desc.max_width = 512;
        stream_desc.max_height = 512;
        stream = 0;
        if (backend.ops.create(backend.context, &stream_desc, &stream) != SACCADE_OK) {
            return 17;
        }
        std::array<SaccadeFrameHandle, 3> held{};
        for (SaccadeFrameHandle& held_frame : held) {
            if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
                return 18;
            }
            frame = output_structure<SaccadeCapturedFrame>();
            if (backend.ops.acquire(backend.context, stream, UINT64_C(2'000'000'000), &frame) != SACCADE_OK ||
                backend.ops.stop(backend.context, stream) != SACCADE_OK) {
                return 19;
            }
            held_frame = frame.frame;
        }
        if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
            return 20;
        }
        frame = output_structure<SaccadeCapturedFrame>();
        result = backend.ops.acquire(backend.context, stream, UINT64_C(250'000'000), &frame);
        if ((result != SACCADE_ERROR_TIMEOUT && result != SACCADE_ERROR_BUSY) ||
            backend.ops.stop(backend.context, stream) != SACCADE_OK) {
            return 21;
        }
        stats = {};
        if (provider.read_stats(stream, &stats) != SACCADE_OK || stats.acquired != held.size() ||
            stats.dropped_capacity == 0) {
            return 22;
        }
        for (SaccadeFrameHandle held_frame : held) {
            if (backend.ops.release(backend.context, stream, held_frame) != SACCADE_OK) {
                return 23;
            }
        }
        if (backend.ops.synchronize(backend.context, stream, UINT64_C(1'000'000'000)) != SACCADE_OK ||
            backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
            return 24;
        }

        bool found_window = false;
        bool captured_window = false;
        for (uint32_t index = 1; index < 256 && !captured_window; ++index) {
            SaccadeCaptureSourceInfo window = output_structure<SaccadeCaptureSourceInfo>();
            result = backend.ops.enumerate_sources(backend.context, index, &window);
            if (result == SACCADE_ERROR_NOT_FOUND) {
                break;
            }
            if (result != SACCADE_OK || window.kind != SACCADE_CAPTURE_SOURCE_WINDOW ||
                window.desktop_bounds.width < 64 || window.desktop_bounds.height < 64) {
                continue;
            }
            found_window = true;
            stream_desc.source_id = window.stable_id;
            stream_desc.max_width = 1536;
            stream_desc.max_height = 1024;
            stream = 0;
            if (backend.ops.create(backend.context, &stream_desc, &stream) != SACCADE_OK) {
                continue;
            }
            if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
                (void)backend.ops.destroy(backend.context, stream);
                continue;
            }
            frame = output_structure<SaccadeCapturedFrame>();
            result = backend.ops.acquire(backend.context, stream, UINT64_C(500'000'000), &frame);
            if (result == SACCADE_OK) {
                captured_window = frame.source_id == window.stable_id && frame.width != 0 && frame.height != 0 &&
                                  provider.read_native_frame(stream, frame.frame, &native) == SACCADE_OK &&
                                  native.metal_texture != nullptr &&
                                  backend.ops.release(backend.context, stream, frame.frame) == SACCADE_OK;
            }
            if (backend.ops.stop(backend.context, stream) != SACCADE_OK ||
                backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
                return 25;
            }
        }
        if (!found_window) {
            return 77;
        }
        if (!captured_window) {
            return 26;
        }
    }
    return 0;
}

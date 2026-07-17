#include "platform/windows/screen_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <array>
#include <charconv>
#include <cstdint>

namespace {

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

void report_native_error(const saccade::platform::windows::ScreenCaptureProvider& provider) noexcept {
    int32_t error = 0;
    std::array<char, 32> text{'H', 'R', 'E', 'S', 'U', 'L', 'T', '=', '0', 'x'};
    if (provider.read_last_native_error(&error) != SACCADE_OK) {
        return;
    }
    auto converted = std::to_chars(text.data() + 10, text.data() + 30, static_cast<uint32_t>(error), 16);
    if (converted.ec != std::errc{}) {
        return;
    }
    *converted.ptr++ = '\n';
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(converted.ptr - text.data()),
                    &written, nullptr);
}

bool detached_from_console() noexcept {
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE && session != WTSGetActiveConsoleSessionId();
}

} // namespace

int main() {
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) {
        return 1;
    }
    saccade::platform::windows::ScreenCaptureProvider provider;
    const SaccadeResult initialized = provider.initialize();
    if (initialized == SACCADE_ERROR_UNSUPPORTED) {
        return 77;
    }
    if (initialized != SACCADE_OK) {
        return 2;
    }
    const SaccadeCaptureProviderDesc backend = provider.descriptor();
    if (backend.context == nullptr || backend.ops.enumerate_sources == nullptr || backend.ops.create == nullptr ||
        backend.ops.start == nullptr || backend.ops.acquire == nullptr || backend.ops.release == nullptr ||
        backend.ops.stop == nullptr || backend.ops.destroy == nullptr) {
        return 3;
    }
    SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
    if (backend.ops.enumerate_sources(backend.context, 0, &source) != SACCADE_OK ||
        source.kind != SACCADE_CAPTURE_SOURCE_DISPLAY || source.stable_id == 0 || source.desktop_bounds.width <= 0 ||
        source.desktop_bounds.height <= 0) {
        return 4;
    }
    SaccadeCaptureStreamDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.source_id = source.stable_id;
    desc.pixel_format = SACCADE_FORMAT_BGRA8;
    desc.queue_capacity = 3;
    SaccadeCaptureStreamHandle stream = 0;
    if (backend.ops.create(backend.context, &desc, &stream) != SACCADE_OK || stream == 0) {
        int32_t native_error = 0;
        (void)provider.read_last_native_error(&native_error);
        if (native_error == static_cast<int32_t>(E_INVALIDARG) && detached_from_console()) {
            return 77;
        }
        report_native_error(provider);
        return 5;
    }
    if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
        report_native_error(provider);
        return 12;
    }

    SaccadeCapturedFrame frame{};
    SaccadeResult acquired = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < 300 && acquired == SACCADE_ERROR_BUSY; ++attempt) {
        Sleep(10);
        frame = output_structure<SaccadeCapturedFrame>();
        acquired = backend.ops.acquire(backend.context, stream, 0, &frame);
    }
    if (acquired != SACCADE_OK || frame.frame == 0 || frame.width == 0 || frame.height == 0 ||
        frame.pixel_format != SACCADE_FORMAT_BGRA8 || frame.transform_epoch == 0) {
        return 6;
    }
    saccade::platform::windows::NativeCapturedFrame native{};
    if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK || native.d3d11_texture == nullptr ||
        native.width < frame.width || native.height < frame.height || native.pixel_format != SACCADE_FORMAT_BGRA8) {
        return 7;
    }
    D3D11_TEXTURE2D_DESC texture_desc{};
    static_cast<ID3D11Texture2D*>(native.d3d11_texture)->GetDesc(&texture_desc);
    if (texture_desc.Width != native.width || texture_desc.Height != native.height ||
        (texture_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        return 8;
    }
    std::array<SaccadeRectI32, 64> damage{};
    uint32_t damage_count = 0;
    if (backend.ops.copy_damage(backend.context, stream, frame.frame, damage.data(),
                                static_cast<uint32_t>(damage.size()), &damage_count) != SACCADE_OK ||
        damage_count != frame.damage_count) {
        return 9;
    }
    if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK ||
        backend.ops.release(backend.context, stream, frame.frame) != SACCADE_ERROR_STALE_HANDLE) {
        return 10;
    }
    SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
    saccade::platform::windows::ScreenCaptureStats stats{};
    if (backend.ops.memory_stats(backend.context, stream, &memory) != SACCADE_OK || memory.copied_bytes != 0 ||
        memory.framework_opaque == 0 || provider.read_stats(stream, &stats) != SACCADE_OK || stats.acquired != 1 ||
        stats.released != 1 || stats.stale_releases != 1 || backend.ops.stop(backend.context, stream) != SACCADE_OK ||
        backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
        return 11;
    }
    (void)SetThreadDpiAwarenessContext(previous);
    return 0;
}

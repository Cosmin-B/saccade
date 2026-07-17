#include "core/stack_string_builder.hpp"
#include "platform/windows/screen_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;
using saccade::core::StackStringBuilder;

enum class ToolResult : int {
    success,
    usage,
    dpi_awareness_failed,
    provider_failed,
    source_failed,
    stream_failed,
    acquire_failed,
    native_frame_failed,
    staging_failed,
    output_failed,
    cleanup_failed
};

constexpr uint32_t default_capture_count = 4;
constexpr uint32_t default_source_index = 0;
constexpr uint32_t default_interval_ms = 350;
constexpr uint32_t maximum_capture_count = 32;
constexpr uint32_t acquire_attempts = 300;
constexpr uint32_t acquire_pause_ms = 10;
constexpr size_t pixel_bytes = 4;
constexpr size_t conversion_pixels = 16384;
constexpr size_t conversion_bytes = conversion_pixels * pixel_bytes;

thread_local std::array<uint8_t, conversion_bytes> conversion_buffer;

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

int result(ToolResult value) noexcept {
    return static_cast<int>(value);
}

bool parse_u32(const char* text, uint32_t minimum, uint32_t maximum, uint32_t* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const char* end = text;
    while (*end != '\0')
        ++end;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum) return false;
    *output = value;
    return true;
}

bool write_all(HANDLE file, const void* bytes, size_t size) noexcept {
    const auto* source = static_cast<const uint8_t*>(bytes);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(size > MAXDWORD ? MAXDWORD : size);
        DWORD written = 0;
        if (WriteFile(file, source, chunk, &written, nullptr) == FALSE || written == 0) return false;
        source += written;
        size -= written;
    }
    return true;
}

bool make_output_path(const char* directory, uint32_t capture_index, uint32_t width, uint32_t height,
                      StackStringBuilder<1024>* output) noexcept {
    output->reset();
    const std::string_view root(directory);
    const bool separator = !root.empty() && root.back() != '\\' && root.back() != '/';
    return output->append(root) && (!separator || output->append('\\')) && output->append("capture-") &&
           output->append_unsigned(capture_index) && output->append('-') && output->append_unsigned(width) &&
           output->append('x') && output->append_unsigned(height) && output->append(".pam");
}

bool write_pam(const char* path, const uint8_t* base, size_t row_pitch, uint32_t width, uint32_t height,
               bool force_opaque) noexcept {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    StackStringBuilder<256> header;
    const bool header_ready = header.append("P7\nWIDTH ") && header.append_unsigned(width) &&
                              header.append("\nHEIGHT ") && header.append_unsigned(height) &&
                              header.append("\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n");
    bool written = header_ready && write_all(file, header.view().data(), header.view().size());
    for (uint32_t y = 0; written && y < height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * row_pitch;
        uint32_t x = 0;
        while (written && x < width) {
            const uint32_t count =
                (width - x) < conversion_pixels ? width - x : static_cast<uint32_t>(conversion_pixels);
            for (uint32_t pixel = 0; pixel < count; ++pixel) {
                const uint8_t* bgra = row + static_cast<size_t>(x + pixel) * pixel_bytes;
                uint8_t* rgba = conversion_buffer.data() + static_cast<size_t>(pixel) * pixel_bytes;
                rgba[0] = bgra[2];
                rgba[1] = bgra[1];
                rgba[2] = bgra[0];
                rgba[3] = force_opaque ? UINT8_MAX : bgra[3];
            }
            written = write_all(file, conversion_buffer.data(), static_cast<size_t>(count) * pixel_bytes);
            x += count;
        }
    }
    const bool closed = CloseHandle(file) != FALSE;
    return written && closed;
}

bool write_pam(const char* path, const D3D11_MAPPED_SUBRESOURCE& mapped, uint32_t width, uint32_t height) noexcept {
    return write_pam(path, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, width, height, false);
}

bool staging_texture(ID3D11Texture2D* source, ComPtr<ID3D11Texture2D>* staging,
                     ComPtr<ID3D11DeviceContext>* context) noexcept {
    if (source == nullptr || staging == nullptr || context == nullptr) return false;
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    ComPtr<ID3D11Device> device;
    source->GetDevice(device.GetAddressOf());
    if (!device) return false;
    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, staging->ReleaseAndGetAddressOf()))) {
        return false;
    }
    device->GetImmediateContext(context->ReleaseAndGetAddressOf());
    return *context != nullptr;
}

void emit_capture(const char* path, const SaccadeCapturedFrame& frame) noexcept {
    StackStringBuilder<1200> text;
    if (!text.append("captured path=") || !text.append(path) || !text.append(" width=") ||
        !text.append_unsigned(frame.width) || !text.append(" height=") || !text.append_unsigned(frame.height) ||
        !text.append(" frame_id=") || !text.append_unsigned(frame.frame_id) || !text.append(" timestamp_ns=") ||
        !text.append_unsigned(frame.timestamp_ns) || !text.append('\n'))
        return;
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.view().data(), static_cast<DWORD>(text.view().size()),
                    &written, nullptr);
}

void emit_line(const StackStringBuilder<512>& text, HANDLE stream) noexcept {
    DWORD written = 0;
    (void)WriteFile(stream, text.view().data(), static_cast<DWORD>(text.view().size()), &written, nullptr);
}

void emit_environment() noexcept {
    DWORD session = 0;
    (void)ProcessIdToSessionId(GetCurrentProcessId(), &session);
    StackStringBuilder<512> text;
    if (text.append("process_id=") && text.append_unsigned(GetCurrentProcessId()) && text.append(" session_id=") &&
        text.append_unsigned(session) && text.append(" active_console_session=") &&
        text.append_unsigned(WTSGetActiveConsoleSessionId()) && text.append('\n')) {
        emit_line(text, GetStdHandle(STD_OUTPUT_HANDLE));
    }
}

void emit_provider_failure(std::string_view operation, SaccadeResult operation_result,
                           const saccade::platform::windows::ScreenCaptureProvider& provider) noexcept {
    int32_t native_error = 0;
    (void)provider.read_last_native_error(&native_error);
    std::array<char, 16> hexadecimal{};
    const auto converted = std::to_chars(hexadecimal.data(), hexadecimal.data() + hexadecimal.size(),
                                         static_cast<uint32_t>(native_error), 16);
    StackStringBuilder<512> text;
    if (converted.ec == std::errc{} && text.append("failure operation=") && text.append(operation) &&
        text.append(" result=") && text.append_signed(operation_result) && text.append(" hresult=0x") &&
        text.append(std::string_view(hexadecimal.data(), static_cast<size_t>(converted.ptr - hexadecimal.data()))) &&
        text.append('\n')) {
        emit_line(text, GetStdHandle(STD_ERROR_HANDLE));
    }
}

void emit_mode(std::string_view mode) noexcept {
    StackStringBuilder<512> text;
    if (text.append("capture_mode=") && text.append(mode) && text.append('\n')) {
        emit_line(text, GetStdHandle(STD_OUTPUT_HANDLE));
    }
}

void emit_win32_failure(std::string_view operation) noexcept {
    StackStringBuilder<512> text;
    if (text.append("win32_failure operation=") && text.append(operation) && text.append(" error=") &&
        text.append_unsigned(GetLastError()) && text.append('\n')) {
        emit_line(text, GetStdHandle(STD_ERROR_HANDLE));
    }
}

ToolResult capture_gdi(const char* directory, uint32_t capture_count, uint32_t interval_ms,
                       const SaccadeRectI32& bounds) noexcept {
    if (bounds.width <= 0 || bounds.height <= 0) return ToolResult::source_failed;
    HDC desktop = GetDC(nullptr);
    HDC memory = desktop != nullptr ? CreateCompatibleDC(desktop) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = bounds.width;
    info.bmiHeader.biHeight = -bounds.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap =
        memory != nullptr ? CreateDIBSection(desktop, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ previous = bitmap != nullptr ? SelectObject(memory, bitmap) : nullptr;
    if (desktop == nullptr || memory == nullptr || bitmap == nullptr || pixels == nullptr || previous == nullptr ||
        previous == HGDI_ERROR) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        if (desktop != nullptr) ReleaseDC(nullptr, desktop);
        return ToolResult::staging_failed;
    }
    emit_mode("gdi_remote_diagnostic");
    StackStringBuilder<1024> path;
    ToolResult capture_result = ToolResult::success;
    for (uint32_t capture_index = 1; capture_index <= capture_count; ++capture_index) {
        if (BitBlt(memory, 0, 0, bounds.width, bounds.height, desktop, bounds.x, bounds.y, SRCCOPY | CAPTUREBLT) ==
            FALSE) {
            emit_win32_failure("bitblt");
            capture_result = ToolResult::output_failed;
            break;
        }
        if (!make_output_path(directory, capture_index, static_cast<uint32_t>(bounds.width),
                              static_cast<uint32_t>(bounds.height), &path)) {
            emit_win32_failure("output_path");
            capture_result = ToolResult::output_failed;
            break;
        }
        if (!write_pam(path.c_str(), static_cast<const uint8_t*>(pixels),
                       static_cast<size_t>(bounds.width) * pixel_bytes, static_cast<uint32_t>(bounds.width),
                       static_cast<uint32_t>(bounds.height), true)) {
            emit_win32_failure("write_pam");
            capture_result = ToolResult::output_failed;
            break;
        }
        SaccadeCapturedFrame frame{};
        frame.frame_id = capture_index;
        frame.timestamp_ns = GetTickCount64() * UINT64_C(1'000'000);
        frame.width = static_cast<uint32_t>(bounds.width);
        frame.height = static_cast<uint32_t>(bounds.height);
        emit_capture(path.c_str(), frame);
        if (capture_index != capture_count && interval_ms != 0) Sleep(interval_ms);
    }
    (void)SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, desktop);
    return capture_result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) return result(ToolResult::usage);
    uint32_t capture_count = default_capture_count;
    uint32_t source_index = default_source_index;
    uint32_t interval_ms = default_interval_ms;
    if ((argc >= 3 && !parse_u32(argv[2], 1, maximum_capture_count, &capture_count)) ||
        (argc >= 4 && !parse_u32(argv[3], 0, 63, &source_index)) ||
        (argc >= 5 && !parse_u32(argv[4], 0, 60'000, &interval_ms))) {
        return result(ToolResult::usage);
    }
    if (CreateDirectoryA(argv[1], nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) {
        return result(ToolResult::output_failed);
    }
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) return result(ToolResult::dpi_awareness_failed);
    emit_environment();

    saccade::platform::windows::ScreenCaptureProvider provider;
    if (provider.initialize() != SACCADE_OK) return result(ToolResult::provider_failed);
    const SaccadeCaptureProviderDesc backend = provider.descriptor();
    SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
    if (backend.ops.enumerate_sources(backend.context, source_index, &source) != SACCADE_OK ||
        source.kind != SACCADE_CAPTURE_SOURCE_DISPLAY) {
        return result(ToolResult::source_failed);
    }
    SaccadeCaptureStreamDesc stream_desc{};
    stream_desc.struct_size = sizeof(stream_desc);
    stream_desc.api_version = SACCADE_API_VERSION;
    stream_desc.source_id = source.stable_id;
    stream_desc.pixel_format = SACCADE_FORMAT_BGRA8;
    stream_desc.queue_capacity = 3;
    SaccadeCaptureStreamHandle stream = 0;
    const SaccadeResult created = backend.ops.create(backend.context, &stream_desc, &stream);
    if (created != SACCADE_OK) {
        emit_provider_failure("create", created, provider);
        int32_t native_error = 0;
        (void)provider.read_last_native_error(&native_error);
        if (native_error == static_cast<int32_t>(E_INVALIDARG)) {
            const ToolResult fallback = capture_gdi(argv[1], capture_count, interval_ms, source.desktop_bounds);
            (void)SetThreadDpiAwarenessContext(previous);
            return result(fallback);
        }
        return result(ToolResult::stream_failed);
    }
    emit_mode("windows_graphics_capture");
    const SaccadeResult started = backend.ops.start(backend.context, stream);
    if (started != SACCADE_OK) {
        emit_provider_failure("start", started, provider);
        return result(ToolResult::stream_failed);
    }

    StackStringBuilder<1024> path;
    for (uint32_t capture_index = 1; capture_index <= capture_count; ++capture_index) {
        SaccadeCapturedFrame frame = output_structure<SaccadeCapturedFrame>();
        SaccadeResult acquired = SACCADE_ERROR_BUSY;
        for (uint32_t attempt = 0; attempt < acquire_attempts && acquired == SACCADE_ERROR_BUSY; ++attempt) {
            Sleep(acquire_pause_ms);
            frame = output_structure<SaccadeCapturedFrame>();
            acquired = backend.ops.acquire(backend.context, stream, 0, &frame);
        }
        if (acquired != SACCADE_OK) return result(ToolResult::acquire_failed);
        saccade::platform::windows::NativeCapturedFrame native{};
        if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK || native.d3d11_texture == nullptr ||
            native.width < frame.width || native.height < frame.height) {
            return result(ToolResult::native_frame_failed);
        }
        auto* texture = static_cast<ID3D11Texture2D*>(native.d3d11_texture);
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11DeviceContext> context;
        if (!staging_texture(texture, &staging, &context)) {
            return result(ToolResult::staging_failed);
        }
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return result(ToolResult::staging_failed);
        }
        const bool path_ready = make_output_path(argv[1], capture_index, frame.width, frame.height, &path);
        const bool saved = path_ready && write_pam(path.c_str(), mapped, frame.width, frame.height);
        context->Unmap(staging.Get(), 0);
        if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK) {
            return result(ToolResult::cleanup_failed);
        }
        if (!saved) return result(ToolResult::output_failed);
        emit_capture(path.c_str(), frame);
        if (capture_index != capture_count && interval_ms != 0) Sleep(interval_ms);
    }
    const bool cleaned = backend.ops.stop(backend.context, stream) == SACCADE_OK &&
                         backend.ops.destroy(backend.context, stream) == SACCADE_OK;
    (void)SetThreadDpiAwarenessContext(previous);
    return cleaned ? result(ToolResult::success) : result(ToolResult::cleanup_failed);
}

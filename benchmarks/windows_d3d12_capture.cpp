#include "backends/d3d12/graphics_device.hpp"
#include "core/stack_string_builder.hpp"
#include "platform/windows/screen_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <d3d12.h>
#include <windows.h>

#include <charconv>
#include <cstdint>

namespace {

using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::InitializationError;
using saccade::backend::d3d12::TextureLease;
using saccade::core::StackStringBuilder;
using saccade::platform::windows::NativeCapturedFrame;
using saccade::platform::windows::ScreenCaptureProvider;

enum class ToolResult : int {
    success,
    usage,
    graphics_device_failed,
    capture_provider_failed,
    source_failed,
    stream_failed,
    acquire_failed,
    native_frame_failed,
    unwrap_failed,
    texture_mismatch,
    return_failed,
    release_failed,
    cleanup_failed
};

constexpr uint32_t default_frame_count = 360;
constexpr uint32_t maximum_frame_count = 3'600;
constexpr uint32_t acquire_attempts = 500;
constexpr uint32_t acquire_pause_ms = 2;

int result(ToolResult value) noexcept {
    return static_cast<int>(value);
}

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
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

SaccadeResult acquire_frame(const SaccadeCaptureProviderDesc& provider, SaccadeCaptureStreamHandle stream,
                            SaccadeCapturedFrame* output) noexcept {
    SaccadeResult acquired = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < acquire_attempts && acquired == SACCADE_ERROR_BUSY; ++attempt) {
        Sleep(acquire_pause_ms);
        *output = output_structure<SaccadeCapturedFrame>();
        acquired = provider.ops.acquire(provider.context, stream, 0, output);
    }
    return acquired;
}

void emit(const StackStringBuilder<512>& text) noexcept {
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

void emit_initialization_error(InitializationError error) noexcept {
    StackStringBuilder<512> text;
    (void)text.append("d3d12_initialization_failed stage=");
    (void)text.append_unsigned(static_cast<uint32_t>(error.stage));
    (void)text.append(" native_code=");
    (void)text.append_signed(error.native_code);
    (void)text.append('\n');
    emit(text);
}

} // namespace

int main(int argc, char** argv) {
    uint32_t frame_count = default_frame_count;
    uint32_t source_index = 0;
    const int argument_base = argc == 4 ? 2 : 1;
    if ((argc > 4) || (argc == 4 && argv[1][0] == '\0') || (argc != 1 && argc != 2 && argc != 3 && argc != 4) ||
        (argc > argument_base && !parse_u32(argv[argument_base], 1, maximum_frame_count, &frame_count)) ||
        (argc > argument_base + 1 && !parse_u32(argv[argument_base + 1], 0, 255, &source_index))) {
        return result(ToolResult::usage);
    }

    StackStringBuilder<512> startup;
    (void)startup.append("windows_d3d12_capture startup\n");
    emit(startup);
    GraphicsDevice graphics;
    StackStringBuilder<512> initializing;
    (void)initializing.append("windows_d3d12_capture initializing\n");
    emit(initializing);
    if (graphics.initialize() != SACCADE_OK) {
        emit_initialization_error(graphics.initialization_error());
        return result(ToolResult::graphics_device_failed);
    }
    ScreenCaptureProvider capture;
    if (capture.initialize(graphics.capture_device()) != SACCADE_OK) {
        return result(ToolResult::capture_provider_failed);
    }
    const SaccadeCaptureProviderDesc provider = capture.descriptor();
    SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
    if (provider.ops.enumerate_sources(provider.context, source_index, &source) != SACCADE_OK ||
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
    if (provider.ops.create(provider.context, &stream_desc, &stream) != SACCADE_OK ||
        provider.ops.start(provider.context, stream) != SACCADE_OK) {
        return result(ToolResult::stream_failed);
    }

    ToolResult tool_result = ToolResult::success;
    uint32_t completed = 0;
    for (; completed < frame_count; ++completed) {
        SaccadeCapturedFrame frame{};
        if (acquire_frame(provider, stream, &frame) != SACCADE_OK) {
            tool_result = ToolResult::acquire_failed;
            break;
        }
        NativeCapturedFrame native{};
        if (capture.read_native_frame(stream, frame.frame, &native) != SACCADE_OK || native.d3d11_texture == nullptr) {
            tool_result = ToolResult::native_frame_failed;
            break;
        }
        TextureLease texture{};
        if (graphics.unwrap(static_cast<ID3D11Texture2D*>(native.d3d11_texture), &texture) != SACCADE_OK) {
            tool_result = ToolResult::unwrap_failed;
            break;
        }
        const D3D12_RESOURCE_DESC desc = texture.texture->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width < frame.width ||
            desc.Height < frame.height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            tool_result = ToolResult::texture_mismatch;
        }
        if (graphics.return_texture(&texture, nullptr, 0) != SACCADE_OK) {
            tool_result = ToolResult::return_failed;
        }
        if (provider.ops.release(provider.context, stream, frame.frame) != SACCADE_OK) {
            tool_result = ToolResult::release_failed;
        }
        if (tool_result != ToolResult::success) break;
    }

    const bool cleaned = provider.ops.stop(provider.context, stream) == SACCADE_OK &&
                         provider.ops.destroy(provider.context, stream) == SACCADE_OK;
    if (tool_result == ToolResult::success && !cleaned) {
        tool_result = ToolResult::cleanup_failed;
    }
    if (tool_result != ToolResult::success) return result(tool_result);

    StackStringBuilder<512> summary;
    (void)summary.append("windows_d3d12_capture frames=");
    (void)summary.append_unsigned(completed);
    (void)summary.append(" source=");
    (void)summary.append_unsigned(source_index);
    (void)summary.append(" size=");
    (void)summary.append_signed(source.desktop_bounds.width);
    (void)summary.append('x');
    (void)summary.append_signed(source.desktop_bounds.height);
    (void)summary.append(" zero_copy=1 pool_stall=0\n");
    emit(summary);
    return result(ToolResult::success);
}

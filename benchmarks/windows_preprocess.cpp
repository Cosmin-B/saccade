#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/preprocessor.hpp"
#include "core/stack_string_builder.hpp"
#include "platform/windows/screen_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>

namespace {

using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::ImagePreprocessor;
using saccade::backend::d3d12::TextureLease;
using saccade::backend::image::PreprocessSubmission;
using saccade::backend::image::TensorFormat;
using saccade::backend::image::TensorSpec;
using saccade::core::StackStringBuilder;
using saccade::platform::windows::NativeCapturedFrame;
using saccade::platform::windows::ScreenCaptureProvider;

enum class ToolResult : int {
    success,
    usage,
    timing_failed,
    provider_failed,
    source_failed,
    stream_failed,
    acquire_failed,
    native_frame_failed,
    preprocessor_failed,
    submission_failed,
    cleanup_failed
};

constexpr uint32_t default_frame_count = 180;
constexpr uint32_t maximum_frame_count = 600;
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

uint64_t nanoseconds(uint64_t ticks, uint64_t frequency) noexcept {
    constexpr uint64_t scale = UINT64_C(1'000'000'000);
    return ticks / frequency * scale + ticks % frequency * scale / frequency;
}

void emit(const StackStringBuilder<1024>& text) noexcept {
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

SaccadeResult acquire_frame(const SaccadeCaptureProviderDesc& backend, SaccadeCaptureStreamHandle stream,
                            SaccadeCapturedFrame* output) noexcept {
    SaccadeResult acquired = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < acquire_attempts && acquired == SACCADE_ERROR_BUSY; ++attempt) {
        Sleep(acquire_pause_ms);
        *output = output_structure<SaccadeCapturedFrame>();
        acquired = backend.ops.acquire(backend.context, stream, 0, output);
    }
    return acquired;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6 || argc > 7) return result(ToolResult::usage);
    uint32_t frame_count = default_frame_count;
    uint32_t source_index = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t tensor_format = 1;
    if (!parse_u32(argv[2], 1, maximum_frame_count, &frame_count) || !parse_u32(argv[3], 0, 63, &source_index) ||
        !parse_u32(argv[4], 1, 8192, &output_width) || !parse_u32(argv[5], 1, 8192, &output_height) ||
        (argc == 7 && !parse_u32(argv[6], 1, 2, &tensor_format))) {
        return result(ToolResult::usage);
    }
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
        return result(ToolResult::timing_failed);
    }

    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return result(ToolResult::provider_failed);
    }
    ScreenCaptureProvider provider;
    if (provider.initialize(graphics.capture_device()) != SACCADE_OK) {
        return result(ToolResult::provider_failed);
    }
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
    if (backend.ops.create(backend.context, &stream_desc, &stream) != SACCADE_OK ||
        backend.ops.start(backend.context, stream) != SACCADE_OK) {
        return result(ToolResult::stream_failed);
    }

    TensorSpec spec{};
    spec.width = output_width;
    spec.height = output_height;
    spec.format = tensor_format == 1 ? TensorFormat::planar_fp16 : TensorFormat::planar_int8;
    if (spec.format == TensorFormat::planar_int8) {
        spec.channel_scale = {255.0F, 255.0F, 255.0F};
        spec.channel_bias = {-128.0F, -128.0F, -128.0F};
    }
    ImagePreprocessor preprocessor;
    std::array<uint64_t, maximum_frame_count> latencies{};
    uint32_t completed = 0;
    ToolResult tool_result = ToolResult::success;
    SaccadeCapturedFrame frame{};
    NativeCapturedFrame native{};
    TextureLease texture{};
    if (acquire_frame(backend, stream, &frame) != SACCADE_OK) {
        tool_result = ToolResult::acquire_failed;
    } else if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK ||
               native.d3d11_texture == nullptr || native.width < frame.width || native.height < frame.height) {
        tool_result = ToolResult::native_frame_failed;
    }
    if (tool_result == ToolResult::success) {
        const SaccadeResult unwrapped = graphics.unwrap(static_cast<ID3D11Texture2D*>(native.d3d11_texture), &texture);
        const SaccadeResult initialized =
            unwrapped == SACCADE_OK ? preprocessor.initialize(graphics.device(), graphics.queue(), argv[1], spec)
                                    : unwrapped;
        if (initialized != SACCADE_OK) {
            tool_result = ToolResult::preprocessor_failed;
        }
    }
    for (; tool_result == ToolResult::success && completed < frame_count; ++completed) {
        LARGE_INTEGER begin{};
        LARGE_INTEGER end{};
        (void)QueryPerformanceCounter(&begin);
        PreprocessSubmission submission{};
        const SaccadeResult submitted =
            preprocessor.submit(texture.texture, native.width, native.height, {0, 0, frame.width, frame.height},
                                frame.frame_id, frame.transform_epoch, &submission);
        const SaccadeResult waited =
            submitted == SACCADE_OK ? preprocessor.wait(&submission, UINT64_C(1'000'000'000)) : submitted;
        (void)QueryPerformanceCounter(&end);
        if (waited != SACCADE_OK || end.QuadPart < begin.QuadPart) {
            StackStringBuilder<1024> failure;
            (void)failure.append("failure frame=");
            (void)failure.append_unsigned(completed + 1U);
            (void)failure.append(" preprocess_result=");
            (void)failure.append_signed(waited);
            (void)failure.append('\n');
            emit(failure);
            tool_result = ToolResult::submission_failed;
            break;
        }
        latencies[completed] = nanoseconds(static_cast<uint64_t>(end.QuadPart - begin.QuadPart),
                                           static_cast<uint64_t>(frequency.QuadPart));
    }

    const bool texture_returned =
        texture.texture == nullptr || graphics.return_texture(&texture, nullptr, 0) == SACCADE_OK;
    const bool frame_released =
        texture_returned &&
        (frame.frame == 0 || backend.ops.release(backend.context, stream, frame.frame) == SACCADE_OK);
    const bool cleaned = frame_released && backend.ops.stop(backend.context, stream) == SACCADE_OK &&
                         backend.ops.destroy(backend.context, stream) == SACCADE_OK;
    if (tool_result == ToolResult::success && !cleaned) {
        tool_result = ToolResult::cleanup_failed;
    }
    if (tool_result != ToolResult::success) return result(tool_result);

    std::sort(latencies.begin(), latencies.begin() + completed);
    const auto stats = preprocessor.stats();
    StackStringBuilder<1024> summary;
    (void)summary.append("windows_preprocess frames=");
    (void)summary.append_unsigned(completed);
    (void)summary.append(" format=");
    (void)summary.append(tensor_format == 1 ? "fp16" : "int8");
    (void)summary.append(" input=");
    (void)summary.append_unsigned(static_cast<uint32_t>(source.desktop_bounds.width));
    (void)summary.append('x');
    (void)summary.append_unsigned(static_cast<uint32_t>(source.desktop_bounds.height));
    (void)summary.append(" output=");
    (void)summary.append_unsigned(output_width);
    (void)summary.append('x');
    (void)summary.append_unsigned(output_height);
    (void)summary.append(" p50_ns=");
    (void)summary.append_unsigned(latencies[completed / 2U]);
    (void)summary.append(" p95_ns=");
    (void)summary.append_unsigned(latencies[completed * 95U / 100U]);
    (void)summary.append(" p99_ns=");
    (void)summary.append_unsigned(latencies[completed * 99U / 100U]);
    (void)summary.append(" source_views=");
    (void)summary.append_unsigned(1);
    (void)summary.append(" output_bytes=");
    (void)summary.append_unsigned(stats.output_bytes);
    (void)summary.append(" imported_bytes=");
    (void)summary.append_unsigned(static_cast<uint64_t>(native.width) * native.height * 4U);
    (void)summary.append(" copied_bytes=");
    (void)summary.append_unsigned(0);
    (void)summary.append('\n');
    emit(summary);
    return result(ToolResult::success);
}

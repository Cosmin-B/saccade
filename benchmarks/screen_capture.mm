#include "platform/macos/screen_capture.hpp"
#include "backends/metal/preprocessor.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <time.h>

namespace {

enum class ExitCode : int {
    success = 0,
    provider_initialization_failed = 1,
    source_enumeration_failed = 2,
    stream_creation_failed = 3,
    stream_start_failed = 4,
    frame_acquisition_failed = 5,
    frame_inspection_failed = 6,
    stream_cleanup_failed = 7,
    fitted_stream_creation_failed = 8,
    fitted_stream_start_failed = 9,
    fitted_frame_acquisition_failed = 10,
    fitted_frame_inspection_failed = 11,
    final_stream_cleanup_failed = 12,
    preprocessor_initialization_failed = 20,
    preprocessor_warmup_failed = 21,
    preprocessor_sample_failed = 22,
    preprocessor_memory_stats_failed = 23,
    invalid_arguments = 64,
    unsupported = 77
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = static_cast<uint32_t>(sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    return value;
}

const char* format_name(saccade::backend::metal::TensorFormat format) noexcept {
    using saccade::backend::metal::TensorFormat;
    if (format == TensorFormat::planar_fp16) return "planar_fp16";
    if (format == TensorFormat::planar_int8) return "planar_int8";
    return "image_bgra8";
}

ExitCode benchmark_preprocessor(void* device, const char* metallib_path, void* texture, uint32_t width, uint32_t height,
                                saccade::backend::metal::TensorFormat format,
                                saccade::backend::metal::PathPreference preference) {
    namespace metal = saccade::backend::metal;
    metal::TensorSpec spec{};
    spec.width = 1536;
    spec.height = 1024;
    spec.format = format;

    metal::ImagePreprocessor preprocessor;
    const SaccadeResult initialized = preprocessor.initialize(device, metallib_path, preference, spec);
    if (initialized == SACCADE_ERROR_UNSUPPORTED && preference == metal::PathPreference::metal4) {
        return ExitCode::success;
    }
    if (initialized != SACCADE_OK) {
        return ExitCode::preprocessor_initialization_failed;
    }

    constexpr uint32_t warmup_count = 8;
    constexpr uint32_t sample_count = 240;
    const metal::SourceRegion region{0, 0, width, height};
    for (uint32_t index = 0; index < warmup_count; ++index) {
        metal::PreprocessSubmission submission{};
        if (preprocessor.submit(texture, width, height, region, index + 1, 1, &submission) != SACCADE_OK ||
            preprocessor.wait(submission, UINT64_C(3'000'000'000)) != SACCADE_OK) {
            return ExitCode::preprocessor_warmup_failed;
        }
    }

    const uint64_t begin = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (uint32_t index = 0; index < sample_count; ++index) {
        metal::PreprocessSubmission submission{};
        if (preprocessor.submit(texture, width, height, region, warmup_count + index + 1, 1, &submission) !=
                SACCADE_OK ||
            preprocessor.wait(submission, UINT64_C(3'000'000'000)) != SACCADE_OK) {
            return ExitCode::preprocessor_sample_failed;
        }
    }
    const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - begin;
    SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
    if (preprocessor.memory_stats(&memory) != SACCADE_OK) {
        return ExitCode::preprocessor_memory_stats_failed;
    }
    const metal::PreprocessorStats stats = preprocessor.stats();
    std::printf("path=%s operation=live_capture_fused_preprocess format=%s "
                "source_resolution=%ux%u output_resolution=%ux%u samples=%u "
                "ns_per_frame=%llu output_bytes=%llu command_allocator_bytes=%llu\n",
                stats.path == metal::Path::metal4 ? "metal4" : "metal3", format_name(format), width, height, spec.width,
                spec.height, sample_count, static_cast<unsigned long long>(elapsed / sample_count),
                static_cast<unsigned long long>(stats.output_bytes),
                static_cast<unsigned long long>(stats.command_allocator_bytes));
    return ExitCode::success;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) {
            return exit_code(ExitCode::invalid_arguments);
        }
        if (!CGPreflightScreenCaptureAccess()) {
            return exit_code(ExitCode::unsupported);
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return exit_code(ExitCode::unsupported);
        }

        saccade::platform::macos::ScreenCaptureProvider provider;
        if (provider.initialize((__bridge void*)device) != SACCADE_OK) {
            return exit_code(ExitCode::provider_initialization_failed);
        }
        SaccadeCaptureProviderDesc backend = provider.descriptor();
        SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
        if (backend.ops.enumerate_sources(backend.context, 0, &source) != SACCADE_OK ||
            source.kind != SACCADE_CAPTURE_SOURCE_DISPLAY) {
            return exit_code(ExitCode::source_enumeration_failed);
        }

        SaccadeCaptureStreamDesc desc{};
        desc.struct_size = static_cast<uint32_t>(sizeof(desc));
        desc.api_version = SACCADE_API_VERSION;
        desc.source_id = source.stable_id;
        desc.pixel_format = SACCADE_FORMAT_BGRA8;
        desc.queue_capacity = 3;
        desc.max_width = 0;
        desc.max_height = 0;

        SaccadeCaptureStreamHandle stream = 0;
        if (backend.ops.create(backend.context, &desc, &stream) != SACCADE_OK) {
            return exit_code(ExitCode::stream_creation_failed);
        }
        const uint64_t begin = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
            return exit_code(ExitCode::stream_start_failed);
        }
        SaccadeCapturedFrame frame = output_structure<SaccadeCapturedFrame>();
        if (backend.ops.acquire(backend.context, stream, UINT64_C(3'000'000'000), &frame) != SACCADE_OK) {
            return exit_code(ExitCode::frame_acquisition_failed);
        }
        const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - begin;
        saccade::platform::macos::NativeCapturedFrame native{};
        SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
        if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK ||
            backend.ops.memory_stats(backend.context, stream, &memory) != SACCADE_OK) {
            return exit_code(ExitCode::frame_inspection_failed);
        }

        std::printf("path=screencapturekit policy=native "
                    "operation=start_to_zero_copy_first_frame "
                    "resolution=%ux%u elapsed_ns=%llu iosurface_bytes=%llu "
                    "copied_bytes=%llu queue_depth=3 iosurface_id=%llu\n",
                    frame.width, frame.height, static_cast<unsigned long long>(elapsed),
                    static_cast<unsigned long long>(memory.device_imported),
                    static_cast<unsigned long long>(memory.copied_bytes),
                    static_cast<unsigned long long>(native.iosurface_id));

        if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK ||
            backend.ops.stop(backend.context, stream) != SACCADE_OK ||
            backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
            return exit_code(ExitCode::stream_cleanup_failed);
        }

        desc.max_width = 1536;
        desc.max_height = 1024;
        stream = 0;
        if (backend.ops.create(backend.context, &desc, &stream) != SACCADE_OK) {
            return exit_code(ExitCode::fitted_stream_creation_failed);
        }
        const uint64_t fit_begin = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (backend.ops.start(backend.context, stream) != SACCADE_OK) {
            return exit_code(ExitCode::fitted_stream_start_failed);
        }
        frame = output_structure<SaccadeCapturedFrame>();
        if (backend.ops.acquire(backend.context, stream, UINT64_C(3'000'000'000), &frame) != SACCADE_OK) {
            return exit_code(ExitCode::fitted_frame_acquisition_failed);
        }
        const uint64_t fit_elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - fit_begin;
        native = {};
        memory = output_structure<SaccadeMemoryStats>();
        if (provider.read_native_frame(stream, frame.frame, &native) != SACCADE_OK ||
            backend.ops.memory_stats(backend.context, stream, &memory) != SACCADE_OK) {
            return exit_code(ExitCode::fitted_frame_inspection_failed);
        }
        std::printf("path=screencapturekit policy=fit_1536x1024 "
                    "operation=start_to_zero_copy_first_frame "
                    "resolution=%ux%u elapsed_ns=%llu iosurface_bytes=%llu "
                    "copied_bytes=%llu queue_depth=3 iosurface_id=%llu\n",
                    frame.width, frame.height, static_cast<unsigned long long>(fit_elapsed),
                    static_cast<unsigned long long>(memory.device_imported),
                    static_cast<unsigned long long>(memory.copied_bytes),
                    static_cast<unsigned long long>(native.iosurface_id));
        const auto device_pointer = (__bridge void*)device;
        ExitCode benchmark_result = benchmark_preprocessor(
            device_pointer, argv[1], native.metal_texture, frame.width, frame.height,
            saccade::backend::metal::TensorFormat::planar_fp16, saccade::backend::metal::PathPreference::automatic);
        if (benchmark_result == ExitCode::success) {
            benchmark_result = benchmark_preprocessor(device_pointer, argv[1], native.metal_texture, frame.width,
                                                      frame.height, saccade::backend::metal::TensorFormat::planar_int8,
                                                      saccade::backend::metal::PathPreference::automatic);
        }
        if (benchmark_result == ExitCode::success) {
            benchmark_result = benchmark_preprocessor(device_pointer, argv[1], native.metal_texture, frame.width,
                                                      frame.height, saccade::backend::metal::TensorFormat::image_bgra8,
                                                      saccade::backend::metal::PathPreference::automatic);
        }
        if (benchmark_result == ExitCode::success) {
            benchmark_result = benchmark_preprocessor(device_pointer, argv[1], native.metal_texture, frame.width,
                                                      frame.height, saccade::backend::metal::TensorFormat::planar_fp16,
                                                      saccade::backend::metal::PathPreference::metal3);
        }
        if (benchmark_result == ExitCode::success) {
            benchmark_result = benchmark_preprocessor(device_pointer, argv[1], native.metal_texture, frame.width,
                                                      frame.height, saccade::backend::metal::TensorFormat::image_bgra8,
                                                      saccade::backend::metal::PathPreference::metal3);
        }
        if (benchmark_result != ExitCode::success) {
            return exit_code(benchmark_result);
        }
        if (backend.ops.release(backend.context, stream, frame.frame) != SACCADE_OK ||
            backend.ops.stop(backend.context, stream) != SACCADE_OK ||
            backend.ops.destroy(backend.context, stream) != SACCADE_OK) {
            return exit_code(ExitCode::final_stream_cleanup_failed);
        }
        return exit_code(ExitCode::success);
    }
}

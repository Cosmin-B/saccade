#include "backends/metal/preprocessor.hpp"
#include "core/stack_string_builder.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <time.h>
#include <unistd.h>

namespace {

using saccade::backend::metal::ImagePreprocessor;
using saccade::backend::metal::Path;
using saccade::backend::metal::PathPreference;
using saccade::backend::metal::PreprocessSubmission;
using saccade::backend::metal::TensorFormat;
using saccade::backend::metal::TensorSpec;
using saccade::core::StackStringBuilder;

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    device_failure = 2,
    initialization_failure = 3,
    texture_failure = 4,
    warmup_failure = 5,
    sample_failure = 6,
    memory_stats_failure = 7,
    output_failure = 8,
    performance_failure = 9,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t input_width = 1280;
constexpr uint32_t input_height = 768;
constexpr uint32_t warmup_count = 8;
constexpr uint32_t sample_count = 128;
constexpr uint64_t wait_timeout_ns = UINT64_C(1'000'000'000);
constexpr uint64_t maximum_p95_ns = UINT64_C(2'000'000);
constexpr float letterbox_channel = 114.0F / 255.0F;

struct Resolution {
    uint32_t width;
    uint32_t height;
};

struct Result {
    Resolution source{};
    uint64_t source_bytes = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
};

constexpr std::array<Resolution, 5> resolutions{{
    {1920, 1080},
    {2560, 1440},
    {3840, 2160},
    {5120, 2880},
    {7680, 4320},
}};

uint64_t monotonic_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t percentile(std::array<uint64_t, sample_count>* samples, uint32_t numerator) noexcept {
    std::sort(samples->begin(), samples->end());
    const uint32_t index = ((sample_count - 1U) * numerator + 50U) / 100U;
    return samples->at(index);
}

id<MTLTexture> make_texture(id<MTLDevice> device, Resolution resolution) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:resolution.width
                                                                                         height:resolution.height
                                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:descriptor];
}

bool run_once(ImagePreprocessor* preprocessor, id<MTLTexture> texture, Resolution resolution,
              uint64_t frame_id) noexcept {
    PreprocessSubmission submission{};
    return preprocessor->submit((__bridge void*)texture, resolution.width, resolution.height, {}, frame_id, 1,
                                &submission) == SACCADE_OK &&
           preprocessor->wait(submission, wait_timeout_ns) == SACCADE_OK;
}

bool append_unsigned(StackStringBuilder<4096>* output, std::string_view name, uint64_t value,
                     bool comma = true) noexcept {
    return output->append('"') && output->append(name) && output->append("\":") && output->append_unsigned(value) &&
           (!comma || output->append(','));
}

bool path_preference(const char* text, PathPreference* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const std::string_view value{text};
    if (value == "automatic") {
        *output = PathPreference::automatic;
        return true;
    }
    if (value == "metal3") {
        *output = PathPreference::metal3;
        return true;
    }
    if (value == "metal4") {
        *output = PathPreference::metal4;
        return true;
    }
    return false;
}

std::string_view path_name(Path path) noexcept {
    if (path == Path::metal4) return "metal4";
    if (path == Path::metal3) return "metal3";
    return "unavailable";
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 3) return to_process_exit_code(ExitCode::invalid_arguments);
        PathPreference preference = PathPreference::automatic;
        if (argc == 3 && !path_preference(argv[2], &preference))
            return to_process_exit_code(ExitCode::invalid_arguments);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) return to_process_exit_code(ExitCode::device_failure);

        TensorSpec spec{};
        spec.width = input_width;
        spec.height = input_height;
        spec.format = TensorFormat::image_bgra8;
        spec.letterbox_rgb = {letterbox_channel, letterbox_channel, letterbox_channel};
        ImagePreprocessor preprocessor;
        if (preprocessor.initialize((__bridge void*)device, argv[1], preference, spec) != SACCADE_OK) {
            return to_process_exit_code(ExitCode::initialization_failure);
        }

        std::array<Result, resolutions.size()> results{};
        uint64_t frame_id = 1;
        bool qualified = true;
        for (uint32_t resolution_index = 0; resolution_index < resolutions.size(); ++resolution_index) {
            const Resolution resolution = resolutions[resolution_index];
            id<MTLTexture> texture = make_texture(device, resolution);
            if (texture == nil) return to_process_exit_code(ExitCode::texture_failure);

            for (uint32_t warmup = 0; warmup < warmup_count; ++warmup)
                if (!run_once(&preprocessor, texture, resolution, frame_id++))
                    return to_process_exit_code(ExitCode::warmup_failure);

            std::array<uint64_t, sample_count> samples{};
            for (uint32_t sample = 0; sample < sample_count; ++sample) {
                const uint64_t started_ns = monotonic_ns();
                if (!run_once(&preprocessor, texture, resolution, frame_id++))
                    return to_process_exit_code(ExitCode::sample_failure);
                samples[sample] = monotonic_ns() - started_ns;
            }

            Result& result = results[resolution_index];
            result.source = resolution;
            result.source_bytes = static_cast<uint64_t>(resolution.width) * resolution.height * 4U;
            result.p50_ns = percentile(&samples, 50);
            result.p95_ns = percentile(&samples, 95);
            result.p99_ns = percentile(&samples, 99);
            qualified = qualified && result.p95_ns <= maximum_p95_ns;
        }

        SaccadeMemoryStats memory{};
        memory.struct_size = sizeof(memory);
        memory.api_version = SACCADE_API_VERSION;
        if (preprocessor.memory_stats(&memory) != SACCADE_OK)
            return to_process_exit_code(ExitCode::memory_stats_failure);

        StackStringBuilder<4096> output;
        const std::string_view selected_path = path_name(preprocessor.stats().path);
        bool written = output.append("{\"path\":\"") && output.append(selected_path) && output.append("\",") &&
                       append_unsigned(&output, "input_width", input_width) &&
                       append_unsigned(&output, "input_height", input_height) &&
                       append_unsigned(&output, "warmups", warmup_count) &&
                       append_unsigned(&output, "samples", sample_count) &&
                       append_unsigned(&output, "maximum_p95_ns", maximum_p95_ns) &&
                       append_unsigned(&output, "preprocess_high_water", memory.high_water_bytes) &&
                       output.append("\"resolutions\":[");
        for (uint32_t index = 0; written && index < results.size(); ++index) {
            const Result& result = results[index];
            written = output.append('{') && append_unsigned(&output, "width", result.source.width) &&
                      append_unsigned(&output, "height", result.source.height) &&
                      append_unsigned(&output, "source_bytes", result.source_bytes) &&
                      append_unsigned(&output, "p50_ns", result.p50_ns) &&
                      append_unsigned(&output, "p95_ns", result.p95_ns) &&
                      append_unsigned(&output, "p99_ns", result.p99_ns, false) && output.append('}') &&
                      (index + 1U == results.size() || output.append(','));
        }
        written = written && output.append("],") && append_unsigned(&output, "qualified", qualified ? 1U : 0U, false) &&
                  output.append("}\n");
        if (!written || output.truncated() ||
            write(STDOUT_FILENO, output.view().data(), output.view().size()) !=
                static_cast<ssize_t>(output.view().size())) {
            return to_process_exit_code(ExitCode::output_failure);
        }
        return to_process_exit_code(qualified ? ExitCode::success : ExitCode::performance_failure);
    }
}

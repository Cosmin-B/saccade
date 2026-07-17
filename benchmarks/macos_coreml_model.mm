#include "model/mapped_artifact.hpp"
#include "platform/macos/coreml_model.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <mach/mach.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using saccade::model::ArtifactView;
using saccade::model::MappedArtifact;
using saccade::platform::macos::CoreMlComputePolicy;
using saccade::platform::macos::CoreMlModel;
using saccade::platform::macos::CoreMlModelConfig;
using saccade::platform::macos::CoreMlPrediction;
using saccade::platform::macos::CoreMlPredictionResult;

constexpr uint32_t default_warmup_runs = 5;
constexpr uint32_t default_measured_runs = 100;
constexpr uint32_t maximum_runs = 1000;
constexpr size_t maximum_packet_bytes =
    sizeof(SaccadeTargetPacketHeader) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);

enum class ExitCode : int { success, usage, artifact, pixel_buffer, model, warmup, prediction, timing, shutdown };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult accept_lab_signature(void*, const ArtifactView&) noexcept {
    return SACCADE_OK;
}

bool parse_count(const char* text, uint32_t* output) noexcept {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > maximum_runs) return false;
    *output = static_cast<uint32_t>(value);
    return true;
}

bool compute_policy(const char* text, CoreMlComputePolicy* output) noexcept {
    if (std::strcmp(text, "all") == 0) {
        *output = CoreMlComputePolicy::all;
    } else if (std::strcmp(text, "cpu-gpu") == 0) {
        *output = CoreMlComputePolicy::cpu_and_gpu;
    } else if (std::strcmp(text, "cpu") == 0) {
        *output = CoreMlComputePolicy::cpu_only;
    } else if (std::strcmp(text, "cpu-ne") == 0) {
        *output = CoreMlComputePolicy::cpu_and_neural_engine;
    } else {
        return false;
    }
    return true;
}

CVPixelBufferRef make_pixel_buffer(uint32_t width, uint32_t height) noexcept {
    NSDictionary* attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    CVPixelBufferRef buffer = nullptr;
    const CVReturn created = CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                                                 (__bridge CFDictionaryRef)attributes, &buffer);
    if (created != kCVReturnSuccess || buffer == nullptr) return nullptr;

    if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(buffer);
        return nullptr;
    }
    std::memset(CVPixelBufferGetBaseAddress(buffer), 0, CVPixelBufferGetDataSize(buffer));
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

uint64_t resident_bytes() noexcept {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
                   KERN_SUCCESS
               ? info.resident_size
               : 0;
}

double milliseconds(uint64_t start, uint64_t end, mach_timebase_info_data_t timebase) noexcept {
    const long double nanoseconds = static_cast<long double>(end - start) * timebase.numer / timebase.denom;
    return static_cast<double>(nanoseconds / 1'000'000.0L);
}

double percentile(const std::array<double, maximum_runs>& values, uint32_t count, uint32_t numerator,
                  uint32_t denominator) noexcept {
    const uint32_t index = std::min(count - 1U, ((count - 1U) * numerator + denominator / 2U) / denominator);
    return values[index];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 6) return exit_code(ExitCode::usage);

    CoreMlComputePolicy policy = CoreMlComputePolicy::all;
    uint32_t measured_runs = default_measured_runs;
    uint32_t warmup_runs = default_warmup_runs;
    if ((argc >= 4 && !compute_policy(argv[3], &policy)) || (argc >= 5 && !parse_count(argv[4], &measured_runs)) ||
        (argc == 6 && !parse_count(argv[5], &warmup_runs))) {
        return exit_code(ExitCode::usage);
    }

    MappedArtifact artifact;
    if (artifact.initialize(argv[1], {nullptr, accept_lab_signature}) != SACCADE_OK) {
        return exit_code(ExitCode::artifact);
    }

    const ArtifactView& view = artifact.view();
    CVPixelBufferRef pixel_buffer = make_pixel_buffer(view.input_width, view.input_height);
    if (pixel_buffer == nullptr) return exit_code(ExitCode::pixel_buffer);

    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS) {
        CVPixelBufferRelease(pixel_buffer);
        return exit_code(ExitCode::timing);
    }

    const uint64_t resident_before = resident_bytes();
    static CoreMlModel model;
    const uint64_t load_start = mach_continuous_time();
    if (model.initialize(view, CoreMlModelConfig{argv[2], policy}) != SACCADE_OK) {
        CVPixelBufferRelease(pixel_buffer);
        return exit_code(ExitCode::model);
    }
    const uint64_t load_end = mach_continuous_time();
    const uint64_t resident_loaded = resident_bytes();

    CoreMlPrediction prediction{};
    prediction.pixel_buffer = pixel_buffer;
    prediction.width = view.input_width;
    prediction.height = view.input_height;
    prediction.pixel_format = SACCADE_FORMAT_BGRA8;
    prediction.scope = {0, 0, static_cast<int32_t>(view.input_width), static_cast<int32_t>(view.input_height)};
    prediction.epochs = {1, 2, 3, 4, 5, 6};

    alignas(SaccadeTargetPacketHeader) static std::array<uint8_t, maximum_packet_bytes> packet{};
    CoreMlPredictionResult result{};
    for (uint32_t index = 0; index < warmup_runs; ++index) {
        if (model.predict(prediction, {packet.data(), packet.size()}, &result) != SACCADE_OK) {
            CVPixelBufferRelease(pixel_buffer);
            return exit_code(ExitCode::warmup);
        }
    }

    std::array<double, maximum_runs> samples{};
    for (uint32_t index = 0; index < measured_runs; ++index) {
        const uint64_t start = mach_continuous_time();
        if (model.predict(prediction, {packet.data(), packet.size()}, &result) != SACCADE_OK) {
            CVPixelBufferRelease(pixel_buffer);
            return exit_code(ExitCode::prediction);
        }
        samples[index] = milliseconds(start, mach_continuous_time(), timebase);
    }

    const uint64_t resident_after = resident_bytes();
    std::sort(samples.begin(), samples.begin() + measured_runs);
    const auto stats = model.stats();

    std::printf("{\n"
                "  \"model_stable_id\": %llu,\n"
                "  \"input_width\": %u,\n"
                "  \"input_height\": %u,\n"
                "  \"model_load_ms\": %.3f,\n"
                "  \"warmup_runs\": %u,\n"
                "  \"measured_runs\": %u,\n"
                "  \"minimum_ms\": %.3f,\n"
                "  \"median_ms\": %.3f,\n"
                "  \"p95_ms\": %.3f,\n"
                "  \"p99_ms\": %.3f,\n"
                "  \"maximum_ms\": %.3f,\n"
                "  \"candidate_count\": %u,\n"
                "  \"target_count\": %u,\n"
                "  \"packet_bytes\": %zu,\n"
                "  \"resident_bytes_before\": %llu,\n"
                "  \"resident_bytes_loaded\": %llu,\n"
                "  \"resident_bytes_after\": %llu,\n"
                "  \"prediction_failures\": %llu\n"
                "}\n",
                static_cast<unsigned long long>(view.stable_id), view.input_width, view.input_height,
                milliseconds(load_start, load_end, timebase), warmup_runs, measured_runs, samples[0],
                percentile(samples, measured_runs, 50, 100), percentile(samples, measured_runs, 95, 100),
                percentile(samples, measured_runs, 99, 100), samples[measured_runs - 1U], result.candidate_count,
                result.target_count, result.byte_size, static_cast<unsigned long long>(resident_before),
                static_cast<unsigned long long>(resident_loaded), static_cast<unsigned long long>(resident_after),
                static_cast<unsigned long long>(stats.failures));

    CVPixelBufferRelease(pixel_buffer);
    if (model.shutdown() != SACCADE_OK || artifact.shutdown() != SACCADE_OK) {
        return exit_code(ExitCode::shutdown);
    }
    return exit_code(ExitCode::success);
}

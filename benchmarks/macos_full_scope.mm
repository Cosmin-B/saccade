#include "application/inference_runtime.hpp"
#include "core/stack_string_builder.hpp"
#include "model/coreml_contract.hpp"
#include "model/mapped_artifact.hpp"
#include "platform/macos/coreml_image_bridge.hpp"
#include "platform/macos/coreml_provider.hpp"
#include "platform/macos/display_topology.hpp"
#include "platform/macos/scene_capture.hpp"
#include "platform/macos/screen_capture.hpp"
#include "scene/store.hpp"
#include "scheduler/desktop_neural_coordinator.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <mach/mach.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace {

using saccade::application::InferenceRuntime;
using saccade::application::InferenceRuntimeConfig;
using saccade::core::StackStringBuilder;
using saccade::geometry::DisplayCatalog;
using saccade::geometry::DisplaySnapshot;
using saccade::model::ArtifactView;
using saccade::model::MappedArtifact;
using saccade::platform::macos::CoreMlComputePolicy;
using saccade::platform::macos::CoreMlImageBridge;
using saccade::platform::macos::CoreMlImageBridgeConfig;
using saccade::platform::macos::CoreMlInferenceProvider;
using saccade::platform::macos::CoreMlProviderConfig;
using saccade::platform::macos::DisplayCollector;
using saccade::platform::macos::SceneCaptureFrame;
using saccade::platform::macos::SceneCaptureSet;
using saccade::platform::macos::ScreenCaptureProvider;
using saccade::scheduler::DesktopNeuralAdvance;
using saccade::scheduler::DesktopNeuralCoordinator;
using saccade::scheduler::DesktopNeuralCoordinatorConfig;
using saccade::scheduler::DesktopNeuralCoordinatorStorage;
using saccade::scheduler::DesktopNeuralFrame;

constexpr uint32_t default_duration_seconds = 10;
constexpr uint32_t maximum_duration_seconds = 300;
constexpr uint32_t maximum_samples = maximum_duration_seconds * 60 + 1;
constexpr uint32_t minimum_soak_duration_seconds = 60;
constexpr uint32_t maximum_memory_samples = maximum_duration_seconds + 2;
constexpr uint32_t warmup_scenes = 4;
constexpr uint64_t session_epoch = 1;
constexpr uint64_t desktop_source_id = UINT64_C(0x5341434341444501);
constexpr uint64_t drain_timeout_ns = UINT64_C(5'000'000'000);
constexpr uint64_t minimum_refresh_millihz = UINT64_C(29'000);
constexpr uint64_t maximum_deadline_miss_ppm = UINT64_C(1'000);
constexpr uint64_t preprocess_lead_ns = UINT64_C(2'000'000);
constexpr uint64_t maximum_capture_age_ns = saccade::scheduler::scene_period_30hz_ns / 3U;
constexpr uint64_t stale_capture_retry_ns = UINT64_C(1'000'000);
constexpr uint64_t memory_sample_period_ns = UINT64_C(1'000'000'000);
constexpr uint64_t memory_warmup_ns = UINT64_C(10'000'000'000);
constexpr uint64_t maximum_tracked_high_water_growth = 0;
constexpr uint64_t maximum_resident_final_growth = UINT64_C(16) * 1024U * 1024U;
constexpr uint64_t maximum_resident_peak_growth = UINT64_C(32) * 1024U * 1024U;

enum class RunMode : uint8_t { live, stable_cached, continuous_change };

enum class ExitCode : int {
    success,
    usage,
    permission,
    device,
    artifact,
    provider,
    inference,
    capture,
    topology,
    bridge,
    scene_store,
    coordinator,
    run,
    memory,
    qualification,
    cleanup
};

struct BridgeSlot {
    CoreMlImageBridge bridge{};
    uint64_t scene_transform_epoch = 0;
    uint64_t preprocess_started_ns = 0;
    bool initialized = false;
};

struct Samples {
    std::array<uint64_t, maximum_samples> batch{};
    std::array<uint64_t, maximum_samples> full_scope{};
    std::array<uint64_t, maximum_samples> capture_age{};
    std::array<uint64_t, maximum_samples> capture_skew{};
    std::array<uint64_t, maximum_samples> preprocess{};
    uint32_t count = 0;
    uint32_t capture_count = 0;
    uint32_t preprocess_count = 0;
    uint32_t scenes_observed = 0;
    uint64_t cold_full_scope_ns = 0;
    uint64_t stale_capture_retries = 0;
};

struct MemorySample {
    uint64_t elapsed_ns = 0;
    uint64_t published_scenes = 0;
    uint64_t tracked_current = 0;
    uint64_t tracked_high_water = 0;
    uint64_t copied_bytes = 0;
    uint64_t resident = 0;
};

struct MemorySamples {
    std::array<MemorySample, maximum_memory_samples> values{};
    uint32_t count = 0;
};

struct MemoryTrend {
    MemorySample baseline{};
    MemorySample final{};
    uint64_t tracked_high_water_growth = 0;
    uint64_t copied_byte_growth = 0;
    uint64_t copied_byte_limit = 0;
    uint64_t resident_final_growth = 0;
    uint64_t resident_peak_growth = 0;
    bool available = false;
    bool qualified = false;
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

uint64_t monotonic_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t next_capture_deadline(uint64_t now_ns, uint64_t start_time_ns) noexcept {
    constexpr uint64_t phase_offset = saccade::scheduler::scene_period_30hz_ns - preprocess_lead_ns;
    const uint64_t phase = start_time_ns + phase_offset;
    return now_ns < phase ? phase
                          : phase + ((now_ns - phase) / saccade::scheduler::scene_period_30hz_ns + 1U) *
                                        saccade::scheduler::scene_period_30hz_ns;
}

bool parse_duration(const char* text, uint32_t* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const char* end = text;
    while (*end != '\0')
        ++end;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > maximum_duration_seconds) return false;
    *output = value;
    return true;
}

bool parse_mode(const char* text, RunMode* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const std::string_view value{text};
    if (value == "live") {
        *output = RunMode::live;
        return true;
    }
    if (value == "stable-cached") {
        *output = RunMode::stable_cached;
        return true;
    }
    if (value == "continuous-change") {
        *output = RunMode::continuous_change;
        return true;
    }
    return false;
}

std::string_view mode_name(RunMode mode) noexcept {
    switch (mode) {
    case RunMode::live:
        return "live";
    case RunMode::stable_cached:
        return "stable-cached";
    case RunMode::continuous_change:
        return "continuous-change";
    }
    return "invalid";
}

SaccadeResult trust_benchmark_artifact(void*, const ArtifactView&) noexcept {
    return SACCADE_OK;
}

uint64_t percentile(std::array<uint64_t, maximum_samples>* samples, uint32_t count, uint32_t numerator) noexcept {
    if (count == 0) return 0;
    std::sort(samples->begin(), samples->begin() + count);
    const uint32_t index = std::min(count - 1U, ((count - 1U) * numerator + 50U) / 100U);
    return samples->at(index);
}

uint64_t resident_bytes() noexcept {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
                   KERN_SUCCESS
               ? info.resident_size
               : 0;
}

void emit(std::string_view text) noexcept {
    (void)write(STDOUT_FILENO, text.data(), text.size());
}

int fail(ExitCode code, std::string_view stage, SaccadeResult result) noexcept {
    StackStringBuilder<256> output;
    (void)output.append("macos_full_scope_failed stage=");
    (void)output.append(stage);
    (void)output.append(" result=");
    (void)output.append_signed(result);
    const SaccadeSpanU8 detail = saccade_last_error();
    if (detail.data != nullptr && detail.size != 0) {
        (void)output.append(" detail=");
        (void)output.append(std::string_view{reinterpret_cast<const char*>(detail.data), detail.size});
    }
    (void)output.append('\n');
    emit(output.view());
    return exit_code(code);
}

template <size_t Capacity>
bool append_metric(StackStringBuilder<Capacity>* text, std::string_view name, uint64_t value) noexcept {
    return text->append(name) && text->append('=') && text->append_unsigned(value) && text->append(' ');
}

SaccadeResult begin_frames(const DisplaySnapshot& displays, SceneCaptureSet* captures,
                           const saccade::geometry::RectQ8& scope, BridgeSlot* bridge, Samples* samples,
                           uint64_t* started, bool* retry) noexcept {
    *retry = false;
    if (bridge->bridge.busy()) return SACCADE_OK;

    std::array<uint32_t, saccade::geometry::display_capacity> indices{};
    uint32_t display_count = 0;
    for (uint32_t index = 0; index < displays.count; ++index) {
        const saccade::geometry::DisplaySurface& display = displays.displays[index];
        if ((display.flags & saccade::geometry::display_surface_mirrored) != 0 &&
            (display.flags & saccade::geometry::display_surface_main) == 0) {
            continue;
        }
        uint32_t insertion = display_count;
        while (insertion != 0 && displays.displays[indices[insertion - 1U]].display_id > display.display_id) {
            indices[insertion] = indices[insertion - 1U];
            --insertion;
        }
        indices[insertion] = index;
        ++display_count;
    }

    std::array<SceneCaptureFrame, saccade::geometry::display_capacity> capture_frames{};
    std::array<saccade::geometry::DisplaySurface, saccade::geometry::display_capacity> capture_displays{};
    const bool full_refresh = !bridge->bridge.atlas_matches(scope, displays.epoch);
    uint32_t acquired_count = 0;
    for (uint32_t index = 0; index < display_count; ++index) {
        const saccade::geometry::DisplaySurface& display = displays.displays[indices[index]];
        SceneCaptureFrame capture{};
        const SaccadeResult acquired = captures->acquire(display.display_id, &capture);
        if (acquired == SACCADE_OK) {
            capture_frames[acquired_count] = capture;
            capture_displays[acquired_count] = display;
            ++acquired_count;
            continue;
        }
        if (acquired == SACCADE_ERROR_BUSY && !full_refresh) continue;
        for (uint32_t release_index = 0; release_index < acquired_count; ++release_index)
            (void)captures->release(capture_frames[release_index]);
        return acquired == SACCADE_ERROR_BUSY ? SACCADE_OK : acquired;
    }
    if (acquired_count == 0) {
        if (full_refresh) return SACCADE_OK;
        const uint64_t started_ns = monotonic_ns();
        const SaccadeResult result = bridge->bridge.begin_cached(started_ns);
        if (result == SACCADE_OK) bridge->preprocess_started_ns = started_ns;
        return result;
    }

    uint64_t oldest_capture_ns = UINT64_MAX;
    uint64_t newest_capture_ns = 0;
    for (uint32_t index = 0; index < acquired_count; ++index) {
        const uint64_t capture_ns = capture_frames[index].frame.timestamp_ns;
        if (capture_ns == 0) continue;
        oldest_capture_ns = std::min(oldest_capture_ns, capture_ns);
        newest_capture_ns = std::max(newest_capture_ns, capture_ns);
    }
    const uint64_t started_ns = monotonic_ns();
    if (!full_refresh && oldest_capture_ns != UINT64_MAX && started_ns > oldest_capture_ns &&
        started_ns - oldest_capture_ns > maximum_capture_age_ns) {
        for (uint32_t index = 0; index < acquired_count; ++index)
            (void)captures->release(capture_frames[index]);
        ++samples->stale_capture_retries;
        *retry = true;
        return SACCADE_OK;
    }
    if (oldest_capture_ns != UINT64_MAX && samples->capture_count < maximum_samples) {
        samples->capture_age[samples->capture_count] =
            started_ns >= oldest_capture_ns ? started_ns - oldest_capture_ns : 0;
        samples->capture_skew[samples->capture_count] = newest_capture_ns - oldest_capture_ns;
        ++samples->capture_count;
    }
    const SaccadeResult result = bridge->bridge.begin_scope(captures, capture_frames.data(), capture_displays.data(),
                                                            acquired_count, scope, desktop_source_id);
    if (result != SACCADE_OK) {
        for (uint32_t index = 0; index < acquired_count; ++index) {
            (void)captures->release(capture_frames[index]);
        }
        return result;
    }
    bridge->scene_transform_epoch = displays.epoch;
    bridge->preprocess_started_ns = started_ns;
    *started += acquired_count;
    return SACCADE_OK;
}

SaccadeResult begin_cached(BridgeSlot* bridge) noexcept {
    if (bridge->bridge.busy()) return SACCADE_OK;
    const uint64_t started_ns = monotonic_ns();
    const SaccadeResult result = bridge->bridge.begin_cached(started_ns);
    if (result == SACCADE_OK) bridge->preprocess_started_ns = started_ns;
    return result;
}

SaccadeResult poll_frames(BridgeSlot* bridge, DesktopNeuralCoordinator* coordinator, SaccadeRuntimeHandle runtime,
                          Samples* samples, uint64_t* offered) noexcept {
    if (!bridge->initialized || !bridge->bridge.preprocessing()) return SACCADE_OK;
    saccade::scheduler::NeuralFrame neural{};
    bool ready = false;
    SaccadeResult result = bridge->bridge.poll(&neural, &ready);
    if (result != SACCADE_OK || !ready) return result;
    if (bridge->preprocess_started_ns != 0 && samples->preprocess_count < maximum_samples) {
        const uint64_t completed_ns = monotonic_ns();
        samples->preprocess[samples->preprocess_count++] =
            completed_ns >= bridge->preprocess_started_ns ? completed_ns - bridge->preprocess_started_ns : 0;
    }
    bridge->preprocess_started_ns = 0;
    DesktopNeuralFrame frame{};
    static_cast<saccade::scheduler::NeuralFrame&>(frame) = neural;
    frame.scene_transform_epoch = bridge->scene_transform_epoch;
    frame.source_count = 1;
    result = coordinator->offer(frame);
    if (result != SACCADE_OK) {
        (void)saccade_frame_release(runtime, frame.frame);
        frame.retire(frame.retire_context, frame.frame);
        return result;
    }
    ++*offered;
    return SACCADE_OK;
}

bool desktop_scope(const DisplaySnapshot& displays, saccade::geometry::RectQ8* output) noexcept {
    if (displays.count == 0 || output == nullptr) return false;
    int64_t left = INT64_MAX;
    int64_t top = INT64_MAX;
    int64_t right = INT64_MIN;
    int64_t bottom = INT64_MIN;
    for (uint32_t index = 0; index < displays.count; ++index) {
        const saccade::geometry::RectQ8& bounds = displays.displays[index].desktop_bounds;
        left = std::min<int64_t>(left, bounds.x);
        top = std::min<int64_t>(top, bounds.y);
        right = std::max<int64_t>(right, static_cast<int64_t>(bounds.x) + bounds.width);
        bottom = std::max<int64_t>(bottom, static_cast<int64_t>(bounds.y) + bounds.height);
    }
    if (left < INT32_MIN || top < INT32_MIN || right > INT32_MAX || bottom > INT32_MAX || left >= right ||
        top >= bottom) {
        return false;
    }
    *output = {static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<int32_t>(right - left),
               static_cast<int32_t>(bottom - top)};
    return true;
}

uint32_t captured_display_count(const DisplaySnapshot& displays) noexcept {
    uint32_t count = 0;
    for (uint32_t index = 0; index < displays.count; ++index) {
        const uint32_t flags = displays.displays[index].flags;
        if ((flags & saccade::geometry::display_surface_mirrored) == 0 ||
            (flags & saccade::geometry::display_surface_main) != 0) {
            ++count;
        }
    }
    return count;
}

void record(const DesktopNeuralAdvance& advance, Samples* samples) noexcept {
    if (!advance.scene_published) return;
    if (samples->scenes_observed++ == 0) samples->cold_full_scope_ns = advance.full_scope_latency_ns;
    if (samples->scenes_observed <= warmup_scenes || samples->count >= maximum_samples) return;
    samples->batch[samples->count] = advance.batch_latency_ns;
    samples->full_scope[samples->count] = advance.full_scope_latency_ns;
    ++samples->count;
}

uint64_t deadline_misses(const std::array<uint64_t, maximum_samples>& samples, uint32_t count) noexcept {
    uint64_t misses = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (samples[index] > saccade::scheduler::scene_period_30hz_ns) ++misses;
    }
    return misses;
}

void add_memory(const SaccadeMemoryStats& source, SaccadeMemoryStats* total) noexcept {
    total->host_committed += source.host_committed;
    total->host_reserved += source.host_reserved;
    total->device_imported += source.device_imported;
    total->device_owned += source.device_owned;
    total->framework_opaque += source.framework_opaque;
    total->copied_bytes += source.copied_bytes;
    total->high_water_bytes += source.high_water_bytes;
}

uint64_t tracked_current(const SaccadeMemoryStats& memory) noexcept {
    return memory.host_committed + memory.device_imported + memory.device_owned + memory.framework_opaque;
}

uint64_t growth(uint64_t value, uint64_t baseline) noexcept {
    return value > baseline ? value - baseline : 0;
}

SaccadeResult sample_memory(InferenceRuntime* inference, SceneCaptureSet* captures, CoreMlImageBridge* bridge,
                            uint64_t elapsed_ns, uint64_t published_scenes, MemorySamples* samples) noexcept {
    if (samples->count == samples->values.size()) return SACCADE_ERROR_CAPACITY;
    SaccadeMemoryStats inference_memory{};
    inference_memory.struct_size = sizeof(inference_memory);
    inference_memory.api_version = SACCADE_API_VERSION;
    SaccadeMemoryStats capture_memory = inference_memory;
    SaccadeMemoryStats preprocess_memory = inference_memory;
    SaccadeResult result =
        saccade_inference_memory_stats(inference->runtime(), inference->session(), &inference_memory);
    if (result == SACCADE_OK) result = captures->read_memory_stats(&capture_memory);
    if (result == SACCADE_OK) result = bridge->read_memory_stats(&preprocess_memory);
    if (result != SACCADE_OK) return result;

    MemorySample& sample = samples->values[samples->count++];
    sample.elapsed_ns = elapsed_ns;
    sample.published_scenes = published_scenes;
    sample.tracked_current =
        tracked_current(inference_memory) + tracked_current(capture_memory) + tracked_current(preprocess_memory);
    sample.tracked_high_water =
        inference_memory.high_water_bytes + capture_memory.high_water_bytes + preprocess_memory.high_water_bytes;
    sample.copied_bytes = inference_memory.copied_bytes + capture_memory.copied_bytes + preprocess_memory.copied_bytes;
    sample.resident = resident_bytes();
    return SACCADE_OK;
}

MemoryTrend analyze_memory(const MemorySamples& samples, uint64_t maximum_output_bytes) noexcept {
    MemoryTrend trend{};
    uint32_t baseline_index = samples.count;
    for (uint32_t index = 0; index < samples.count; ++index) {
        if (samples.values[index].elapsed_ns >= memory_warmup_ns) {
            baseline_index = index;
            break;
        }
    }
    if (baseline_index == samples.count || baseline_index + 1U >= samples.count) return trend;

    trend.baseline = samples.values[baseline_index];
    trend.final = samples.values[samples.count - 1U];
    for (uint32_t index = baseline_index; index < samples.count; ++index) {
        trend.tracked_high_water_growth =
            std::max(trend.tracked_high_water_growth,
                     growth(samples.values[index].tracked_high_water, trend.baseline.tracked_high_water));
        trend.copied_byte_growth =
            std::max(trend.copied_byte_growth, growth(samples.values[index].copied_bytes, trend.baseline.copied_bytes));
        trend.resident_peak_growth =
            std::max(trend.resident_peak_growth, growth(samples.values[index].resident, trend.baseline.resident));
    }
    trend.resident_final_growth = growth(trend.final.resident, trend.baseline.resident);
    const uint64_t scenes_after_baseline = growth(trend.final.published_scenes, trend.baseline.published_scenes);
    trend.copied_byte_limit = (scenes_after_baseline + 1U) * maximum_output_bytes;
    trend.available = true;
    trend.qualified = trend.tracked_high_water_growth <= maximum_tracked_high_water_growth &&
                      trend.copied_byte_growth <= trend.copied_byte_limit &&
                      trend.resident_final_growth <= maximum_resident_final_growth &&
                      trend.resident_peak_growth <= maximum_resident_peak_growth;
    return trend;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 4 || argc > 6) return exit_code(ExitCode::usage);
        uint32_t duration_seconds = default_duration_seconds;
        if (argc >= 5 && !parse_duration(argv[4], &duration_seconds)) return exit_code(ExitCode::usage);
        RunMode mode = RunMode::live;
        if (argc == 6 && !parse_mode(argv[5], &mode)) return exit_code(ExitCode::usage);
        if (mode != RunMode::live && duration_seconds < minimum_soak_duration_seconds) {
            return exit_code(ExitCode::usage);
        }
        if (!CGPreflightScreenCaptureAccess()) return exit_code(ExitCode::permission);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) return exit_code(ExitCode::device);
        void* metal_device = (__bridge void*)device;

        const saccade::model::ArtifactVerifier verifier{nullptr, trust_benchmark_artifact};
        static MappedArtifact artifact;
        SaccadeResult result = artifact.initialize(argv[1], verifier);
        if (result != SACCADE_OK) return fail(ExitCode::artifact, "artifact", result);

        static CoreMlInferenceProvider provider;
        CoreMlProviderConfig provider_config{};
        provider_config.model_root = argv[2];
        provider_config.compute_policy = CoreMlComputePolicy::cpu_and_neural_engine;
        provider_config.verifier = verifier;
        result = provider.initialize(provider_config);
        if (result != SACCADE_OK) return fail(ExitCode::provider, "provider", result);

        static InferenceRuntime inference;
        const SaccadeInferenceProviderDesc provider_desc = provider.descriptor();
        InferenceRuntimeConfig inference_config{};
        inference_config.provider = provider_desc;
        inference_config.artifact = artifact.bytes();
        inference_config.model_stable_id = artifact.view().stable_id;
        inference_config.provider_stable_id = provider_desc.info.stable_id;
        inference_config.required_capability_bits =
            SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC;
        inference_config.preferred_capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU |
                                                     SACCADE_PROVIDER_CAPABILITY_ACCELERATOR |
                                                     SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
        inference_config.required_format_bits = SACCADE_FORMAT_BGRA8;
        inference_config.required_precision_bits = artifact.view().precision_bits;
        inference_config.required_import_bits = SACCADE_IMPORT_IOSURFACE;
        result = inference.initialize(inference_config);
        if (result != SACCADE_OK) return fail(ExitCode::inference, "inference", result);

        static ScreenCaptureProvider capture_provider;
        static SceneCaptureSet captures;
        static DisplayCatalog displays;
        static DisplayCollector display_collector;
        result = capture_provider.initialize(metal_device);
        if (result != SACCADE_OK) return fail(ExitCode::capture, "capture_provider", result);
        result = display_collector.refresh(&displays);
        if (result != SACCADE_OK || displays.snapshot().count == 0)
            return fail(ExitCode::topology, "topology", result == SACCADE_OK ? SACCADE_ERROR_NOT_FOUND : result);
        result = captures.initialize(&capture_provider, 0, 0);
        if (result == SACCADE_OK) result = captures.synchronize(displays.snapshot());
        if (result == SACCADE_OK) result = captures.set_running(true);
        if (result != SACCADE_OK) return fail(ExitCode::capture, "capture", result);

        saccade::geometry::RectQ8 scope{};
        if (!desktop_scope(displays.snapshot(), &scope)) return exit_code(ExitCode::topology);
        static BridgeSlot bridge;
        CoreMlImageBridgeConfig bridge_config{};
        bridge_config.runtime = inference.runtime();
        bridge_config.metal_device = metal_device;
        bridge_config.metallib_path = argv[3];
        bridge_config.input_width = artifact.view().input_width;
        bridge_config.input_height = artifact.view().input_height;
        saccade::model::coreml::Contract contract{};
        result = saccade::model::coreml::parse_contract(artifact.view(), &contract);
        if (result != SACCADE_OK) return fail(ExitCode::bridge, "bridge contract", result);
        bridge_config.letterbox_rgb = contract.letterbox_rgb;
        result = bridge.bridge.initialize(bridge_config);
        if (result != SACCADE_OK) return fail(ExitCode::bridge, "bridge", result);
        bridge.initialized = true;

        static saccade::scene::SceneStoreStorage scene_storage;
        static DesktopNeuralCoordinatorStorage coordinator_storage;
        static saccade::scene::SceneStore scenes;
        static DesktopNeuralCoordinator coordinator;
        result = scenes.initialize(&scene_storage);
        if (result != SACCADE_OK) return fail(ExitCode::scene_store, "scene_store", result);
        DesktopNeuralCoordinatorConfig coordinator_config{};
        coordinator_config.runtime = inference.runtime();
        coordinator_config.session = inference.session();
        coordinator_config.model_epoch = artifact.view().stable_id;
        coordinator_config.session_epoch = session_epoch;
        coordinator_config.desktop_source_id = desktop_source_id;
        coordinator_config.maximum_output_bytes = inference.info().max_output_bytes;
        coordinator_config.maximum_targets = artifact.view().max_targets;
        coordinator_config.start_time_ns = monotonic_ns();
        result = coordinator.initialize(coordinator_config, &coordinator_storage, &scenes);
        if (result != SACCADE_OK) return fail(ExitCode::coordinator, "coordinator", result);

        static Samples samples;
        const uint64_t duration_ns = static_cast<uint64_t>(duration_seconds) * UINT64_C(1'000'000'000);
        const uint64_t run_started = monotonic_ns();
        uint64_t next_capture_ns = run_started;
        uint64_t capture_cycles = 0;
        uint64_t frames_started = 0;
        uint64_t frames_offered = 0;
        uint64_t next_memory_sample_ns = run_started;
        static MemorySamples memory_samples;
        SaccadeResult run_result = SACCADE_OK;
        while (run_result == SACCADE_OK) {
            const uint64_t now_ns = monotonic_ns();
            if (now_ns - run_started >= duration_ns) break;
            if (now_ns >= next_memory_sample_ns) {
                run_result = sample_memory(&inference, &captures, &bridge.bridge, now_ns - run_started,
                                           coordinator.stats().batches_published, &memory_samples);
                do {
                    next_memory_sample_ns += memory_sample_period_ns;
                } while (next_memory_sample_ns <= now_ns);
            }
            if (run_result == SACCADE_OK && now_ns >= next_capture_ns) {
                bool retry = false;
                if (mode == RunMode::stable_cached && bridge.bridge.atlas_matches(scope, displays.snapshot().epoch)) {
                    run_result = begin_cached(&bridge);
                } else {
                    run_result =
                        begin_frames(displays.snapshot(), &captures, scope, &bridge, &samples, &frames_started, &retry);
                }
                ++capture_cycles;
                next_capture_ns = retry ? now_ns + stale_capture_retry_ns
                                        : next_capture_deadline(now_ns, coordinator_config.start_time_ns);
            }
            if (run_result == SACCADE_OK)
                run_result = poll_frames(&bridge, &coordinator, inference.runtime(), &samples, &frames_offered);
            DesktopNeuralAdvance advance{};
            if (run_result == SACCADE_OK) run_result = coordinator.advance(now_ns, &advance);
            record(advance, &samples);
            (void)sched_yield();
        }

        const uint64_t drain_deadline = monotonic_ns() + drain_timeout_ns;
        while (run_result == SACCADE_OK &&
               (bridge.bridge.busy() || coordinator.stats().batches_published < coordinator.stats().batches_started) &&
               monotonic_ns() < drain_deadline) {
            run_result = poll_frames(&bridge, &coordinator, inference.runtime(), &samples, &frames_offered);
            DesktopNeuralAdvance advance{};
            if (run_result == SACCADE_OK) run_result = coordinator.advance(monotonic_ns(), &advance);
            record(advance, &samples);
            (void)sched_yield();
        }
        if (captures.set_running(false) != SACCADE_OK && run_result == SACCADE_OK) run_result = SACCADE_ERROR_BACKEND;
        const uint64_t run_ended = monotonic_ns();
        if (run_result != SACCADE_OK || run_ended <= run_started || bridge.bridge.busy()) {
            return exit_code(ExitCode::run);
        }
        if (sample_memory(&inference, &captures, &bridge.bridge, run_ended - run_started,
                          coordinator.stats().batches_published, &memory_samples) != SACCADE_OK) {
            return exit_code(ExitCode::memory);
        }

        SaccadeMemoryStats inference_memory{};
        inference_memory.struct_size = sizeof(inference_memory);
        inference_memory.api_version = SACCADE_API_VERSION;
        SaccadeMemoryStats capture_memory = inference_memory;
        SaccadeMemoryStats preprocess_memory = inference_memory;
        if (saccade_inference_memory_stats(inference.runtime(), inference.session(), &inference_memory) != SACCADE_OK ||
            captures.read_memory_stats(&capture_memory) != SACCADE_OK) {
            return exit_code(ExitCode::memory);
        }
        SaccadeMemoryStats memory{};
        memory.struct_size = sizeof(memory);
        memory.api_version = SACCADE_API_VERSION;
        if (bridge.bridge.read_memory_stats(&memory) != SACCADE_OK) return exit_code(ExitCode::memory);
        add_memory(memory, &preprocess_memory);

        const auto coordinator_stats = coordinator.stats();
        const auto scheduler_stats = coordinator.scheduler_stats();
        saccade::platform::macos::SceneCaptureStats capture_stats{};
        (void)captures.read_stats(&capture_stats);
        const uint64_t batch_misses = deadline_misses(samples.batch, samples.count);
        const uint64_t full_scope_misses = deadline_misses(samples.full_scope, samples.count);
        const uint64_t batch_p50 = percentile(&samples.batch, samples.count, 50);
        const uint64_t batch_p95 = percentile(&samples.batch, samples.count, 95);
        const uint64_t batch_p99 = percentile(&samples.batch, samples.count, 99);
        const uint64_t full_p50 = percentile(&samples.full_scope, samples.count, 50);
        const uint64_t full_p95 = percentile(&samples.full_scope, samples.count, 95);
        const uint64_t full_p99 = percentile(&samples.full_scope, samples.count, 99);
        const uint64_t capture_age_p50 = percentile(&samples.capture_age, samples.capture_count, 50);
        const uint64_t capture_age_p95 = percentile(&samples.capture_age, samples.capture_count, 95);
        const uint64_t capture_age_p99 = percentile(&samples.capture_age, samples.capture_count, 99);
        const uint64_t capture_skew_p95 = percentile(&samples.capture_skew, samples.capture_count, 95);
        const uint64_t preprocess_p50 = percentile(&samples.preprocess, samples.preprocess_count, 50);
        const uint64_t preprocess_p95 = percentile(&samples.preprocess, samples.preprocess_count, 95);
        const uint64_t preprocess_p99 = percentile(&samples.preprocess, samples.preprocess_count, 99);
        const uint64_t measured_ns = run_ended - run_started;
        const uint64_t refresh_millihz =
            coordinator_stats.batches_published * UINT64_C(1'000'000'000'000) / measured_ns;
        const uint64_t measured_deadline_misses = std::max(batch_misses, full_scope_misses);
        const uint64_t deadline_miss_ppm =
            samples.count == 0 ? UINT64_MAX : measured_deadline_misses * UINT64_C(1'000'000) / samples.count;
        const bool latency_qualified =
            refresh_millihz >= minimum_refresh_millihz && batch_p95 <= saccade::scheduler::scene_period_30hz_ns &&
            full_p95 <= saccade::scheduler::scene_period_30hz_ns && deadline_miss_ppm <= maximum_deadline_miss_ppm;
        const uint32_t expected_stable_frames = captured_display_count(displays.snapshot());
        const bool stable_admitted =
            mode != RunMode::stable_cached || (frames_started != 0 && frames_started == expected_stable_frames);
        const bool continuous_admitted =
            mode != RunMode::continuous_change || frames_started >= coordinator_stats.batches_published;
        const bool mode_admitted = stable_admitted && continuous_admitted;
        const MemoryTrend memory_trend = analyze_memory(memory_samples, inference.info().max_output_bytes);
        const bool memory_required = mode != RunMode::live;
        const bool qualified = latency_qualified && mode_admitted && (!memory_required || memory_trend.qualified);

        StackStringBuilder<4096> output;
        const bool written =
            output.append("macos_full_scope mode=") && output.append(mode_name(mode)) && output.append(' ') &&
            append_metric(&output, "duration_ns", measured_ns) &&
            append_metric(&output, "displays", displays.snapshot().count) &&
            append_metric(&output, "capture_cycles", capture_cycles) &&
            append_metric(&output, "frames_started", frames_started) &&
            append_metric(&output, "frames_offered", frames_offered) &&
            append_metric(&output, "scenes", coordinator_stats.batches_published) &&
            append_metric(&output, "warmup_scenes", warmup_scenes) &&
            append_metric(&output, "measured_scenes", samples.count) &&
            append_metric(&output, "refresh_millihz", refresh_millihz) &&
            append_metric(&output, "batch_p50_ns", batch_p50) && append_metric(&output, "batch_p95_ns", batch_p95) &&
            append_metric(&output, "batch_p99_ns", batch_p99) &&
            append_metric(&output, "full_scope_p50_ns", full_p50) &&
            append_metric(&output, "full_scope_p95_ns", full_p95) &&
            append_metric(&output, "full_scope_p99_ns", full_p99) &&
            append_metric(&output, "capture_age_p50_ns", capture_age_p50) &&
            append_metric(&output, "capture_age_p95_ns", capture_age_p95) &&
            append_metric(&output, "capture_age_p99_ns", capture_age_p99) &&
            append_metric(&output, "capture_skew_p95_ns", capture_skew_p95) &&
            append_metric(&output, "preprocess_p50_ns", preprocess_p50) &&
            append_metric(&output, "preprocess_p95_ns", preprocess_p95) &&
            append_metric(&output, "preprocess_p99_ns", preprocess_p99) &&
            append_metric(&output, "cold_full_scope_ns", samples.cold_full_scope_ns) &&
            append_metric(&output, "batch_misses", batch_misses) &&
            append_metric(&output, "full_scope_misses", full_scope_misses) &&
            append_metric(&output, "raw_batch_misses", coordinator_stats.batch_deadlines_missed) &&
            append_metric(&output, "raw_full_scope_misses", coordinator_stats.full_scope_deadlines_missed) &&
            append_metric(&output, "deadline_miss_ppm", deadline_miss_ppm) &&
            append_metric(&output, "interaction_ticks", scheduler_stats.interaction_ticks) &&
            append_metric(&output, "interaction_skipped", scheduler_stats.interaction_skipped) &&
            append_metric(&output, "capture_empty", capture_stats.empty_acquires) &&
            append_metric(&output, "stale_capture_retries", samples.stale_capture_retries) &&
            append_metric(&output, "targets", coordinator_stats.targets_published) &&
            append_metric(&output, "inference_high_water", inference_memory.high_water_bytes) &&
            append_metric(&output, "preprocess_high_water", preprocess_memory.high_water_bytes) &&
            append_metric(&output, "capture_high_water", capture_memory.high_water_bytes) &&
            append_metric(&output, "resident_bytes", resident_bytes()) &&
            append_metric(&output, "memory_samples", memory_samples.count) &&
            append_metric(&output, "memory_baseline_elapsed_ns", memory_trend.baseline.elapsed_ns) &&
            append_metric(&output, "memory_baseline_tracked_current", memory_trend.baseline.tracked_current) &&
            append_metric(&output, "memory_final_tracked_current", memory_trend.final.tracked_current) &&
            append_metric(&output, "memory_baseline_tracked_high_water", memory_trend.baseline.tracked_high_water) &&
            append_metric(&output, "memory_final_tracked_high_water", memory_trend.final.tracked_high_water) &&
            append_metric(&output, "tracked_high_water_growth", memory_trend.tracked_high_water_growth) &&
            append_metric(&output, "copied_byte_growth", memory_trend.copied_byte_growth) &&
            append_metric(&output, "copied_byte_limit", memory_trend.copied_byte_limit) &&
            append_metric(&output, "memory_baseline_resident", memory_trend.baseline.resident) &&
            append_metric(&output, "memory_final_resident", memory_trend.final.resident) &&
            append_metric(&output, "resident_final_growth", memory_trend.resident_final_growth) &&
            append_metric(&output, "resident_peak_growth", memory_trend.resident_peak_growth) &&
            append_metric(&output, "maximum_resident_final_growth", maximum_resident_final_growth) &&
            append_metric(&output, "maximum_resident_peak_growth", maximum_resident_peak_growth) &&
            append_metric(&output, "memory_evidence", memory_trend.available ? 1U : 0U) &&
            append_metric(&output, "memory_qualified", memory_trend.qualified ? 1U : 0U) &&
            append_metric(&output, "mode_admitted", mode_admitted ? 1U : 0U) &&
            append_metric(&output, "latency_qualified", latency_qualified ? 1U : 0U) &&
            append_metric(&output, "qualified", qualified ? 1U : 0U) && output.append('\n');
        if (!written) return exit_code(ExitCode::run);
        emit(output.view());

        SaccadeResult cleanup = coordinator.shutdown();
        const SaccadeResult bridge_stopped = bridge.bridge.shutdown();
        if (bridge_stopped != SACCADE_OK && cleanup == SACCADE_OK) cleanup = bridge_stopped;
        bridge.initialized = false;
        if (captures.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
        if (inference.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
        if (provider.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
        if (artifact.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
        if (cleanup != SACCADE_OK) return exit_code(ExitCode::cleanup);
        return qualified ? exit_code(ExitCode::success) : exit_code(ExitCode::qualification);
    }
}

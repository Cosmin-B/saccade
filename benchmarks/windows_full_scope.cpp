#include "application/inference_runtime.hpp"
#include "backends/d3d12/directml_provider.hpp"
#include "core/stack_string_builder.hpp"
#include "model/mapped_artifact.hpp"
#include "platform/windows/display_topology.hpp"
#include "platform/windows/neural_bridge.hpp"
#include "platform/windows/runtime_scheduling.hpp"
#include "platform/windows/scene_capture.hpp"
#include "platform/windows/screen_capture.hpp"
#include "scene/store.hpp"
#include "scheduler/desktop_neural_coordinator.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace {

using saccade::application::InferenceRuntime;
using saccade::application::InferenceRuntimeConfig;
using saccade::backend::d3d12::DirectMlExecutionPolicy;
using saccade::backend::d3d12::DirectMlInferenceProvider;
using saccade::backend::d3d12::DirectMlPipelineStats;
using saccade::backend::d3d12::DirectMlProviderConfig;
using saccade::core::StackStringBuilder;
using saccade::geometry::CoordinateSpace;
using saccade::geometry::CoordinateTransform;
using saccade::geometry::DisplayCatalog;
using saccade::geometry::DisplaySnapshot;
using saccade::geometry::DisplaySurface;
using saccade::geometry::TransformDesc;
using saccade::model::ArtifactView;
using saccade::model::MappedArtifact;
using saccade::platform::windows::DisplayCollector;
using saccade::platform::windows::NeuralBridge;
using saccade::platform::windows::NeuralBridgeConfig;
using saccade::platform::windows::RuntimeScheduling;
using saccade::platform::windows::SceneCaptureFrame;
using saccade::platform::windows::SceneCaptureSet;
using saccade::platform::windows::ScreenCaptureProvider;
using saccade::scheduler::DesktopNeuralAdvance;
using saccade::scheduler::DesktopNeuralCoordinator;
using saccade::scheduler::DesktopNeuralCoordinatorConfig;
using saccade::scheduler::DesktopNeuralCoordinatorStats;
using saccade::scheduler::DesktopNeuralCoordinatorStorage;
using saccade::scheduler::DesktopNeuralFrame;
using saccade::scheduler::DualRateStats;

constexpr uint32_t default_duration_seconds = 10;
constexpr uint32_t maximum_duration_seconds = 300;
constexpr uint32_t maximum_samples = maximum_duration_seconds * 60;
constexpr uint64_t session_epoch = 1;
constexpr uint64_t desktop_source_id = UINT64_C(0x5341434341444501);
constexpr uint64_t drain_timeout_ns = UINT64_C(5'000'000'000);
constexpr uint64_t capture_start_timeout_ns = UINT64_C(5'000'000'000);
constexpr uint64_t minimum_refresh_millihz = UINT64_C(29'000);
constexpr uint64_t minimum_interaction_millihz = UINT64_C(115'000);
constexpr uint64_t maximum_deadline_miss_ppm = UINT64_C(1'000);
constexpr uint32_t warmup_scene_count = 4;

enum class BenchmarkMode : uint8_t { live, replay };

enum class ExitCode : int {
    success,
    usage,
    timing,
    scheduling,
    artifact,
    provider,
    inference,
    capture,
    topology,
    scene_store,
    coordinator,
    run,
    memory,
    qualification,
    cleanup
};

struct Samples {
    std::array<uint64_t, maximum_samples> batch{};
    std::array<uint64_t, maximum_samples> full_scope{};
    std::array<uint64_t, maximum_samples> capture_age{};
    std::array<uint64_t, maximum_samples> import{};
    std::array<uint64_t, maximum_samples> preprocess{};
    std::array<uint64_t, maximum_samples> inference{};
    std::array<uint64_t, maximum_samples> postprocess{};
    uint32_t count = 0;
};

struct ReplayFrames {
    std::array<SceneCaptureFrame, saccade::geometry::display_capacity> frames{};
    uint32_t count = 0;
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
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

bool parse_mode(const char* text, BenchmarkMode* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const std::string_view value{text};
    if (value == "live") {
        *output = BenchmarkMode::live;
        return true;
    }
    if (value == "replay") {
        *output = BenchmarkMode::replay;
        return true;
    }
    return false;
}

std::string_view mode_name(BenchmarkMode mode) noexcept {
    return mode == BenchmarkMode::live ? "live" : "replay";
}

uint64_t monotonic_ns() noexcept {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0 && QueryPerformanceFrequency(&frequency) == 0) return 0;
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0) return 0;
    const uint64_t whole = static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart);
    const uint64_t remainder = static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart);
    return whole * UINT64_C(1'000'000'000) +
           remainder * UINT64_C(1'000'000'000) / static_cast<uint64_t>(frequency.QuadPart);
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

void record_sample(Samples* samples, const DesktopNeuralAdvance& advance, bool profile_stages,
                   const DirectMlInferenceProvider& provider, DirectMlPipelineStats* previous) noexcept {
    if (!advance.scene_published || samples->count >= maximum_samples) return;
    const uint32_t index = samples->count;
    samples->batch[index] = advance.batch_latency_ns;
    samples->full_scope[index] = advance.full_scope_latency_ns;
    samples->capture_age[index] = advance.full_scope_latency_ns >= advance.batch_latency_ns
                                      ? advance.full_scope_latency_ns - advance.batch_latency_ns
                                      : 0;
    if (profile_stages) {
        const DirectMlPipelineStats current = provider.pipeline_stats();
        samples->import[index] = current.import_ns - previous->import_ns;
        samples->preprocess[index] = current.preprocess_ns - previous->preprocess_ns;
        samples->inference[index] = current.inference_ns - previous->inference_ns;
        samples->postprocess[index] = current.postprocess_ns - previous->postprocess_ns;
        *previous = current;
    }
    ++samples->count;
}

void emit(std::string_view text) noexcept {
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

int fail(ExitCode code, std::string_view stage, SaccadeResult result) noexcept {
    StackStringBuilder<256> output;
    (void)output.append("windows_full_scope_failed stage=");
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

bool append_metric(StackStringBuilder<2048>* text, std::string_view name, uint64_t value) noexcept {
    return text->append(name) && text->append('=') && text->append_unsigned(value) && text->append(' ');
}

bool q8(uint32_t value, int32_t* output) noexcept {
    if (value > (static_cast<uint32_t>(INT32_MAX) >> 8U)) return false;
    *output = static_cast<int32_t>(value << 8U);
    return true;
}

SaccadeResult release_replay_frames(SceneCaptureSet*, ReplayFrames*) noexcept;

SaccadeResult acquire_replay_frames(const DisplaySnapshot& displays, SceneCaptureSet* captures,
                                    ReplayFrames* output) noexcept {
    const uint64_t deadline_ns = monotonic_ns() + capture_start_timeout_ns;
    for (uint32_t index = 0; index < displays.count; ++index) {
        SaccadeResult result = SACCADE_ERROR_BUSY;
        while (result == SACCADE_ERROR_BUSY && monotonic_ns() < deadline_ns) {
            result = captures->acquire(displays.displays[index].display_id, &output->frames[index]);
            if (result == SACCADE_ERROR_BUSY) Sleep(1);
        }
        if (result != SACCADE_OK) {
            (void)release_replay_frames(captures, output);
            return result;
        }
        ++output->count;
    }
    return SACCADE_OK;
}

SaccadeResult release_replay_frames(SceneCaptureSet* captures, ReplayFrames* replay) noexcept {
    SaccadeResult result = SACCADE_OK;
    while (replay->count != 0) {
        --replay->count;
        const SaccadeResult released = captures->release(replay->frames[replay->count]);
        if (released != SACCADE_OK && result == SACCADE_OK) result = released;
    }
    return result;
}

SaccadeResult import_replay_frame(const SceneCaptureFrame& capture, const DisplaySurface& display,
                                  uint64_t scene_transform_epoch, uint64_t frame_id, uint64_t capture_time_ns,
                                  SaccadeRuntimeHandle runtime, DesktopNeuralFrame* output) noexcept {
    TransformDesc transform{};
    if (!q8(capture.frame.width, &transform.source.width) || !q8(capture.frame.height, &transform.source.height)) {
        return SACCADE_ERROR_CAPACITY;
    }
    transform.destination = display.desktop_bounds;
    transform.epoch = capture.frame.transform_epoch;
    transform.source_space = CoordinateSpace::capture;
    transform.destination_space = CoordinateSpace::desktop;
    CoordinateTransform source_to_desktop;
    SaccadeResult result = source_to_desktop.initialize(transform);
    if (result != SACCADE_OK) return result;

    SaccadeWin32CaptureFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.texture = capture.native.d3d11_texture;
    frame.subresource = capture.native.subresource;
    frame.pixel_format = capture.native.pixel_format;
    frame.width = capture.frame.width;
    frame.height = capture.frame.height;
    frame.frame_id = frame_id;
    frame.transform_epoch = capture.frame.transform_epoch;
    result = saccade_frame_import_win32_capture(runtime, &frame, &output->frame);
    if (result != SACCADE_OK) return result;

    output->source_id = capture.frame.source_id;
    output->topology_epoch = capture.topology_epoch;
    output->transform_epoch = capture.frame.transform_epoch;
    output->capture_time_ns = capture_time_ns;
    output->scene_transform_epoch = scene_transform_epoch;
    output->width = frame.width;
    output->height = frame.height;
    output->source_to_desktop = source_to_desktop;
    return SACCADE_OK;
}

SaccadeResult offer_replay_frames(const DisplaySnapshot& displays, const ReplayFrames& replay,
                                  DesktopNeuralCoordinator* coordinator, SaccadeRuntimeHandle runtime,
                                  uint64_t capture_time_ns, uint64_t* next_frame_id, uint64_t* offered) noexcept {
    for (uint32_t index = 0; index < replay.count; ++index) {
        DesktopNeuralFrame frame{};
        SaccadeResult result = import_replay_frame(replay.frames[index], displays.displays[index], displays.epoch,
                                                   (*next_frame_id)++, capture_time_ns, runtime, &frame);
        if (result != SACCADE_OK) return result;
        frame.source_count = replay.count;
        result = coordinator->offer(frame);
        if (result != SACCADE_OK) {
            if (result != SACCADE_ERROR_STALE_HANDLE) {
                (void)saccade_frame_release(runtime, frame.frame);
                if (frame.retire != nullptr) frame.retire(frame.retire_context, frame.frame);
            }
            return result;
        }
        ++*offered;
    }
    return SACCADE_OK;
}

SaccadeResult offer_frames(const DisplaySnapshot& displays, SceneCaptureSet* captures, NeuralBridge* bridge,
                           DesktopNeuralCoordinator* coordinator, SaccadeRuntimeHandle runtime,
                           uint64_t* offered) noexcept {
    for (uint32_t index = 0; index < displays.count; ++index) {
        const auto& display = displays.displays[index];
        SceneCaptureFrame capture{};
        SaccadeResult result = captures->acquire(display.display_id, &capture);
        if (result == SACCADE_ERROR_BUSY) continue;
        if (result != SACCADE_OK) return result;

        DesktopNeuralFrame frame{};
        result = bridge->import(captures, capture, display, displays.epoch, &frame);
        if (result != SACCADE_OK) {
            (void)captures->release(capture);
            return result;
        }
        frame.source_count = displays.count;
        if (frame.capture_time_ns == 0) frame.capture_time_ns = monotonic_ns();
        result = coordinator->offer(frame);
        if (result != SACCADE_OK) {
            if (result != SACCADE_ERROR_STALE_HANDLE) {
                (void)saccade_frame_release(runtime, frame.frame);
                if (frame.retire != nullptr) frame.retire(frame.retire_context, frame.frame);
            }
            return result;
        }
        ++*offered;
    }
    return SACCADE_OK;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) return exit_code(ExitCode::usage);
    uint32_t duration_seconds = default_duration_seconds;
    if (argc >= 4 && !parse_duration(argv[3], &duration_seconds)) return exit_code(ExitCode::usage);
    BenchmarkMode mode = BenchmarkMode::live;
    if (argc == 5 && !parse_mode(argv[4], &mode)) return exit_code(ExitCode::usage);

    const DPI_AWARENESS_CONTEXT previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RuntimeScheduling scheduling;
    SaccadeResult result = scheduling.initialize();
    if (result != SACCADE_OK) return fail(ExitCode::scheduling, "scheduling", result);
    const uint64_t initialized_at = monotonic_ns();
    if (initialized_at == 0) return exit_code(ExitCode::timing);
    const saccade::model::ArtifactVerifier verifier{nullptr, trust_benchmark_artifact};
    static MappedArtifact artifact;
    result = artifact.initialize(argv[1], verifier);
    if (result != SACCADE_OK) return fail(ExitCode::artifact, "artifact", result);

    static DirectMlInferenceProvider provider;
    const bool profile_stages = GetEnvironmentVariableW(L"SACCADE_PROFILE_WINDOWS_PIPELINE", nullptr, 0) != 0;
    const DirectMlProviderConfig provider_config{argv[2], verifier, DirectMlExecutionPolicy::hardware_only,
                                                 profile_stages};
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
        SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_GPU;
    inference_config.preferred_capability_bits = SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    inference_config.required_format_bits = SACCADE_FORMAT_BGRA8;
    inference_config.required_precision_bits = artifact.view().precision_bits;
    inference_config.required_import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    result = inference.initialize(inference_config);
    if (result != SACCADE_OK) {
        StackStringBuilder<128> stage;
        (void)stage.append("directml_model_stage=");
        (void)stage.append_unsigned(static_cast<uint32_t>(provider.model_stage()));
        (void)stage.append('\n');
        emit(stage.view());
        return fail(ExitCode::inference, "inference", result);
    }
    static ScreenCaptureProvider capture_provider;
    static SceneCaptureSet captures;
    static DisplayCatalog displays;
    static DisplayCollector display_collector;
    result = mode == BenchmarkMode::live ? capture_provider.initialize_native(provider.adapter_luid())
                                         : capture_provider.initialize(provider.capture_device());
    if (result != SACCADE_OK) return fail(ExitCode::capture, "capture_provider", result);
    result = display_collector.refresh(&displays);
    if (result != SACCADE_OK || displays.snapshot().count == 0)
        return fail(ExitCode::topology, "topology", result == SACCADE_OK ? SACCADE_ERROR_NOT_FOUND : result);
    result = captures.initialize(&capture_provider, 0, 0);
    if (result != SACCADE_OK) return fail(ExitCode::capture, "capture_set", result);
    result = captures.synchronize(displays.snapshot());
    if (result != SACCADE_OK) return fail(ExitCode::capture, "capture_synchronize", result);
    result = captures.set_running(true);
    if (result != SACCADE_OK) return fail(ExitCode::capture, "capture_start", result);

    static NeuralBridge bridge;
    if (mode == BenchmarkMode::live) {
        const NeuralBridgeConfig bridge_config{inference.runtime(), capture_provider.device(),
                                               capture_provider.context(), provider.graphics_device()};
        result = bridge.initialize(bridge_config);
    } else {
        result = bridge.initialize(inference.runtime());
    }
    if (result != SACCADE_OK) return fail(ExitCode::capture, "bridge", result);

    static saccade::scene::SceneStoreStorage scene_storage;
    static DesktopNeuralCoordinatorStorage coordinator_storage;
    static saccade::scene::SceneStore scenes;
    static DesktopNeuralCoordinator coordinator;
    DesktopNeuralCoordinatorConfig coordinator_config{};
    coordinator_config.runtime = inference.runtime();
    coordinator_config.session = inference.session();
    coordinator_config.model_epoch = artifact.view().stable_id;
    coordinator_config.session_epoch = session_epoch;
    coordinator_config.desktop_source_id = desktop_source_id;
    coordinator_config.maximum_output_bytes = inference.info().max_output_bytes;
    coordinator_config.maximum_targets = artifact.view().max_targets;
    coordinator_config.start_time_ns = monotonic_ns();
    result = scenes.initialize(&scene_storage);
    if (result != SACCADE_OK) return fail(ExitCode::scene_store, "scene_store", result);
    result = coordinator.initialize(coordinator_config, &coordinator_storage, &scenes);
    if (result != SACCADE_OK) return fail(ExitCode::coordinator, "coordinator", result);

    static Samples samples;
    static ReplayFrames replay;
    if (mode == BenchmarkMode::replay) {
        result = acquire_replay_frames(displays.snapshot(), &captures, &replay);
        if (result != SACCADE_OK) return fail(ExitCode::capture, "replay_capture", result);
    }
    uint64_t next_frame_id = 1;
    uint64_t frames_offered = 0;
    uint64_t capture_cycles = 0;
    uint64_t cold_batch_latency_ns = 0;
    uint64_t cold_full_scope_latency_ns = 0;
    SaccadeResult run_result = SACCADE_OK;

    const uint64_t warmup_started = monotonic_ns();
    const uint64_t warmup_deadline = warmup_started + capture_start_timeout_ns;
    uint64_t next_capture_ns = warmup_started;
    while (run_result == SACCADE_OK && coordinator.stats().batches_published < warmup_scene_count &&
           monotonic_ns() < warmup_deadline) {
        const uint64_t now_ns = monotonic_ns();
        if (now_ns == 0) {
            run_result = SACCADE_ERROR_BACKEND;
            break;
        }
        if (now_ns >= next_capture_ns) {
            run_result = mode == BenchmarkMode::live
                             ? offer_frames(displays.snapshot(), &captures, &bridge, &coordinator, inference.runtime(),
                                            &frames_offered)
                             : offer_replay_frames(displays.snapshot(), replay, &coordinator, inference.runtime(),
                                                   now_ns, &next_frame_id, &frames_offered);
            ++capture_cycles;
            const uint64_t elapsed = now_ns - next_capture_ns;
            next_capture_ns += (elapsed / saccade::scheduler::interaction_period_120hz_ns + 1U) *
                               saccade::scheduler::interaction_period_120hz_ns;
        }

        DesktopNeuralAdvance advance{};
        if (run_result == SACCADE_OK) run_result = coordinator.advance(now_ns, &advance);
        if (advance.scene_published && cold_full_scope_latency_ns == 0) {
            cold_batch_latency_ns = advance.batch_latency_ns;
            cold_full_scope_latency_ns = advance.full_scope_latency_ns;
        }
        (void)SwitchToThread();
    }
    if (run_result != SACCADE_OK || coordinator.stats().batches_published < warmup_scene_count) {
        int32_t capture_error = 0;
        (void)capture_provider.read_last_native_error(&capture_error);
        saccade::platform::windows::SceneCaptureStats warmup_capture{};
        (void)captures.read_stats(&warmup_capture);
        const auto warmup_bridge = bridge.stats();
        const auto transfer_stats = bridge.transfer_stats();
        StackStringBuilder<512> warmup_failure;
        (void)warmup_failure.append("windows_full_scope_warmup_failed result=");
        (void)warmup_failure.append_signed(run_result);
        (void)warmup_failure.append(" scenes=");
        (void)warmup_failure.append_unsigned(coordinator.stats().batches_published);
        (void)warmup_failure.append(" offered=");
        (void)warmup_failure.append_unsigned(frames_offered);
        (void)warmup_failure.append(" acquired=");
        (void)warmup_failure.append_unsigned(warmup_capture.frames_acquired);
        (void)warmup_failure.append(" empty=");
        (void)warmup_failure.append_unsigned(warmup_capture.empty_acquires);
        (void)warmup_failure.append(" imports=");
        (void)warmup_failure.append_unsigned(warmup_bridge.imports);
        (void)warmup_failure.append(" native_error=");
        (void)warmup_failure.append_signed(capture_error);
        (void)warmup_failure.append(" transfer_stage=");
        (void)warmup_failure.append_unsigned(static_cast<uint32_t>(transfer_stats.error_stage));
        (void)warmup_failure.append(" transfer_error=");
        (void)warmup_failure.append_signed(transfer_stats.native_error);
        (void)warmup_failure.append(" device_removed=");
        (void)warmup_failure.append_signed(capture_provider.device()->GetDeviceRemovedReason());
        (void)warmup_failure.append('\n');
        emit(warmup_failure.view());
        return exit_code(ExitCode::run);
    }

    const DesktopNeuralCoordinatorStats coordinator_baseline = coordinator.stats();
    const DualRateStats scheduler_baseline = coordinator.scheduler_stats();
    DirectMlPipelineStats previous_pipeline_stats = provider.pipeline_stats();
    saccade::platform::windows::SceneCaptureStats capture_baseline{};
    (void)captures.read_stats(&capture_baseline);
    SaccadeMemoryStats inference_memory_baseline{};
    inference_memory_baseline.struct_size = sizeof(inference_memory_baseline);
    inference_memory_baseline.api_version = SACCADE_API_VERSION;
    SaccadeMemoryStats capture_memory_baseline = inference_memory_baseline;
    if (saccade_inference_memory_stats(inference.runtime(), inference.session(), &inference_memory_baseline) !=
            SACCADE_OK ||
        captures.read_memory_stats(&capture_memory_baseline) != SACCADE_OK) {
        return exit_code(ExitCode::memory);
    }

    frames_offered = 0;
    capture_cycles = 0;
    const uint64_t duration_ns = static_cast<uint64_t>(duration_seconds) * UINT64_C(1'000'000'000);
    const uint64_t run_started = monotonic_ns();
    next_capture_ns = run_started;
    while (run_result == SACCADE_OK) {
        const uint64_t now_ns = monotonic_ns();
        if (now_ns == 0) {
            run_result = SACCADE_ERROR_BACKEND;
            break;
        }
        if (now_ns - run_started >= duration_ns) break;
        if (now_ns >= next_capture_ns) {
            run_result = mode == BenchmarkMode::live
                             ? offer_frames(displays.snapshot(), &captures, &bridge, &coordinator, inference.runtime(),
                                            &frames_offered)
                             : offer_replay_frames(displays.snapshot(), replay, &coordinator, inference.runtime(),
                                                   now_ns, &next_frame_id, &frames_offered);
            ++capture_cycles;
            const uint64_t elapsed = now_ns - next_capture_ns;
            next_capture_ns += (elapsed / saccade::scheduler::interaction_period_120hz_ns + 1U) *
                               saccade::scheduler::interaction_period_120hz_ns;
        }
        DesktopNeuralAdvance advance{};
        if (run_result == SACCADE_OK) run_result = coordinator.advance(now_ns, &advance);
        record_sample(&samples, advance, profile_stages, provider, &previous_pipeline_stats);
        (void)SwitchToThread();
    }

    const uint64_t drain_deadline = monotonic_ns() + drain_timeout_ns;
    while (run_result == SACCADE_OK && coordinator.stats().batches_published < coordinator.stats().batches_started &&
           monotonic_ns() < drain_deadline) {
        DesktopNeuralAdvance advance{};
        run_result = coordinator.advance(monotonic_ns(), &advance);
        record_sample(&samples, advance, profile_stages, provider, &previous_pipeline_stats);
        (void)SwitchToThread();
    }
    if (mode == BenchmarkMode::replay) {
        const SaccadeResult released = release_replay_frames(&captures, &replay);
        if (released != SACCADE_OK && run_result == SACCADE_OK) run_result = released;
        if (captures.set_running(false) != SACCADE_OK && run_result == SACCADE_OK) {
            run_result = SACCADE_ERROR_BACKEND;
        }
    }
    const uint64_t run_ended = monotonic_ns();
    if (run_result != SACCADE_OK || run_ended <= run_started) return exit_code(ExitCode::run);

    SaccadeMemoryStats inference_memory{};
    inference_memory.struct_size = sizeof(inference_memory);
    inference_memory.api_version = SACCADE_API_VERSION;
    SaccadeMemoryStats capture_memory = inference_memory;
    if (saccade_inference_memory_stats(inference.runtime(), inference.session(), &inference_memory) != SACCADE_OK ||
        captures.read_memory_stats(&capture_memory) != SACCADE_OK) {
        return exit_code(ExitCode::memory);
    }

    const auto coordinator_stats = coordinator.stats();
    const auto scheduler_stats = coordinator.scheduler_stats();
    const auto pipeline_stats = provider.pipeline_stats();
    const uint64_t timed_tickets = pipeline_stats.tickets == 0 ? 1 : pipeline_stats.tickets;
    saccade::platform::windows::SceneCaptureStats capture_stats{};
    (void)captures.read_stats(&capture_stats);
    const uint64_t batch_p50 = percentile(&samples.batch, samples.count, 50);
    const uint64_t batch_p95 = percentile(&samples.batch, samples.count, 95);
    const uint64_t batch_p99 = percentile(&samples.batch, samples.count, 99);
    const uint64_t full_p50 = percentile(&samples.full_scope, samples.count, 50);
    const uint64_t full_p95 = percentile(&samples.full_scope, samples.count, 95);
    const uint64_t full_p99 = percentile(&samples.full_scope, samples.count, 99);
    const uint64_t capture_age_p50 = percentile(&samples.capture_age, samples.count, 50);
    const uint64_t capture_age_p95 = percentile(&samples.capture_age, samples.count, 95);
    const uint64_t capture_age_p99 = percentile(&samples.capture_age, samples.count, 99);
    const uint64_t import_p95 = percentile(&samples.import, samples.count, 95);
    const uint64_t preprocess_p50 = percentile(&samples.preprocess, samples.count, 50);
    const uint64_t preprocess_p95 = percentile(&samples.preprocess, samples.count, 95);
    const uint64_t preprocess_p99 = percentile(&samples.preprocess, samples.count, 99);
    const uint64_t inference_p50 = percentile(&samples.inference, samples.count, 50);
    const uint64_t inference_p95 = percentile(&samples.inference, samples.count, 95);
    const uint64_t inference_p99 = percentile(&samples.inference, samples.count, 99);
    const uint64_t postprocess_p50 = percentile(&samples.postprocess, samples.count, 50);
    const uint64_t postprocess_p95 = percentile(&samples.postprocess, samples.count, 95);
    const uint64_t postprocess_p99 = percentile(&samples.postprocess, samples.count, 99);
    const uint64_t measured_ns = run_ended - run_started;
    const uint64_t batches_published = coordinator_stats.batches_published - coordinator_baseline.batches_published;
    const uint64_t batch_deadline_misses =
        coordinator_stats.batch_deadlines_missed - coordinator_baseline.batch_deadlines_missed;
    const uint64_t full_scope_deadline_misses =
        coordinator_stats.full_scope_deadlines_missed - coordinator_baseline.full_scope_deadlines_missed;
    const uint64_t deadline_misses = std::max(batch_deadline_misses, full_scope_deadline_misses);
    const uint64_t refresh_millihz = batches_published * UINT64_C(1'000'000'000'000) / measured_ns;
    const uint64_t interaction_ticks =
        scheduler_stats.interaction_ticks - scheduler_baseline.interaction_ticks;
    const uint64_t interaction_refresh_millihz =
        interaction_ticks * UINT64_C(1'000'000'000'000) / measured_ns;
    const uint64_t deadline_miss_ppm =
        batches_published == 0 ? UINT64_MAX : deadline_misses * UINT64_C(1'000'000) / batches_published;
    const uint64_t inference_high_water_growth =
        inference_memory.high_water_bytes > inference_memory_baseline.high_water_bytes
            ? inference_memory.high_water_bytes - inference_memory_baseline.high_water_bytes
            : 0;
    const uint64_t capture_high_water_growth =
        capture_memory.high_water_bytes > capture_memory_baseline.high_water_bytes
            ? capture_memory.high_water_bytes - capture_memory_baseline.high_water_bytes
            : 0;
    const bool memory_stable = inference_high_water_growth == 0 && capture_high_water_growth == 0;
    const bool scheduling_ready = scheduling.mmcss_active() && provider.worker_mmcss_active();
    const bool qualified = scheduling_ready && refresh_millihz >= minimum_refresh_millihz &&
                           interaction_refresh_millihz >= minimum_interaction_millihz &&
                           batch_p95 <= saccade::scheduler::scene_period_30hz_ns &&
                           full_p95 <= saccade::scheduler::scene_period_30hz_ns &&
                           deadline_miss_ppm <= maximum_deadline_miss_ppm && memory_stable;

    StackStringBuilder<2048> output;
    bool written =
        output.append("windows_full_scope mode=") && output.append(mode_name(mode)) && output.append(' ') &&
        append_metric(&output, "mmcss", scheduling.mmcss_active() ? 1U : 0U) &&
        append_metric(&output, "worker_mmcss", provider.worker_mmcss_active() ? 1U : 0U) &&
        append_metric(&output, "worker_thread_id", provider.worker_thread_id()) &&
        append_metric(&output, "process_priority_elevated", scheduling.process_priority_elevated() ? 1U : 0U) &&
        append_metric(&output, "duration_ns", measured_ns) &&
        append_metric(&output, "displays", displays.snapshot().count) &&
        append_metric(&output, "warmup_scenes", coordinator_baseline.batches_published) &&
        append_metric(&output, "cold_batch_ns", cold_batch_latency_ns) &&
        append_metric(&output, "cold_full_scope_ns", cold_full_scope_latency_ns) &&
        append_metric(&output, "capture_cycles", capture_cycles) &&
        append_metric(&output, "frames_offered", frames_offered) &&
        append_metric(&output, "scenes", batches_published) &&
        append_metric(&output, "refresh_millihz", refresh_millihz) &&
        append_metric(&output, "interaction_refresh_millihz", interaction_refresh_millihz) &&
        append_metric(&output, "minimum_interaction_millihz", minimum_interaction_millihz) &&
        append_metric(&output, "batch_p50_ns", batch_p50) && append_metric(&output, "batch_p95_ns", batch_p95) &&
        append_metric(&output, "batch_p99_ns", batch_p99) && append_metric(&output, "full_scope_p50_ns", full_p50) &&
        append_metric(&output, "full_scope_p95_ns", full_p95) &&
        append_metric(&output, "full_scope_p99_ns", full_p99) &&
        append_metric(&output, "capture_age_p50_ns", capture_age_p50) &&
        append_metric(&output, "capture_age_p95_ns", capture_age_p95) &&
        append_metric(&output, "capture_age_p99_ns", capture_age_p99) &&
        append_metric(&output, "batch_misses", batch_deadline_misses) &&
        append_metric(&output, "full_scope_misses", full_scope_deadline_misses) &&
        append_metric(&output, "deadline_miss_ppm", deadline_miss_ppm) &&
        append_metric(&output, "scene_replaced", scheduler_stats.scene_replaced - scheduler_baseline.scene_replaced) &&
        append_metric(&output, "interaction_ticks", interaction_ticks) &&
        append_metric(&output, "interaction_skipped",
                      scheduler_stats.interaction_skipped - scheduler_baseline.interaction_skipped) &&
        append_metric(&output, "capture_empty", capture_stats.empty_acquires - capture_baseline.empty_acquires) &&
        append_metric(&output, "targets",
                      coordinator_stats.targets_published - coordinator_baseline.targets_published) &&
        append_metric(&output, "inference_high_water_baseline", inference_memory_baseline.high_water_bytes) &&
        append_metric(&output, "inference_high_water", inference_memory.high_water_bytes) &&
        append_metric(&output, "inference_high_water_growth", inference_high_water_growth) &&
        append_metric(&output, "capture_high_water_baseline", capture_memory_baseline.high_water_bytes) &&
        append_metric(&output, "capture_high_water", capture_memory.high_water_bytes) &&
        append_metric(&output, "capture_high_water_growth", capture_high_water_growth) &&
        append_metric(&output, "memory_stable", memory_stable ? 1U : 0U) &&
        append_metric(&output, "direct_imports", pipeline_stats.direct_imports) &&
        append_metric(&output, "wrapped_imports", pipeline_stats.wrapped_imports) &&
        append_metric(&output, "profile_import_ns", pipeline_stats.import_ns / timed_tickets) &&
        append_metric(&output, "profile_preprocess_ns", pipeline_stats.preprocess_ns / timed_tickets) &&
        append_metric(&output, "profile_inference_ns", pipeline_stats.inference_ns / timed_tickets) &&
        append_metric(&output, "profile_postprocess_ns", pipeline_stats.postprocess_ns / timed_tickets) &&
        append_metric(&output, "profile_import_p95_ns", import_p95) &&
        append_metric(&output, "profile_preprocess_p50_ns", preprocess_p50) &&
        append_metric(&output, "profile_preprocess_p95_ns", preprocess_p95) &&
        append_metric(&output, "profile_preprocess_p99_ns", preprocess_p99) &&
        append_metric(&output, "profile_inference_p50_ns", inference_p50) &&
        append_metric(&output, "profile_inference_p95_ns", inference_p95) &&
        append_metric(&output, "profile_inference_p99_ns", inference_p99) &&
        append_metric(&output, "profile_postprocess_p50_ns", postprocess_p50) &&
        append_metric(&output, "profile_postprocess_p95_ns", postprocess_p95) &&
        append_metric(&output, "profile_postprocess_p99_ns", postprocess_p99) &&
        append_metric(&output, "qualified", qualified ? 1U : 0U) && output.append('\n');
    if (!written) return exit_code(ExitCode::run);
    emit(output.view());

    SaccadeResult cleanup = coordinator.shutdown();
    if (mode == BenchmarkMode::live && captures.set_running(false) != SACCADE_OK && cleanup == SACCADE_OK) {
        cleanup = SACCADE_ERROR_BACKEND;
    }
    if (bridge.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (captures.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (inference.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (provider.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (artifact.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (scheduling.shutdown() != SACCADE_OK && cleanup == SACCADE_OK) cleanup = SACCADE_ERROR_BACKEND;
    if (previous_dpi != nullptr) (void)SetThreadDpiAwarenessContext(previous_dpi);
    if (cleanup != SACCADE_OK) return exit_code(ExitCode::cleanup);
    if (!qualified) return exit_code(ExitCode::qualification);
    return exit_code(ExitCode::success);
}

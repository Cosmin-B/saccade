#include "backends/d3d12/directml_provider.hpp"
#include "application/settings.hpp"
#include "model/artifact.hpp"
#include "model/directml_contract.hpp"
#include "platform/windows/screen_capture.hpp"
#include "platform/windows/desktop_pipeline.hpp"
#include "scene/packet.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {

using saccade::backend::d3d12::DirectMlExecutionPolicy;
using saccade::backend::d3d12::DirectMlInferenceProvider;
using saccade::backend::d3d12::DirectMlModelStage;
using saccade::backend::d3d12::DirectMlProviderConfig;
using saccade::platform::windows::NativeCapturedFrame;
using saccade::platform::windows::ScreenCaptureProvider;

constexpr uint64_t model_stable_id = 0x5341434341444501;
constexpr uint32_t candidate_capacity = 3;
constexpr uint32_t target_capacity = 3;
constexpr uint32_t expected_target_count = 2;
constexpr uint32_t maximum_output_bytes =
    sizeof(SaccadeTargetPacketHeader) + target_capacity * sizeof(SaccadeTargetRecord);
constexpr uint32_t required_capabilities =
    SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC;
constexpr char input_name[] = "input";
constexpr char candidate_name[] = "candidates";
constexpr std::array<uint8_t, 301> graph{
    0x08, 0x07, 0x12, 0x0c, 0x73, 0x61, 0x63, 0x63, 0x61, 0x64, 0x65, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x3a, 0x94, 0x02,
    0x0a, 0x21, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x0a, 0x05, 0x73, 0x68, 0x61, 0x70, 0x65, 0x12, 0x08, 0x72,
    0x65, 0x73, 0x68, 0x61, 0x70, 0x65, 0x64, 0x22, 0x07, 0x52, 0x65, 0x73, 0x68, 0x61, 0x70, 0x65, 0x0a, 0x1e, 0x0a,
    0x08, 0x72, 0x65, 0x73, 0x68, 0x61, 0x70, 0x65, 0x64, 0x0a, 0x04, 0x7a, 0x65, 0x72, 0x6f, 0x12, 0x07, 0x63, 0x6c,
    0x65, 0x61, 0x72, 0x65, 0x64, 0x22, 0x03, 0x4d, 0x75, 0x6c, 0x0a, 0x20, 0x0a, 0x07, 0x63, 0x6c, 0x65, 0x61, 0x72,
    0x65, 0x64, 0x0a, 0x04, 0x72, 0x6f, 0x77, 0x73, 0x12, 0x0a, 0x63, 0x61, 0x6e, 0x64, 0x69, 0x64, 0x61, 0x74, 0x65,
    0x73, 0x22, 0x03, 0x41, 0x64, 0x64, 0x12, 0x19, 0x73, 0x61, 0x63, 0x63, 0x61, 0x64, 0x65, 0x5f, 0x64, 0x69, 0x72,
    0x65, 0x63, 0x74, 0x6d, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x76, 0x69, 0x64, 0x65, 0x72, 0x2a, 0x0f, 0x08, 0x02, 0x10,
    0x07, 0x3a, 0x02, 0x03, 0x06, 0x42, 0x05, 0x73, 0x68, 0x61, 0x70, 0x65, 0x2a, 0x0e, 0x08, 0x01, 0x10, 0x0a, 0x42,
    0x04, 0x7a, 0x65, 0x72, 0x6f, 0x4a, 0x02, 0x00, 0x00, 0x2a, 0x32, 0x08, 0x03, 0x08, 0x06, 0x10, 0x0a, 0x42, 0x04,
    0x72, 0x6f, 0x77, 0x73, 0x4a, 0x24, 0x66, 0x2e, 0x66, 0x2e, 0x66, 0x32, 0x66, 0x32, 0x33, 0x3b, 0x00, 0x3c, 0x66,
    0x36, 0x66, 0x36, 0x66, 0x32, 0x66, 0x32, 0x66, 0x3a, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x5a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x16, 0x0a, 0x14, 0x08, 0x0a,
    0x12, 0x10, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x03, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x03, 0x62,
    0x1c, 0x0a, 0x0a, 0x63, 0x61, 0x6e, 0x64, 0x69, 0x64, 0x61, 0x74, 0x65, 0x73, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x0a,
    0x12, 0x08, 0x0a, 0x02, 0x08, 0x03, 0x0a, 0x02, 0x08, 0x06, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x0d};

constexpr size_t payload_size = saccade::model::directml::payload_header_bytes + sizeof(input_name) - 1U +
                                sizeof(candidate_name) - 1U + graph.size();
constexpr size_t payload_offset = saccade::model::artifact_header_bytes;
constexpr size_t signature_offset = payload_offset + payload_size;
constexpr size_t artifact_size = signature_offset + saccade::model::artifact_signature_bytes;

enum class TestResult : int {
    success,
    usage,
    provider_unavailable = 77,
    provider_device_failed = 2,
    capture_initialization_failed = 3,
    source_failed,
    stream_failed,
    runtime_failed,
    registration_failed,
    session_failed,
    model_stage_failed,
    capture_failed,
    native_frame_failed,
    import_failed,
    submit_failed,
    wait_failed,
    collect_failed,
    packet_failed,
    cleanup_failed,
    verification_failed,
    pipeline_file_failed,
    pipeline_initialization_failed,
    pipeline_state_failed,
    pipeline_diagnostics_failed,
    pipeline_shutdown_failed,
    software_provider_failed,
    software_device_failed,
    software_runtime_failed,
    software_session_failed,
    software_cleanup_failed,
    worker_scheduling_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

void write_u32(uint8_t* data, size_t offset, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void write_u64(uint8_t* data, size_t offset, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index) {
        data[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

std::array<uint8_t, artifact_size> make_artifact() noexcept {
    std::array<uint8_t, artifact_size> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = 'D';
    write_u32(bytes.data(), 4, saccade::model::artifact_version);
    write_u32(bytes.data(), 8, saccade::model::artifact_header_bytes);
    write_u32(bytes.data(), 12, static_cast<uint32_t>(bytes.size()));
    write_u64(bytes.data(), 16, model_stable_id);
    write_u32(bytes.data(), 24, static_cast<uint32_t>(saccade::model::GraphKind::ui_detector));
    write_u32(bytes.data(), 28, static_cast<uint32_t>(saccade::model::ArtifactKind::onnx));
    write_u32(bytes.data(), 32, SACCADE_PRECISION_FP16);
    write_u32(bytes.data(), 36, 3);
    write_u32(bytes.data(), 40, 2);
    write_u32(bytes.data(), 44, 3);
    write_u32(bytes.data(), 48, target_capacity);
    write_u32(bytes.data(), 52, maximum_output_bytes);
    write_u64(bytes.data(), 56, payload_offset);
    write_u64(bytes.data(), 64, payload_size);
    write_u64(bytes.data(), 72, signature_offset);
    write_u32(bytes.data(), 80, saccade::model::artifact_signature_bytes);
    write_u32(bytes.data(), 84, saccade::model::artifact_has_signature);
    write_u64(bytes.data(), 88, saccade::model::directml::provider_compatibility_bit);
    uint8_t* payload = bytes.data() + payload_offset;
    payload[0] = 'S';
    payload[1] = 'C';
    payload[2] = 'D';
    payload[3] = 'M';
    write_u32(payload, 4, saccade::model::directml::contract_version);
    write_u32(payload, 8, static_cast<uint32_t>(saccade::model::directml::InputKind::planar_fp16));
    write_u32(payload, 12, static_cast<uint32_t>(saccade::model::directml::OutputLayout::normalized_target_rows_v1));
    write_u32(payload, 16, candidate_capacity);
    write_u32(payload, 20, target_capacity);
    write_u32(payload, 24, 1);
    write_u32(payload, 28, 32768);
    write_u32(payload, 32, sizeof(input_name) - 1U);
    write_u32(payload, 36, sizeof(candidate_name) - 1U);
    size_t position = saccade::model::directml::payload_header_bytes;
    std::memcpy(payload + position, input_name, sizeof(input_name) - 1U);
    position += sizeof(input_name) - 1U;
    std::memcpy(payload + position, candidate_name, sizeof(candidate_name) - 1U);
    position += sizeof(candidate_name) - 1U;
    std::memcpy(payload + position, graph.data(), graph.size());
    return bytes;
}

struct Verification {
    uint32_t calls = 0;
};

SaccadeResult verify(void* context, const saccade::model::ArtifactView& artifact) noexcept {
    auto* verification = static_cast<Verification*>(context);
    ++verification->calls;
    return artifact.stable_id == model_stable_id && artifact.artifact == saccade::model::ArtifactKind::onnx
               ? SACCADE_OK
               : SACCADE_ERROR_INVALID_ARGUMENT;
}

TestResult verify_software_model(const char* shader_directory, Verification* verification,
                                 const std::array<uint8_t, artifact_size>& artifact) noexcept {
    static DirectMlInferenceProvider provider;
    const DirectMlProviderConfig config{
        shader_directory, {verification, verify}, DirectMlExecutionPolicy::software_only};
    if (provider.initialize(config) != SACCADE_OK) return TestResult::software_provider_failed;

    const SaccadeInferenceProviderDesc descriptor = provider.descriptor();
    SaccadeDeviceInfo device = output_structure<SaccadeDeviceInfo>();
    if (descriptor.ops.enumerate_devices(descriptor.context, 0, &device) != SACCADE_OK ||
        (device.capability_bits & SACCADE_PROVIDER_CAPABILITY_CPU) == 0 ||
        (device.capability_bits & SACCADE_PROVIDER_CAPABILITY_GPU) != 0) {
        return TestResult::software_device_failed;
    }

    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK ||
        saccade_register_inference_provider(runtime, &descriptor) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return TestResult::software_runtime_failed;
    }

    SaccadeInferenceSessionDesc session_desc{};
    session_desc.struct_size = sizeof(session_desc);
    session_desc.api_version = SACCADE_API_VERSION;
    session_desc.model_bytes = {artifact.data(), artifact.size()};
    session_desc.model_stable_id = model_stable_id;
    session_desc.required_capability_bits =
        SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC;
    session_desc.required_format_bits = SACCADE_FORMAT_BGRA8;
    session_desc.required_precision_bits = SACCADE_PRECISION_FP16;
    session_desc.required_import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    session_desc.queue_capacity = 1;
    session_desc.max_in_flight = 1;
    SaccadeExecutionContextHandle session = 0;
    SaccadeInferenceSessionInfo session_info = output_structure<SaccadeInferenceSessionInfo>();
    if (saccade_inference_session_create(runtime, &session_desc, &session, &session_info) != SACCADE_OK) {
        return TestResult::software_session_failed;
    }
    if (!provider.worker_mmcss_active() || provider.worker_thread_id() == 0) {
        return TestResult::worker_scheduling_failed;
    }

    if (saccade_inference_session_destroy(runtime, session) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK || provider.shutdown() != SACCADE_OK) {
        return TestResult::software_cleanup_failed;
    }
    if (provider.initialize(config) != SACCADE_OK || provider.shutdown() != SACCADE_OK)
        return TestResult::software_cleanup_failed;
    return TestResult::success;
}

TestResult verify_provider_policies(const char* shader_directory) noexcept {
    Verification verification{};
    static DirectMlInferenceProvider provider;
    const DirectMlProviderConfig hardware_config{
        shader_directory, {&verification, verify}, DirectMlExecutionPolicy::hardware_only};
    if (provider.initialize(hardware_config) != SACCADE_OK) return TestResult::provider_unavailable;

    const SaccadeInferenceProviderDesc descriptor = provider.descriptor();
    SaccadeDeviceInfo device = output_structure<SaccadeDeviceInfo>();
    const bool hardware_valid = descriptor.ops.enumerate_devices(descriptor.context, 0, &device) == SACCADE_OK &&
                                device.stable_id == provider.adapter_luid() &&
                                (device.capability_bits & SACCADE_PROVIDER_CAPABILITY_GPU) != 0 &&
                                (device.capability_bits & SACCADE_PROVIDER_CAPABILITY_CPU) == 0;
    if (!hardware_valid) {
        (void)provider.shutdown();
        return TestResult::provider_device_failed;
    }
    if (provider.shutdown() != SACCADE_OK) return TestResult::cleanup_failed;

    const auto artifact = make_artifact();
    const TestResult software = verify_software_model(shader_directory, &verification, artifact);
    if (software != TestResult::success) return software;
    return verification.calls == 1 ? TestResult::success : TestResult::verification_failed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--headless") == 0) {
        return result(verify_provider_policies(argv[2]));
    }
    if (argc != 4 || std::strcmp(argv[1], "--live") != 0) return result(TestResult::usage);

    const char* shader_directory = argv[2];
    const char* artifact_path = argv[3];
    Verification verification{};
    static DirectMlInferenceProvider inference_provider;
    if (inference_provider.initialize(DirectMlProviderConfig{shader_directory, {&verification, verify}}) !=
        SACCADE_OK) {
        return result(TestResult::provider_unavailable);
    }
    const SaccadeInferenceProviderDesc inference = inference_provider.descriptor();
    SaccadeDeviceInfo inference_device = output_structure<SaccadeDeviceInfo>();
    if (inference.ops.enumerate_devices(inference.context, 0, &inference_device) != SACCADE_OK ||
        inference_device.stable_id != inference_provider.adapter_luid())
        return result(TestResult::provider_device_failed);
    static ScreenCaptureProvider capture_provider;
    if (capture_provider.initialize(inference_provider.capture_device()) != SACCADE_OK) {
        return result(TestResult::capture_initialization_failed);
    }
    const SaccadeCaptureProviderDesc capture = capture_provider.descriptor();
    SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
    if (capture.ops.enumerate_sources(capture.context, 0, &source) != SACCADE_OK) {
        return result(TestResult::source_failed);
    }
    SaccadeCaptureStreamDesc stream_desc{};
    stream_desc.struct_size = sizeof(stream_desc);
    stream_desc.api_version = SACCADE_API_VERSION;
    stream_desc.source_id = source.stable_id;
    stream_desc.pixel_format = SACCADE_FORMAT_BGRA8;
    stream_desc.queue_capacity = 3;
    SaccadeCaptureStreamHandle stream = 0;
    if (capture.ops.create(capture.context, &stream_desc, &stream) != SACCADE_OK ||
        capture.ops.start(capture.context, stream) != SACCADE_OK) {
        return result(TestResult::stream_failed);
    }

    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK) {
        return result(TestResult::runtime_failed);
    }
    const SaccadeInferenceProviderDesc provider = inference_provider.descriptor();
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return result(TestResult::registration_failed);
    }
    auto artifact = make_artifact();
    SaccadeInferenceSessionDesc session_desc{};
    session_desc.struct_size = sizeof(session_desc);
    session_desc.api_version = SACCADE_API_VERSION;
    session_desc.model_bytes = {artifact.data(), artifact.size()};
    session_desc.model_stable_id = model_stable_id;
    session_desc.required_capability_bits = required_capabilities;
    session_desc.required_format_bits = SACCADE_FORMAT_BGRA8;
    session_desc.required_precision_bits = SACCADE_PRECISION_INT8;
    session_desc.required_import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    session_desc.queue_capacity = 1;
    session_desc.max_in_flight = 1;
    SaccadeExecutionContextHandle session = 0;
    SaccadeInferenceSessionInfo session_info = output_structure<SaccadeInferenceSessionInfo>();
    if (saccade_inference_session_create(runtime, &session_desc, &session, &session_info) != SACCADE_OK) {
        return result(TestResult::session_failed);
    }
    if (inference_provider.model_stage() != DirectMlModelStage::ready) {
        return result(TestResult::model_stage_failed);
    }

    SaccadeCapturedFrame captured{};
    SaccadeResult acquired = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < 300 && acquired == SACCADE_ERROR_BUSY; ++attempt) {
        Sleep(10);
        captured = output_structure<SaccadeCapturedFrame>();
        acquired = capture.ops.acquire(capture.context, stream, 0, &captured);
    }
    if (acquired != SACCADE_OK) return result(TestResult::capture_failed);
    NativeCapturedFrame native{};
    if (capture_provider.read_native_frame(stream, captured.frame, &native) != SACCADE_OK ||
        native.d3d11_texture == nullptr) {
        return result(TestResult::native_frame_failed);
    }
    SaccadeWin32CaptureFrameDesc frame_desc{};
    frame_desc.struct_size = sizeof(frame_desc);
    frame_desc.api_version = SACCADE_API_VERSION;
    frame_desc.texture = native.d3d11_texture;
    frame_desc.subresource = native.subresource;
    frame_desc.pixel_format = native.pixel_format;
    frame_desc.width = captured.width;
    frame_desc.height = captured.height;
    frame_desc.frame_id = captured.frame_id;
    frame_desc.transform_epoch = captured.transform_epoch;
    SaccadeFrameHandle frame = 0;
    if (saccade_frame_import_win32_capture(runtime, &frame_desc, &frame) != SACCADE_OK ||
        capture.ops.release(capture.context, stream, captured.frame) != SACCADE_OK) {
        return result(TestResult::import_failed);
    }
    SaccadeInferenceSubmitDesc submit{};
    submit.struct_size = sizeof(submit);
    submit.api_version = SACCADE_API_VERSION;
    submit.frame = frame;
    submit.scope = {0, 0, static_cast<int32_t>(captured.width), static_cast<int32_t>(captured.height)};
    submit.output_capacity = maximum_output_bytes;
    submit.model_epoch = 1;
    submit.session_epoch = 2;
    submit.transform_epoch = captured.transform_epoch;
    submit.topology_epoch = 3;
    submit.source_id = source.stable_id;
    SaccadeTicketHandle ticket = 0;
    if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK) {
        return result(TestResult::submit_failed);
    }
    SaccadeInferenceStatus status = output_structure<SaccadeInferenceStatus>();
    const SaccadeResult waited = saccade_inference_wait(runtime, session, ticket, UINT64_C(5'000'000'000), &status);
    if (waited != SACCADE_OK || status.state != SACCADE_TICKET_COMPLETE || status.result != SACCADE_OK) {
        std::fprintf(stderr, "wait %d state %u result %d\n", waited, status.state, status.result);
        return result(TestResult::wait_failed);
    }
    alignas(8) std::array<uint8_t, maximum_output_bytes> packet{};
    size_t packet_size = 0;
    if (saccade_inference_collect(runtime, session, ticket, {packet.data(), packet.size()}, &packet_size) !=
        SACCADE_OK) {
        return result(TestResult::collect_failed);
    }
    saccade::scene::PacketView packet_view{};
    if (saccade::scene::validate_packet({packet.data(), packet_size}, &packet_view) != SACCADE_OK ||
        packet_view.header->target_count != expected_target_count ||
        packet_view.header->frame_id != captured.frame_id) {
        return result(TestResult::packet_failed);
    }
    const bool cleaned =
        saccade_frame_release(runtime, frame) == SACCADE_OK &&
        saccade_inference_session_destroy(runtime, session) == SACCADE_OK &&
        saccade_runtime_destroy(runtime) == SACCADE_OK && capture.ops.stop(capture.context, stream) == SACCADE_OK &&
        capture.ops.destroy(capture.context, stream) == SACCADE_OK && inference_provider.shutdown() == SACCADE_OK;
    if (!cleaned) return result(TestResult::cleanup_failed);
    if (verification.calls != 1) return result(TestResult::verification_failed);
    const TestResult software_result = verify_software_model(shader_directory, &verification, artifact);
    if (software_result != TestResult::success) return result(software_result);
    std::FILE* file = nullptr;
    if (fopen_s(&file, artifact_path, "wb") != 0 || file == nullptr ||
        std::fwrite(artifact.data(), 1, artifact.size(), file) != artifact.size() || std::fclose(file) != 0)
        return result(TestResult::pipeline_file_failed);
    verification.calls = 0;
    auto settings = saccade::application::default_settings();
    static saccade::platform::windows::DesktopPipeline pipeline;
    saccade::platform::windows::DesktopPipelineConfig pipeline_config{};
    pipeline_config.artifact_path = artifact_path;
    pipeline_config.shader_directory = shader_directory;
    pipeline_config.settings = &settings;
    pipeline_config.verifier = {&verification, verify};
    pipeline_config.start_time_ns = 1;
    const DPI_AWARENESS_CONTEXT previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous_dpi == nullptr) return result(TestResult::pipeline_initialization_failed);
    const SaccadeResult pipeline_initialized = pipeline.initialize(pipeline_config);
    if (pipeline_initialized != SACCADE_OK) {
        std::fprintf(stderr, "pipeline initialization result: %d stage: %u\n", pipeline_initialized,
                     static_cast<uint32_t>(pipeline.last_stage()));
        return result(TestResult::pipeline_initialization_failed);
    }
    if (pipeline.active() || pipeline.stats().capture_starts != 0 || verification.calls != 2)
        return result(TestResult::pipeline_state_failed);
    saccade::platform::windows::DesktopPipelineDiagnostics diagnostics{};
    if (pipeline.read_diagnostics(&diagnostics) != SACCADE_OK || diagnostics.model.model_stable_id != model_stable_id ||
        diagnostics.display_count == 0 || diagnostics.topology_epoch == 0 ||
        diagnostics.overlay_set.active_surfaces != diagnostics.display_count ||
        diagnostics.capture.active_streams != diagnostics.display_count)
        return result(TestResult::pipeline_diagnostics_failed);
    if (pipeline.shutdown() != SACCADE_OK || std::remove(artifact_path) != 0)
        return result(TestResult::pipeline_shutdown_failed);
    (void)SetThreadDpiAwarenessContext(previous_dpi);
    return result(TestResult::success);
}

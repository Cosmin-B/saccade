#include "application/desktop_runtime.hpp"
#include "application/inference_runtime.hpp"
#include "application/session_key_router.hpp"
#include "backends/reference_cpu/reference_cpu.hpp"
#include "input/plan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    runtime_create_failed,
    session_create_failed,
    owner_initialize_failed,
    fusion_setting_failed,
    frame_import_failed,
    frame_offer_failed,
    advance_failed,
    dispatch_failed,
    selection_failed,
    grid_failed,
    window_failed,
    trace_failed,
    shutdown_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct Capture {
    saccade::application::DesktopRuntime* runtime = nullptr;
    uint32_t executions = 0;
    uint32_t retirements = 0;
    uint32_t session_commands = 0;
    saccade::application::Command last_session_command = saccade::application::Command::pointer_move;
};

SaccadeResult execute(void* context, SaccadeSpanU8 bytes, uint32_t, uint64_t) noexcept {
    saccade::input::PlanView plan{};
    if (saccade::input::validate_plan(bytes, &plan) != SACCADE_OK) return SACCADE_ERROR_INVALID_ARGUMENT;
    ++static_cast<Capture*>(context)->executions;
    return SACCADE_OK;
}

SaccadeResult read_environment(void*, saccade::application::InteractionState* output) noexcept {
    *output = {};
    output->permission_epoch = 70;
    output->focus_id = 80;
    output->permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_WINDOW;
    return SACCADE_OK;
}

void retire(void* context, SaccadeFrameHandle) noexcept {
    ++static_cast<Capture*>(context)->retirements;
}

SaccadeResult route_session_command(void* context, saccade::application::Command command,
                                    uint64_t timestamp_ns) noexcept {
    auto* capture = static_cast<Capture*>(context);
    ++capture->session_commands;
    capture->last_session_command = command;
    saccade::application::InteractionCommandResult output{};
    return capture->runtime->dispatch(command, timestamp_ns, &output);
}

SaccadeResult unsupported_request(void*, const SaccadeAccessibilityQueryDesc*, SaccadeTicketHandle*) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeResult unsupported_poll(void*, SaccadeTicketHandle, SaccadeAccessibilityStatus*) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeResult unsupported_wait(void*, SaccadeTicketHandle, uint64_t, SaccadeAccessibilityStatus*) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeResult unsupported_collect(void*, SaccadeSnapshotHandle, SaccadeMutableSpanU8, size_t*) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeResult unsupported_cancel(void*, SaccadeTicketHandle) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeResult unsupported_release(void*, SaccadeSnapshotHandle) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

SaccadeAccessibilityProviderDesc accessibility(Capture* context) noexcept {
    SaccadeAccessibilityProviderDesc value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.context = context;
    value.ops.struct_size = sizeof(value.ops);
    value.ops.api_version = SACCADE_API_VERSION;
    value.ops.request = unsupported_request;
    value.ops.poll = unsupported_poll;
    value.ops.wait = unsupported_wait;
    value.ops.collect = unsupported_collect;
    value.ops.cancel = unsupported_cancel;
    value.ops.release = unsupported_release;
    return value;
}

saccade::geometry::CoordinateTransform source_transform() noexcept {
    saccade::geometry::CoordinateTransform transform;
    saccade::geometry::TransformDesc desc{};
    desc.source = {0, 0, 2 * 256, 2 * 256};
    desc.destination = desc.source;
    desc.epoch = 10;
    desc.source_space = saccade::geometry::CoordinateSpace::capture;
    desc.destination_space = saccade::geometry::CoordinateSpace::desktop;
    (void)transform.initialize(desc);
    return transform;
}

} // namespace

int main() {
    saccade::application::DebugTrace bounded_trace;
    for (uint32_t index = 0; index < saccade::application::debug_trace_capacity + 8U; ++index)
        bounded_trace.record(saccade::application::DebugTraceCode::frame_offered, index + 1U, index);
    const saccade::application::DebugTraceSnapshot bounded_snapshot = bounded_trace.snapshot();
    if (bounded_snapshot.count != saccade::application::debug_trace_capacity || bounded_snapshot.overwritten != 8 ||
        bounded_snapshot.next_sequence != saccade::application::debug_trace_capacity + 9U ||
        bounded_snapshot.events[0].sequence != 9 ||
        bounded_snapshot.events[saccade::application::debug_trace_capacity - 1U].sequence !=
            saccade::application::debug_trace_capacity + 8U)
        return result(TestResult::trace_failed);
    saccade::backend::reference_cpu::Backend backend;
    const SaccadeInferenceProviderDesc provider = backend.provider();
    const auto model = saccade::backend::reference_cpu::encode_model({200, 1});
    saccade::application::InferenceRuntimeConfig inference_config{};
    inference_config.provider = provider;
    inference_config.artifact = {model.data(), model.size()};
    inference_config.model_stable_id = 1;
    inference_config.provider_stable_id = provider.info.stable_id;
    inference_config.required_capability_bits =
        SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT;
    inference_config.required_format_bits = SACCADE_FORMAT_BGRA8;
    inference_config.required_precision_bits = SACCADE_PRECISION_FP32;
    inference_config.required_import_bits = SACCADE_IMPORT_HOST;
    saccade::application::InferenceRuntime inference;
    if (inference.initialize(inference_config) != SACCADE_OK) return result(TestResult::session_create_failed);
    const SaccadeRuntimeHandle runtime = inference.runtime();

    Capture capture{};
    static saccade::application::DesktopRuntimeStorage storage;
    saccade::application::DesktopRuntimeConfig config{};
    config.neural.runtime = runtime;
    config.neural.session = inference.session();
    config.neural.model_epoch = 50;
    config.neural.session_epoch = 60;
    config.neural.desktop_source_id = 90;
    config.neural.maximum_output_bytes = inference.info().max_output_bytes;
    config.neural.maximum_targets = 16;
    config.neural.start_time_ns = 1;
    config.accessibility = accessibility(&capture);
    config.executor = {&capture, execute};
    constexpr std::array<uint16_t, 2> alphabet{'A', 'S'};
    std::copy(alphabet.begin(), alphabet.end(), config.interaction.hints.alphabet.begin());
    config.interaction.hints.physical_keys[0] = 0x04;
    config.interaction.hints.physical_keys[1] = 0x16;
    config.interaction.hints.alphabet_count = static_cast<uint32_t>(alphabet.size());
    config.environment = {nullptr, read_environment};
    saccade::application::DesktopRuntime owner;
    if (owner.initialize(config, &storage) != SACCADE_OK) return result(TestResult::owner_initialize_failed);
    capture.runtime = &owner;
    saccade::scene::FusionConfig updated_fusion = config.fusion;
    updated_fusion.iou_threshold_q16 = 40000;
    if (owner.set_fusion(updated_fusion) != SACCADE_OK) return result(TestResult::fusion_setting_failed);
    updated_fusion.iou_threshold_q16 = 0;
    if (owner.set_fusion(updated_fusion) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::fusion_setting_failed);

    const std::array<uint8_t, 16> pixels{255, 255, 255, 255, 255, 255, 255, 255,
                                         255, 255, 255, 255, 255, 255, 255, 255};
    SaccadeHostFrameDesc frame_desc{};
    frame_desc.struct_size = sizeof(frame_desc);
    frame_desc.api_version = SACCADE_API_VERSION;
    frame_desc.data = {pixels.data(), pixels.size()};
    frame_desc.width = 2;
    frame_desc.height = 2;
    frame_desc.row_stride_bytes = 8;
    frame_desc.pixel_format = SACCADE_FORMAT_BGRA8;
    frame_desc.frame_id = 2;
    frame_desc.transform_epoch = 10;
    SaccadeFrameHandle frame = 0;
    if (saccade_frame_import(runtime, &frame_desc, &frame) != SACCADE_OK)
        return result(TestResult::frame_import_failed);
    saccade::scheduler::DesktopNeuralFrame offered{};
    offered.frame = frame;
    offered.source_id = 40;
    offered.topology_epoch = 30;
    offered.transform_epoch = 10;
    offered.scene_transform_epoch = 20;
    offered.source_count = 1;
    offered.width = 2;
    offered.height = 2;
    offered.source_to_desktop = source_transform();
    offered.retire_context = &capture;
    offered.retire = retire;
    if (owner.offer(offered) != SACCADE_OK) return result(TestResult::frame_offer_failed);
    saccade::application::DesktopRuntimeAdvance advance{};
    if (owner.advance(1, &advance) != SACCADE_OK || advance.scene.scene_published ||
        owner.advance(2, &advance) != SACCADE_OK || !advance.scene.scene_published || advance.scene.target_count != 1 ||
        capture.retirements != 1)
        return result(TestResult::advance_failed);
    saccade::application::InteractionCommandResult command{};
    saccade::application::SessionKeyRouter keys;
    saccade::application::SessionKeyRoute key_result{};
    constexpr saccade::application::HotkeyBinding session_binding{saccade::application::Command::edge_snap_right, 0x4f,
                                                                  0, saccade::application::hotkey_session_only, 0};
    constexpr saccade::application::HotkeyBinding position_binding{
        saccade::application::Command::target_position_1, 0x1e, 0, saccade::application::hotkey_session_only, '1'};
    if (keys.initialize(&owner, {&capture, route_session_command}) != SACCADE_OK ||
        keys.replace(&session_binding, 1) != SACCADE_OK ||
        keys.route({3, session_binding.physical_key, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        capture.session_commands != 0 ||
        owner.dispatch(saccade::application::Command::left_click, 4, &command) != SACCADE_OK || !owner.active() ||
        keys.route({5, 0x28, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        keys.route({6, 0x2a, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        keys.route({7, 0x29, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        keys.route({8, position_binding.physical_key, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        !owner.active() || capture.session_commands != 0 ||
        keys.route({9, session_binding.physical_key, 0, 0}, &key_result) != SACCADE_OK || !key_result.handled ||
        key_result.session_ended || capture.session_commands != 1 ||
        capture.last_session_command != saccade::application::Command::edge_snap_right ||
        keys.route({10, 0x04, 0, 'x'}, &key_result) != SACCADE_OK || !key_result.handled || !key_result.session_ended ||
        capture.executions != 1 || owner.active() ||
        keys.route({11, session_binding.physical_key, 0, 0}, &key_result) != SACCADE_OK || key_result.handled ||
        capture.session_commands != 1)
        return result(TestResult::selection_failed);
    saccade::scene::GridSceneConfig grid{};
    grid.scope = {0, 0, 256, 256};
    grid.frame_id = 3;
    grid.model_epoch = 50;
    grid.session_epoch = 60;
    grid.transform_epoch = 20;
    grid.topology_epoch = 30;
    grid.source_id = 91;
    grid.rows = 1;
    grid.columns = 1;
    saccade::application::SceneCoordinatorAdvance grid_scene{};
    const std::array session_bindings{session_binding, position_binding};
    if (keys.replace(session_bindings.data(), static_cast<uint32_t>(session_bindings.size())) != SACCADE_OK ||
        owner.set_source(saccade::application::SceneSource::grid) != SACCADE_OK ||
        owner.publish_grid(grid, &grid_scene) != SACCADE_OK || !grid_scene.scene_published ||
        grid_scene.target_count != 1 ||
        owner.dispatch(saccade::application::Command::left_click, 12, &command) != SACCADE_OK || !owner.active() ||
        keys.route({13, position_binding.physical_key, 0, '1'}, &key_result) != SACCADE_OK || !key_result.handled ||
        key_result.session_ended || capture.last_session_command != saccade::application::Command::target_position_1 ||
        !owner.active() || keys.route({14, 0x04, 0, 'A'}, &key_result) != SACCADE_OK || !key_result.handled ||
        !key_result.session_ended || capture.executions != 2 || owner.active())
        return result(TestResult::grid_failed);
    SaccadeWindowInfo window{};
    window.struct_size = sizeof(window);
    window.api_version = SACCADE_API_VERSION;
    window.stable_id = 92;
    window.process_id = 93;
    window.desktop_bounds = {0, 0, 100, 80};
    saccade::scene::WindowSceneConfig window_config{};
    window_config.frame_id = 4;
    window_config.model_epoch = 50;
    window_config.session_epoch = 60;
    window_config.transform_epoch = 20;
    window_config.topology_epoch = 30;
    window_config.source_id = 94;
    if (owner.set_source(saccade::application::SceneSource::windows) != SACCADE_OK ||
        owner.publish_windows(window_config, &window, 1, &grid_scene) != SACCADE_OK ||
        owner.dispatch(saccade::application::Command::window_activate, 15, &command) != SACCADE_OK || !owner.active() ||
        keys.route({16, 0x04, 0, 'A'}, &key_result) != SACCADE_OK || !key_result.handled || !key_result.session_ended ||
        capture.executions != 3 || owner.active())
        return result(TestResult::window_failed);
    const saccade::application::DesktopRuntimeDiagnostics diagnostics = owner.diagnostics();
    if (diagnostics.scene_status.scene_epoch != grid_scene.scene_epoch || diagnostics.scene_status.frame_id != 4 ||
        diagnostics.scene_status.target_count != 1 || diagnostics.scene_status.packet_flags != 0 ||
        diagnostics.scene_status.source != saccade::application::SceneSource::windows || diagnostics.trace.count == 0 ||
        diagnostics.trace.events[0].code != saccade::application::DebugTraceCode::runtime_initialized ||
        diagnostics.trace.events[diagnostics.trace.count - 1U].sequence + 1U != diagnostics.trace.next_sequence)
        return result(TestResult::trace_failed);
    if (keys.shutdown() != SACCADE_OK || owner.shutdown() != SACCADE_OK || inference.shutdown() != SACCADE_OK)
        return result(TestResult::shutdown_failed);
    return result(TestResult::success);
}

#include "application/desktop_runtime.hpp"

namespace saccade::application {

DesktopRuntime::~DesktopRuntime() {
    (void)shutdown();
}

DesktopRuntimeDiagnostics DesktopRuntime::diagnostics() const noexcept {
    return {stats_,           neural_.stats(),      neural_.scheduler_stats(), scene_.stats(),   scene_.status(),
            session_.stats(), interaction_.stats(), overlay_.stats(),          trace_.snapshot()};
}

uint16_t DesktopRuntime::hint_symbol_for_physical_key(uint32_t physical_key) const noexcept {
    return initialized_ ? interaction::symbol_for_physical_key(config_.interaction.hints, physical_key) : 0;
}

SaccadeResult DesktopRuntime::fail(SaccadeResult result) noexcept {
    if (result != SACCADE_OK) {
        ++stats_.failures;
        trace_.record(DebugTraceCode::runtime_failure, 0, stats_.failures, result);
    }
    return result;
}

SaccadeResult DesktopRuntime::initialize(const DesktopRuntimeConfig& config, DesktopRuntimeStorage* storage) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (storage == nullptr || config.environment.read == nullptr || config.executor.execute == nullptr ||
        (config.sink.input_lease_active == nullptr) != (config.sink.neutralize_input == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    trace_.reset();
    config_ = config;
    SaccadeResult result = neural_scenes_.initialize(&storage->neural_scenes);
    if (result != SACCADE_OK) return result;
    result = output_scenes_.initialize(&storage->output_scenes);
    if (result != SACCADE_OK) return result;
    result = neural_.initialize(config.neural, &storage->neural, &neural_scenes_);
    if (result != SACCADE_OK) return result;
    neural_initialized_ = true;
    SceneCoordinatorConfig scene_config{};
    scene_config.neural_scenes = &neural_scenes_;
    scene_config.output_scenes = &output_scenes_;
    scene_config.accessibility = config.accessibility;
    scene_config.fusion = config.fusion;
    scene_config.filter = config.scene_filter;
    scene_config.semantic_freshness = config.semantic_freshness;
    scene_config.source = config.source;
    result = scene_.initialize(scene_config, &storage->scene);
    if (result != SACCADE_OK) {
        (void)neural_.shutdown();
        neural_initialized_ = false;
        return result;
    }

    scene_initialized_ = true;
    result = session_.initialize(&output_scenes_, &storage->session, config.executor);
    if (result != SACCADE_OK) {
        (void)scene_.shutdown();
        (void)neural_.shutdown();
        scene_initialized_ = false;
        neural_initialized_ = false;
        return result;
    }
    session_initialized_ = true;

    const InteractionControllerSink sink{this, forward, input_lease_active, neutralize_input};
    result = interaction_.initialize(&session_, config.interaction, {this, read_state}, sink);
    if (result != SACCADE_OK) {
        (void)session_.shutdown();
        (void)scene_.shutdown();
        (void)neural_.shutdown();
        session_initialized_ = false;
        scene_initialized_ = false;
        neural_initialized_ = false;
        return result;
    }
    interaction_initialized_ = true;
    initialized_ = true;
    trace_.record(DebugTraceCode::runtime_initialized, 0);
    return SACCADE_OK;
}

SaccadeResult DesktopRuntime::offer(scheduler::DesktopNeuralFrame frame) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = neural_.offer(frame);
    if (result != SACCADE_OK) return fail(result);
    ++stats_.frames_offered;
    trace_.record(DebugTraceCode::frame_offered, 0, frame.source_id);
    return SACCADE_OK;
}

SaccadeResult DesktopRuntime::request_semantic(const SaccadeAccessibilityQueryDesc& query) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.request_semantic(query);
    if (result != SACCADE_OK) return fail(result);
    ++stats_.semantic_requests;
    trace_.record(DebugTraceCode::semantic_requested, 0, query.window_id);
    return SACCADE_OK;
}

SaccadeResult DesktopRuntime::cancel_semantic() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.cancel_semantic();
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_source(SceneSource source) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.set_source(source);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_interaction_profile(InteractionProfile profile) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = interaction_.set_profile(profile);
    if (result == SACCADE_OK) config_.interaction = profile;
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_text(SaccadeSpanU8 text) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = interaction_.set_text(text);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_target_filter(TargetFilterConfig filter) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.set_filter(filter);
    if (result == SACCADE_OK) config_.scene_filter = filter;
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_fusion(scene::FusionConfig fusion) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.set_fusion(fusion);
    if (result == SACCADE_OK) config_.fusion = fusion;
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::set_scope(const geometry::RectQ8* scope) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.set_scope(scope);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::publish_grid(scene::GridSceneConfig config, SceneCoordinatorAdvance* output) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.publish_grid(config, output);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::publish_windows(scene::WindowSceneConfig config, const SaccadeWindowInfo* windows,
                                              uint32_t count, SceneCoordinatorAdvance* output) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = scene_.publish_windows(config, windows, count, output);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::advance(uint64_t now_ns, DesktopRuntimeAdvance* output) noexcept {
    if (!initialized_ || now_ns == 0 || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    ++stats_.advances;
    SaccadeResult result = neural_.advance(now_ns, &output->neural);
    if (result != SACCADE_OK) return fail(result);
    result = scene_.advance(&output->scene);
    if (result != SACCADE_OK) return fail(result);
    if (output->neural.interaction_due) {
        result = interaction_.tick(now_ns);
        if (result != SACCADE_OK && result != SACCADE_ERROR_STALE_HANDLE && result != SACCADE_ERROR_PERMISSION)
            return fail(result);
        output->interaction_ticked = true;
    }
    if (output->neural.scene_published || output->scene.scene_published)
        trace_.record(DebugTraceCode::scene_published, now_ns,
                      output->neural.scene_published ? output->neural.target_count : output->scene.target_count);
    return result;
}

SaccadeResult DesktopRuntime::dispatch(Command command, uint64_t now_ns, InteractionCommandResult* output) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    ++stats_.commands;
    const SaccadeResult result = interaction_.dispatch(command, now_ns, output);
    trace_.record(DebugTraceCode::command_dispatched, now_ns, static_cast<uint32_t>(command), result);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::enter_symbol(uint16_t symbol, uint64_t now_ns, SessionEvent* output) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    ++stats_.symbols;
    const SaccadeResult result = session_.enter_symbol(symbol, now_ns, output);
    trace_.record(DebugTraceCode::symbol_entered, now_ns, symbol, result);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::observe_physical_input(uint64_t now_ns) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = interaction_.observe_physical_input(now_ns);
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopRuntime::acquire_scene(scene::PacketView* output) noexcept {
    return initialized_ ? output_scenes_.acquire_latest(output) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopRuntime::compose_overlay(const OverlayComposeConfig& config, OverlayComposeWorkspace* workspace,
                                              SaccadeMutableSpanU8 output, OverlayComposeResult* result) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (!session_.active()) return SACCADE_ERROR_NOT_FOUND;

    OverlayComposeConfig resolved = config;
    const interaction::SelectionView selection = session_.selection();
    if (selection.target_count != 0) resolved.active_target_id = selection.target_ids[selection.target_count - 1U];

    const SaccadeResult composed = overlay_.compose(session_.scene_view(), session_.labels(), session_.label_count(),
                                                    resolved, workspace, output, result);
    if (composed != SACCADE_OK) return fail(composed);
    ++stats_.overlay_compositions;
    trace_.record(DebugTraceCode::overlay_composed, 0, result->target_count);
    return SACCADE_OK;
}

SaccadeResult DesktopRuntime::capture_debugger_scene(Debugger* debugger) noexcept {
    return capture_debugger_scene(debugger, {});
}

SaccadeResult DesktopRuntime::capture_debugger_scene(Debugger* debugger,
                                                     const DebuggerCaptureContext& context) noexcept {
    if (!initialized_ || debugger == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    scene::PacketView latest{};
    const SaccadeResult acquired = output_scenes_.acquire_latest(&latest);
    if (acquired != SACCADE_OK) return acquired;

    DebuggerCaptureContext resolved = context;
    const scene::FusionStats fusion = scene_.latest_fusion_stats();
    const uint32_t fusion_input_count = scene_.latest_fusion_input_count();
    if (resolved.fusion == nullptr && fusion_input_count != 0) {
        resolved.fusion = &fusion;
        resolved.fusion_input_count = fusion_input_count;
    }
    return debugger->capture_scene(latest, resolved);
}

SaccadeResult DesktopRuntime::read_environment(InteractionState* output) noexcept {
    SaccadeResult result = config_.environment.read(config_.environment.context, output);
    if (result != SACCADE_OK) return result;
    scene::PacketView latest{};
    result = output_scenes_.acquire_latest(&latest);
    if (result != SACCADE_OK) return result;
    output->scene_epoch = latest.header->scene_epoch;
    output->transform_epoch = latest.header->transform_epoch;
    output->topology_epoch = latest.header->topology_epoch;
    return SACCADE_OK;
}

SaccadeResult DesktopRuntime::read_state(void* context, InteractionState* output) noexcept {
    if (context == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    return static_cast<DesktopRuntime*>(context)->read_environment(output);
}

SaccadeResult DesktopRuntime::forward_command(Command command, uint64_t now_ns) noexcept {
    SessionEvent event{};
    switch (command) {
    case Command::confirm:
        return session_.confirm(now_ns, &event);
    case Command::backspace:
        return session_.backspace(&event);
    case Command::cancel:
        return session_.cancel(interaction::SelectionCancelReason::user);
    default:
        return config_.sink.forward == nullptr ? SACCADE_ERROR_NOT_FOUND
                                               : config_.sink.forward(config_.sink.context, command, now_ns);
    }
}

SaccadeResult DesktopRuntime::forward(void* context, Command command, uint64_t now_ns) noexcept {
    if (context == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    return static_cast<DesktopRuntime*>(context)->forward_command(command, now_ns);
}

bool DesktopRuntime::input_lease_active(void* context) noexcept {
    auto* runtime = static_cast<DesktopRuntime*>(context);
    return runtime != nullptr && runtime->config_.sink.input_lease_active != nullptr &&
           runtime->config_.sink.input_lease_active(runtime->config_.sink.context);
}

SaccadeResult DesktopRuntime::neutralize_input(void* context) noexcept {
    auto* runtime = static_cast<DesktopRuntime*>(context);
    if (runtime == nullptr || runtime->config_.sink.neutralize_input == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    return runtime->config_.sink.neutralize_input(runtime->config_.sink.context);
}

SaccadeResult DesktopRuntime::shutdown() noexcept {
    if (!initialized_ && !neural_initialized_ && !scene_initialized_ && !session_initialized_ &&
        !interaction_initialized_)
        return SACCADE_OK;
    SaccadeResult result = SACCADE_OK;
    if (interaction_initialized_) {
        result = interaction_.shutdown();
        if (result == SACCADE_OK) interaction_initialized_ = false;
    }
    if (result == SACCADE_OK && session_initialized_) {
        result = session_.shutdown();
        if (result == SACCADE_OK) session_initialized_ = false;
    }
    if (result == SACCADE_OK && scene_initialized_) {
        result = scene_.shutdown();
        if (result == SACCADE_OK) scene_initialized_ = false;
    }
    if (result == SACCADE_OK && neural_initialized_) {
        result = neural_.shutdown();
        if (result == SACCADE_OK) neural_initialized_ = false;
    }
    if (result != SACCADE_OK) return fail(result);
    trace_.record(DebugTraceCode::runtime_shutdown, 0);
    config_ = {};
    initialized_ = false;
    return SACCADE_OK;
}

} // namespace saccade::application

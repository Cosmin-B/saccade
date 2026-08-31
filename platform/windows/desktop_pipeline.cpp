#include "platform/windows/desktop_pipeline.hpp"

#include "input/execution_preflight.hpp"
#include "platform/windows/glyph_atlas.hpp"
#include "platform/windows/keyboard.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace saccade::platform::windows {
namespace {

constexpr uint64_t activation_timeout_ns = UINT64_C(2'000'000'000);
constexpr uint64_t keyboard_layout_check_period_ns = UINT64_C(250'000'000);
constexpr uint64_t desktop_source_id = UINT64_C(0x5341434341444501);
constexpr uint64_t grid_source_id = UINT64_C(0x5341434341444502);
constexpr uint64_t window_source_id = UINT64_C(0x5341434341444503);

void preserve_first_error(SaccadeResult candidate, SaccadeResult* first) noexcept {
    if (*first == SACCADE_OK && candidate != SACCADE_OK)
        *first = candidate;
}

bool source_for_command(application::Command command, application::TargetSource* source) noexcept {
    switch (command) {
    case application::Command::source_pixel:
        *source = application::TargetSource::pixel;
        return true;
    case application::Command::source_semantic:
        *source = application::TargetSource::semantic;
        return true;
    case application::Command::source_grid:
        *source = application::TargetSource::grid;
        return true;
    case application::Command::source_fused:
        *source = application::TargetSource::fused;
        return true;
    default:
        return false;
    }
}

bool scope_for_command(application::Command command, application::TargetScope* scope) noexcept {
    switch (command) {
    case application::Command::scope_desktop:
        *scope = application::TargetScope::desktop;
        return true;
    case application::Command::scope_active_window:
        *scope = application::TargetScope::active_window;
        return true;
    case application::Command::scope_monitor:
        *scope = application::TargetScope::monitor;
        return true;
    default:
        return false;
    }
}

bool window_navigation_command(application::Command command) noexcept {
    return command >= application::Command::window_cycle_forward && command <= application::Command::window_activate_down;
}

bool mode_command(application::Command command) noexcept {
    return command >= application::Command::mode_single && command <= application::Command::mode_path;
}

bool intersects(const geometry::RectQ8& left, const geometry::RectQ8& right) noexcept {
    return static_cast<int64_t>(left.x) + left.width > right.x && static_cast<int64_t>(right.x) + right.width > left.x &&
           static_cast<int64_t>(left.y) + left.height > right.y && static_cast<int64_t>(right.y) + right.height > left.y;
}

uint64_t display_id_for_window(HWND window) noexcept {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr)
        return 0;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) == 0 ? 0 : stable_display_id(info.szDevice);
}

application::SceneSource scene_source(application::TargetSource source) noexcept {
    switch (source) {
    case application::TargetSource::pixel:
        return application::SceneSource::pixel;
    case application::TargetSource::semantic:
        return application::SceneSource::semantic;
    case application::TargetSource::grid:
        return application::SceneSource::grid;
    case application::TargetSource::fused:
        return application::SceneSource::fused;
    }
    return application::SceneSource::pixel;
}

application::TargetFilterConfig target_filter(const application::DetectorSettings& detector) noexcept {
    application::TargetFilterConfig filter{};
    filter.confidence_q16 = detector.confidence_q16;
    filter.text_confidence_q16 = detector.text_sensitivity_q16;
    filter.minimum_width_q8 = detector.minimum_width_q8;
    filter.minimum_height_q8 = detector.minimum_height_q8;
    if (detector.merge_policy == application::MergePolicy::text_first)
        filter.order = application::TargetOrderPolicy::text_first;
    else if (detector.merge_policy == application::MergePolicy::controls_first)
        filter.order = application::TargetOrderPolicy::controls_first;
    return filter;
}

scene::FusionConfig fusion_config(const application::DetectorSettings& detector, uint32_t maximum_targets) noexcept {
    scene::FusionConfig config{};
    config.maximum_targets = maximum_targets;
    config.iou_threshold_q16 = detector.duplicate_iou_q16;
    config.merge_duplicates = detector.merge_policy != application::MergePolicy::disabled;
    return config;
}

application::LabelPlacement label_placement(const application::SettingsDocument& settings) noexcept {
    const application::HintPlacement placement =
        settings.appearance.placement == application::HintPlacement::automatic ? settings.hints.placement : settings.appearance.placement;
    return static_cast<application::LabelPlacement>(placement);
}

backend::d3d12::DirectMlExecutionPolicy execution_policy(application::ComputePolicy policy) noexcept {
    if (policy == application::ComputePolicy::cpu_only)
        return backend::d3d12::DirectMlExecutionPolicy::software_only;
    if (policy == application::ComputePolicy::automatic)
        return backend::d3d12::DirectMlExecutionPolicy::hardware_then_software;
    return backend::d3d12::DirectMlExecutionPolicy::hardware_only;
}

uint32_t required_compute_capability(application::ComputePolicy policy) noexcept {
    if (policy == application::ComputePolicy::cpu_only)
        return SACCADE_PROVIDER_CAPABILITY_CPU;
    if (policy == application::ComputePolicy::cpu_and_gpu || policy == application::ComputePolicy::named_device)
        return SACCADE_PROVIDER_CAPABILITY_GPU;
    if (policy == application::ComputePolicy::cpu_and_accelerator)
        return SACCADE_PROVIDER_CAPABILITY_ACCELERATOR;
    return 0;
}

bool secure_desktop() noexcept {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr)
        return true;
    wchar_t name[64]{};
    DWORD required = 0;
    const BOOL read = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &required);
    (void)CloseDesktop(desktop);
    return read == 0 || std::wcscmp(name, L"Default") != 0;
}

struct SurfaceQualification {
    ActionPointDisposition disposition = ActionPointDisposition::unavailable;
    uint32_t reason_bits = surface_reason_none;
};

SurfaceQualification qualify_surface(const ActionPointQualifier& qualifier) noexcept {
    if (secure_desktop()) {
        return {ActionPointDisposition::secure, surface_reason_secure_desktop};
    }

    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return {ActionPointDisposition::unavailable, surface_reason_foreground_missing};
    }

    const ActionPointDisposition disposition = qualifier.qualify_focus(reinterpret_cast<uintptr_t>(foreground));
    if (disposition == ActionPointDisposition::secure) {
        return {disposition, surface_reason_secure_focus};
    }
    if (disposition == ActionPointDisposition::unavailable) {
        return {disposition, surface_reason_focus_unavailable};
    }
    return {ActionPointDisposition::qualified, surface_reason_none};
}

bool target_window_available(uint64_t window_id) noexcept {
    if (window_id == 0)
        return true;
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(window_id));
    return IsWindow(window) != 0 && (IsWindowVisible(window) != 0 || IsIconic(window) != 0);
}

bool q8(int32_t value, int32_t* output) noexcept {
    const int64_t scaled = static_cast<int64_t>(value) * 256;
    if (scaled < INT32_MIN || scaled > INT32_MAX)
        return false;
    *output = static_cast<int32_t>(scaled);
    return true;
}

bool virtual_desktop(const geometry::DisplaySnapshot& snapshot, VirtualDesktop* output) noexcept {
    if (output == nullptr || snapshot.epoch == 0 || snapshot.count == 0)
        return false;
    int64_t left = INT64_MAX;
    int64_t top = INT64_MAX;
    int64_t right = INT64_MIN;
    int64_t bottom = INT64_MIN;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::RectQ8& bounds = snapshot.displays[index].desktop_bounds;
        if (!geometry::rect_valid(bounds) || bounds.x % 256 != 0 || bounds.y % 256 != 0 || bounds.width % 256 != 0 ||
            bounds.height % 256 != 0)
            return false;
        left = std::min<int64_t>(left, bounds.x / 256);
        top = std::min<int64_t>(top, bounds.y / 256);
        right = std::max<int64_t>(right, (static_cast<int64_t>(bounds.x) + bounds.width) / 256);
        bottom = std::max<int64_t>(bottom, (static_cast<int64_t>(bounds.y) + bounds.height) / 256);
    }
    if (left < INT32_MIN || top < INT32_MIN || right <= left || bottom <= top || static_cast<uint64_t>(right - left) > UINT32_MAX ||
        static_cast<uint64_t>(bottom - top) > UINT32_MAX)
        return false;
    *output = {static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<uint32_t>(right - left),
               static_cast<uint32_t>(bottom - top), snapshot.epoch};
    return true;
}

geometry::PointQ8 pointer_position() noexcept {
    POINT point{};
    geometry::PointQ8 output{};
    if (GetCursorPos(&point) != 0) {
        (void)q8(point.x, &output.x);
        (void)q8(point.y, &output.y);
    }
    return output;
}

geometry::PointQ8 desktop_center(const VirtualDesktop& desktop) noexcept {
    return {static_cast<int32_t>((static_cast<int64_t>(desktop.x) * 256) + static_cast<int64_t>(desktop.width) * 128),
            static_cast<int32_t>((static_cast<int64_t>(desktop.y) * 256) + static_cast<int64_t>(desktop.height) * 128)};
}

bool system_dark_theme() noexcept {
    constexpr wchar_t personalize_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD apps_use_light_theme = 1;
    DWORD size = sizeof(apps_use_light_theme);
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, personalize_key, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &apps_use_light_theme, &size);
    return status == ERROR_SUCCESS && apps_use_light_theme == 0;
}

} // namespace

DesktopPipeline::~DesktopPipeline() {
    (void)shutdown();
}

SaccadeResult DesktopPipeline::set_text(SaccadeSpanU8 text) noexcept {
    return initialized_ ? runtime_.set_text(text) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::fail(SaccadeResult result, DesktopPipelineStage stage) noexcept {
    if (result != SACCADE_OK) {
        last_result_ = result;
        last_stage_ = stage;
        ++stats_.failures;
    }
    return result;
}

SaccadeResult DesktopPipeline::initialize(const DesktopPipelineConfig& config) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.artifact_path == nullptr || config.shader_directory == nullptr || config.settings == nullptr ||
        config.verifier.verify == nullptr || config.start_time_ns == 0 || application::validate_settings(*config.settings) != SACCADE_OK) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeResult result = debugger_.initialized() ? debugger_.clear() : debugger_.initialize(&debugger_storage_);
    if (result != SACCADE_OK)
        return result;
    config_ = config;
    settings_ = *config.settings;
    config_.settings = &settings_;
    result = resolve_hint_language(settings_.hints, &resolved_hints_, &keyboard_layout_token_);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::keys);
    result = artifact_.initialize(config.artifact_path, config.verifier);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::artifact);
    backend::d3d12::DirectMlProviderConfig provider_config{};
    provider_config.shader_directory = config.shader_directory;
    provider_config.verifier = config.verifier;
    provider_config.execution_policy = execution_policy(config.settings->compute.policy);
    if (config.settings->compute.policy == application::ComputePolicy::named_device)
        provider_config.device_stable_id = config.settings->compute.device_stable_id;
    result = provider_.initialize(provider_config);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::provider);
    provider_initialized_ = true;
    const SaccadeInferenceProviderDesc provider = provider_.descriptor();
    application::InferenceRuntimeConfig inference_config{};
    inference_config.provider = provider;
    inference_config.artifact = artifact_.bytes();
    inference_config.model_stable_id = artifact_.view().stable_id;
    inference_config.provider_stable_id = provider.info.stable_id;
    inference_config.device_stable_id = config.settings->compute.device_stable_id;
    inference_config.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC |
                                                required_compute_capability(config.settings->compute.policy);
    inference_config.preferred_capability_bits = SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    inference_config.required_format_bits = SACCADE_FORMAT_BGRA8;
    inference_config.required_precision_bits = artifact_.view().precision_bits;
    inference_config.required_import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    result = inference_.initialize(inference_config);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::inference);
    result = capture_provider_.initialize_native(provider_.adapter_luid());
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::capture_provider);
    result = display_collector_.refresh(&displays_);
    if (result != SACCADE_OK || displays_.snapshot().count == 0)
        return fail(result == SACCADE_OK ? SACCADE_ERROR_NOT_FOUND : result, DesktopPipelineStage::topology);
    result = captures_.initialize(&capture_provider_, 0, 0);
    if (result == SACCADE_OK)
        captures_initialized_ = true;
    if (result == SACCADE_OK)
        result = captures_.synchronize(displays_.snapshot());
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::capture_set);
    result = accessibility_.initialize();
    if (result == SACCADE_OK)
        accessibility_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::accessibility);
    result = action_point_qualifier_.initialize();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    VirtualDesktop desktop{};
    if (!virtual_desktop(displays_.snapshot(), &desktop))
        return fail(SACCADE_ERROR_CAPACITY, DesktopPipelineStage::input);
    const geometry::PointQ8 pointer = pointer_position();
    result = input_.initialize(desktop, {this, submit_with_send_input, activate_window, preflight_input}, permission_epoch_, pointer.x,
                               pointer.y);
    if (result == SACCADE_OK)
        input_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    const NeuralBridgeConfig bridge_config{inference_.runtime(), capture_provider_.device(), capture_provider_.context(),
                                           provider_.graphics_device()};
    result = bridge_.initialize(bridge_config);
    if (result == SACCADE_OK)
        bridge_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::bridge);
    const geometry::PointQ8 center = desktop_center(desktop);
    application::DesktopRuntimeConfig runtime_config{};
    runtime_config.neural.runtime = inference_.runtime();
    runtime_config.neural.session = inference_.session();
    runtime_config.neural.model_epoch = artifact_.view().stable_id;
    runtime_config.neural.session_epoch = config.start_time_ns;
    runtime_config.neural.desktop_source_id = desktop_source_id;
    runtime_config.neural.maximum_output_bytes = inference_.info().max_output_bytes;
    runtime_config.neural.maximum_targets = artifact_.view().max_targets;
    runtime_config.neural.start_time_ns = config.start_time_ns;
    runtime_config.accessibility = accessibility_.descriptor();
    runtime_config.fusion = fusion_config(config.settings->detector, artifact_.view().max_targets);
    runtime_config.scene_filter = target_filter(config.settings->detector);
    runtime_config.semantic_freshness = {this, semantic_query_current};
    runtime_config.source = scene_source(config.settings->source);
    runtime_config.executor = {this, execute_plan};
    runtime_config.interaction =
        application::make_interaction_profile(settings_, resolved_hints_, pointer.x, pointer.y, center.x, center.y, config.start_time_ns);
    runtime_config.environment = {this, read_environment};
    runtime_config.sink = {this, forward_command, input_lease_active, neutralize_input};
    result = runtime_.initialize(runtime_config, &runtime_storage_);
    if (result == SACCADE_OK)
        runtime_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    constexpr SaccadeAgentCapabilityBits agent_capabilities = SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER |
                                                              SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_WINDOW;
    result = agent_.initialize(
        {this, acquire_agent_scene, execute_plan, read_agent_physical_state, abort_agent_input, cycle_agent_window, agent_capabilities});
    if (result == SACCADE_OK)
        agent_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    scope_ = config.settings->scope;
    result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    result = keys_.initialize(&runtime_, {this, route_session_command});
    if (result == SACCADE_OK)
        keys_initialized_ = true;
    if (result == SACCADE_OK)
        result = keys_.replace(settings_.bindings.data(), settings_.binding_count);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::keys);
    ID3D12Device* graphics = provider_.graphics_device();
    if (graphics == nullptr)
        return fail(SACCADE_ERROR_STATE, DesktopPipelineStage::provider);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* overlay_queue = nullptr;
    const HRESULT queue_result = graphics->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&overlay_queue));
    if (FAILED(queue_result) || overlay_queue == nullptr)
        return fail(SACCADE_ERROR_BACKEND, DesktopPipelineStage::provider);
    overlay_queue_ = overlay_queue;
    overlay_style_ = application::resolve_overlay_style(config.settings->appearance, config.settings->flags, system_dark_theme());
    application::SettingsDocument glyph_settings = settings_;
    glyph_settings.hints = resolved_hints_;
    result = rasterize_glyph_atlas(glyph_settings, &glyph_atlases_[0]);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::overlay);
    current_glyph_atlas_ = 0;
    result = overlays_.initialize(graphics, overlay_queue, config.shader_directory, {this, load_overlay, observe_overlay});
    if (result == SACCADE_OK)
        overlays_initialized_ = true;
    if (result == SACCADE_OK)
        result = overlays_.set_glyph_atlas(glyph_atlases_[current_glyph_atlas_].view());
    if (result == SACCADE_OK)
        result = overlays_.synchronize(displays_.snapshot());
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::overlay);
    overlay_frames_.fill(OverlayFrameSlot{});
    for (uint32_t index = 0; index < displays_.snapshot().count; ++index)
        overlay_frames_[index].display_id_ = displays_.snapshot().displays[index].display_id;
    source_ = config.settings->source;
    next_capture_ns_ = config.start_time_ns;
    next_keyboard_layout_check_ns_ = config.start_time_ns + keyboard_layout_check_period_ns;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::apply_settings(const application::SettingsDocument& settings, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0 || application::validate_settings(settings) != SACCADE_OK)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (active())
        return SACCADE_ERROR_BUSY;
    if (settings.compute.policy != settings_.compute.policy || settings.compute.device_stable_id != settings_.compute.device_stable_id) {
        const application::SettingsDocument previous = settings_;
        DesktopPipelineConfig restart = config_;
        restart.start_time_ns = now_ns;
        SaccadeResult result = shutdown();
        if (result != SACCADE_OK)
            return result;
        restart.settings = &settings;
        result = initialize(restart);
        if (result == SACCADE_OK)
            return SACCADE_OK;
        const SaccadeResult requested_result = result;
        (void)shutdown();
        restart.settings = &previous;
        (void)initialize(restart);
        return requested_result;
    }

    const application::SettingsDocument previous = settings_;
    const application::TargetSource previous_source = source_;
    const application::TargetScope previous_scope = scope_;
    const application::HintSettings previous_hints = resolved_hints_;
    const uint64_t previous_layout_token = keyboard_layout_token_;
    const SaccadeOverlayStyle previous_style = overlay_style_;
    const uint32_t previous_glyph_atlas = current_glyph_atlas_;
    const uint32_t pending_glyph_atlas = previous_glyph_atlas == 0 ? 1U : 0U;
    application::HintSettings pending_hints{};
    uint64_t pending_layout_token = 0;
    SaccadeResult result = resolve_hint_language(settings.hints, &pending_hints, &pending_layout_token);
    if (result != SACCADE_OK)
        return result;
    application::SettingsDocument glyph_settings = settings;
    glyph_settings.hints = pending_hints;
    result = rasterize_glyph_atlas(glyph_settings, &glyph_atlases_[pending_glyph_atlas]);
    if (result != SACCADE_OK)
        return result;
    VirtualDesktop desktop{};
    if (!virtual_desktop(displays_.snapshot(), &desktop))
        return SACCADE_ERROR_STATE;
    const geometry::PointQ8 pointer = pointer_position();
    const geometry::PointQ8 center = desktop_center(desktop);
    settings_ = settings;
    resolved_hints_ = pending_hints;
    keyboard_layout_token_ = pending_layout_token;
    source_ = settings.source;
    scope_ = settings.scope;
    overlay_style_ = application::resolve_overlay_style(settings.appearance, settings.flags, system_dark_theme());
    result = runtime_.set_interaction_profile(
        application::make_interaction_profile(settings_, resolved_hints_, pointer.x, pointer.y, center.x, center.y, now_ns));
    if (result == SACCADE_OK)
        result = runtime_.set_target_filter(target_filter(settings.detector));
    if (result == SACCADE_OK)
        result = runtime_.set_fusion(fusion_config(settings.detector, artifact_.view().max_targets));
    if (result == SACCADE_OK)
        result = runtime_.set_source(scene_source(source_));
    if (result == SACCADE_OK)
        result = apply_scope();
    if (result == SACCADE_OK)
        result = keys_.replace(settings.bindings.data(), settings.binding_count);
    if (result == SACCADE_OK)
        result = overlays_.set_glyph_atlas(glyph_atlases_[pending_glyph_atlas].view());
    if (result == SACCADE_OK) {
        if ((previous_source == application::TargetSource::semantic || previous_source == application::TargetSource::fused) &&
            source_ != application::TargetSource::semantic && source_ != application::TargetSource::fused) {
            semantic_window_id_ = 0;
        }
        if (previous_scope != scope_)
            semantic_window_id_ = 0;
        current_glyph_atlas_ = pending_glyph_atlas;
        next_keyboard_layout_check_ns_ = now_ns + keyboard_layout_check_period_ns;
        overlay_dirty_ = true;
        return SACCADE_OK;
    }

    settings_ = previous;
    resolved_hints_ = previous_hints;
    keyboard_layout_token_ = previous_layout_token;
    source_ = previous_source;
    scope_ = previous_scope;
    overlay_style_ = previous_style;
    current_glyph_atlas_ = previous_glyph_atlas;
    (void)runtime_.set_interaction_profile(
        application::make_interaction_profile(previous, previous_hints, pointer.x, pointer.y, center.x, center.y, now_ns));
    (void)runtime_.set_target_filter(target_filter(previous.detector));
    (void)runtime_.set_fusion(fusion_config(previous.detector, artifact_.view().max_targets));
    (void)runtime_.set_source(scene_source(previous_source));
    (void)apply_scope();
    (void)keys_.replace(previous.bindings.data(), previous.binding_count);
    return fail(result, DesktopPipelineStage::runtime);
}

SaccadeResult DesktopPipeline::refresh_hint_language(uint64_t now_ns) noexcept {
    if (now_ns < next_keyboard_layout_check_ns_)
        return SACCADE_OK;
    next_keyboard_layout_check_ns_ =
        now_ns > UINT64_MAX - keyboard_layout_check_period_ns ? UINT64_MAX : now_ns + keyboard_layout_check_period_ns;
    if (std::strcmp(settings_.hints.language.data(), "und") != 0)
        return SACCADE_OK;

    const uint64_t current = active_keyboard_layout_token();
    if (current == 0)
        return SACCADE_ERROR_BACKEND;
    if (current == keyboard_layout_token_ || runtime_.active())
        return SACCADE_OK;
    return apply_settings(settings_, now_ns);
}

SaccadeResult DesktopPipeline::publish_overlays() noexcept {
    if (!runtime_.active())
        return SACCADE_ERROR_NOT_FOUND;
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    size_t offset = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        OverlayFrameSlot& slot = overlay_frames_[index];
        SaccadeResult result = geometry::make_desktop_to_surface_transform(snapshot.displays[index], snapshot.epoch, &overlay_transform_);
        if (result != SACCADE_OK)
            return result;
        application::OverlayComposeConfig config{};
        config.display_id = snapshot.displays[index].display_id;
        config.transform_epoch = snapshot.epoch;
        config.desktop_to_surface = &overlay_transform_;
        config.styles = &overlay_style_;
        const overlay::GlyphAtlasStorage& glyph_atlas = glyph_atlases_[current_glyph_atlas_];
        config.glyph_symbols = glyph_atlas.symbols.data();
        config.style_count = 1;
        config.glyph_symbol_count = glyph_atlas.glyph_count;
        config.placement = label_placement(settings_);
        config.role_styles.fill(0);
        application::OverlayComposeResult composed{};
        result = runtime_.compose_overlay(config, &overlay_workspace_, {overlay_arena_.data() + offset, overlay_arena_.size() - offset},
                                          &composed);
        if (result != SACCADE_OK)
            return result;
        const auto* header = reinterpret_cast<const SaccadeOverlayPacketHeader*>(overlay_arena_.data() + offset);
        slot.offset_ = offset;
        slot.byte_size_ = composed.byte_size;
        slot.scene_epoch_ = header->scene_epoch;
        slot.transform_epoch_ = header->transform_epoch;
        slot.display_id_ = snapshot.displays[index].display_id;
        slot.active_target_index_ = composed.active_target_index;
        slot.reveal_ticks_ = (overlay_style_.flags & SACCADE_OVERLAY_STYLE_ANIMATED) != 0 ? 16U : 0U;
        slot.present_pending_ = true;
        offset += composed.byte_size;
    }
    overlay_dirty_ = false;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::start_overlay() noexcept {
    if (overlay_dirty_) {
        const SaccadeResult published = publish_overlays();
        if (published != SACCADE_OK)
            return fail(published, DesktopPipelineStage::overlay);
    }
    if (overlay_running_)
        return SACCADE_OK;
    const SaccadeResult result = overlays_.start();
    if (result != SACCADE_OK)
        return fail(result);
    overlay_running_ = true;
    ++stats_.overlay_starts;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::stop_overlay() noexcept {
    if (!overlay_running_)
        return SACCADE_OK;
    const SaccadeResult result = overlays_.stop();
    if (result != SACCADE_OK)
        return fail(result);
    overlay_running_ = false;
    ++stats_.overlay_stops;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::start_capture() noexcept {
    if (capture_running_)
        return SACCADE_OK;
    const SaccadeResult result = captures_.set_running(true);
    if (result != SACCADE_OK)
        return fail(result);
    capture_running_ = true;
    ++stats_.capture_starts;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::stop_capture(bool* stopped) noexcept {
    *stopped = false;
    if (!capture_running_ || pending_command_ || runtime_.active() || input_.synthetic_input_active())
        return SACCADE_OK;
    const SaccadeResult result = captures_.set_running(false);
    if (result != SACCADE_OK)
        return fail(result);
    capture_running_ = false;
    *stopped = true;
    ++stats_.capture_stops;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::request(application::Command command, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (topology_pending_) {
        const SaccadeResult synchronized = synchronize_topology();
        if (synchronized != SACCADE_OK)
            return synchronized;
    }
    if (!input_available_)
        return SACCADE_ERROR_PERMISSION;
    if (qualify_surface(action_point_qualifier_).disposition != ActionPointDisposition::qualified) {
        const SaccadeResult result = set_input_available(false, now_ns);
        return result == SACCADE_OK ? SACCADE_ERROR_PERMISSION : result;
    }
    application::TargetSource requested_source{};
    if (source_for_command(command, &requested_source))
        return change_source(requested_source, now_ns);
    if (command == application::Command::scope_toggle) {
        const application::TargetScope next =
            scope_ == application::TargetScope::active_window ? application::TargetScope::desktop : application::TargetScope::active_window;
        return change_scope(next, now_ns);
    }
    application::TargetScope requested_scope{};
    if (scope_for_command(command, &requested_scope))
        return change_scope(requested_scope, now_ns);
    if (mode_command(command) && runtime_.active()) {
        application::InteractionCommandResult output{};
        SaccadeResult result = runtime_.dispatch(command, now_ns, &output);
        if (result != SACCADE_OK)
            return fail(result);
        (void)stop_overlay();
        return restart_action(now_ns);
    }
    if (command == application::Command::free_pointer) {
        if (pending_command_ || runtime_.active())
            return SACCADE_ERROR_BUSY;
        SaccadeResult result = apply_scope();
        if (result != SACCADE_OK)
            return fail(result);
        if (source_ != application::TargetSource::grid) {
            result = runtime_.set_source(application::SceneSource::grid);
            if (result != SACCADE_OK)
                return fail(result);
            source_ = application::TargetSource::grid;
        }
        return begin_grid_action(command, now_ns);
    }
    if (window_navigation_command(command))
        return navigate_window(command, now_ns, false);
    if (command == application::Command::window_activate) {
        if (pending_command_ || runtime_.active())
            return SACCADE_ERROR_BUSY;
        return begin_window_action(now_ns);
    }
    if (!application::command_targets_scene(command)) {
        application::InteractionCommandResult output{};
        return runtime_.dispatch(command, now_ns, &output);
    }
    if (pending_command_ || runtime_.active())
        return SACCADE_ERROR_BUSY;
    SaccadeResult source_result = runtime_.set_source(scene_source(source_));
    if (source_result != SACCADE_OK)
        return fail(source_result);
    SaccadeResult result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result);
    if (source_ == application::TargetSource::grid)
        return begin_grid_action(command, now_ns);
    result = start_capture();
    if (result != SACCADE_OK)
        return result;
    pending_ = command;
    pending_command_ = true;
    pending_deadline_ns_ = now_ns + activation_timeout_ns;
    if (pending_deadline_ns_ <= now_ns)
        return fail(SACCADE_ERROR_CAPACITY);
    next_capture_ns_ = now_ns;
    ++stats_.activations;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::begin_window_action(uint64_t now_ns) noexcept {
    SaccadeResult result = window_navigator_.collect(accessibility_.descriptor(), GetCurrentProcessId(), &windows_);
    if (result != SACCADE_OK)
        return fail(result);
    result = runtime_.set_source(application::SceneSource::windows);
    if (result != SACCADE_OK)
        return fail(result);
    scene::WindowSceneConfig config{};
    config.frame_id = next_window_frame_id_++;
    if (next_window_frame_id_ == 0)
        ++next_window_frame_id_;
    config.model_epoch = artifact_.view().stable_id;
    config.session_epoch = config_.start_time_ns;
    config.transform_epoch = displays_.snapshot().epoch;
    config.topology_epoch = displays_.snapshot().epoch;
    config.source_id = window_source_id;
    application::SceneCoordinatorAdvance scene{};
    result = runtime_.publish_windows(config, windows_.windows.data(), windows_.count, &scene);
    if (result != SACCADE_OK)
        return fail(result);
    application::InteractionCommandResult action{};
    result = runtime_.dispatch(application::Command::window_activate, now_ns, &action);
    if (result != SACCADE_OK)
        return fail(result);
    window_scene_active_ = action.action_started;
    return action.action_started ? start_overlay() : SACCADE_OK;
}

SaccadeResult DesktopPipeline::navigate_window(application::Command command, uint64_t now_ns, bool agent_preflight) noexcept {
    const bool restart = now_ns != 0 && runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    const uint64_t expected_topology_epoch = displays_.snapshot().epoch;
    const uint64_t expected_permission_epoch = permission_epoch_;
    const uint64_t current = reinterpret_cast<uintptr_t>(GetForegroundWindow());
    SaccadeResult result = window_navigator_.collect(accessibility_.descriptor(), GetCurrentProcessId(), &windows_);
    if (result != SACCADE_OK)
        return fail(result);
    uint64_t selected = 0;
    if (command == application::Command::window_cycle_forward)
        result = window_navigator_.cycle(windows_, current, true, &selected);
    else if (command == application::Command::window_cycle_backward)
        result = window_navigator_.cycle(windows_, current, false, &selected);
    else if (command == application::Command::window_activate_behind)
        result = window_navigator_.behind(windows_, current, &selected);
    else {
        const application::WindowDirection direction =
            command == application::Command::window_activate_left    ? application::WindowDirection::left
            : command == application::Command::window_activate_right ? application::WindowDirection::right
            : command == application::Command::window_activate_up    ? application::WindowDirection::up
                                                                     : application::WindowDirection::down;
        result = window_navigator_.directional(windows_, current, direction, &selected);
    }
    if (result != SACCADE_OK)
        return fail(result);
    if (agent_preflight) {
        result = refresh_topology();
        if (result != SACCADE_OK)
            return fail(result, DesktopPipelineStage::topology);
        if (topology_pending_ || displays_.snapshot().epoch != expected_topology_epoch || permission_epoch_ != expected_permission_epoch ||
            reinterpret_cast<uintptr_t>(GetForegroundWindow()) != current) {
            return fail(SACCADE_ERROR_STALE_HANDLE, DesktopPipelineStage::input);
        }
        if (!input_available_ || qualify_surface(action_point_qualifier_).disposition != ActionPointDisposition::qualified) {
            return fail(SACCADE_ERROR_PERMISSION, DesktopPipelineStage::input);
        }
        if (!target_window_available(selected))
            return fail(SACCADE_ERROR_NOT_FOUND, DesktopPipelineStage::input);
    }
    result = activate_window(nullptr, selected);
    if (result != SACCADE_OK)
        return fail(result);
    result = invalidate_semantic();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::accessibility);
    if (scope_ == application::TargetScope::active_window) {
        result = apply_scope();
        if (result != SACCADE_OK)
            return fail(result);
    }
    if (!restart)
        return SACCADE_OK;
    (void)stop_overlay();
    return restart_action(now_ns);
}

SaccadeResult DesktopPipeline::resolve_scope(geometry::RectQ8* output, uint64_t* display_id) noexcept {
    *display_id = 0;
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    if (scope_ == application::TargetScope::active_window) {
        HWND window = GetForegroundWindow();
        RECT bounds{};
        if (window == nullptr || GetWindowRect(window, &bounds) == 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
            !q8(bounds.left, &output->x) || !q8(bounds.top, &output->y) || !q8(bounds.right - bounds.left, &output->width) ||
            !q8(bounds.bottom - bounds.top, &output->height))
            return SACCADE_ERROR_NOT_FOUND;
        return SACCADE_OK;
    }
    if (scope_ == application::TargetScope::monitor) {
        const geometry::DisplaySurface* display = nullptr;
        if (config_.settings->monitor_stable_id != 0) {
            display = displays_.find(config_.settings->monitor_stable_id);
        } else {
            const geometry::PointQ8 pointer = pointer_position();
            for (uint32_t index = 0; index < snapshot.count; ++index) {
                const geometry::RectQ8& bounds = snapshot.displays[index].desktop_bounds;
                if (pointer.x >= bounds.x && pointer.y >= bounds.y &&
                    static_cast<int64_t>(pointer.x) < static_cast<int64_t>(bounds.x) + bounds.width &&
                    static_cast<int64_t>(pointer.y) < static_cast<int64_t>(bounds.y) + bounds.height) {
                    display = &snapshot.displays[index];
                    break;
                }
            }
        }
        if (display == nullptr)
            return SACCADE_ERROR_NOT_FOUND;
        *output = display->desktop_bounds;
        *display_id = display->display_id;
        return SACCADE_OK;
    }
    VirtualDesktop desktop{};
    if (!virtual_desktop(snapshot, &desktop) || desktop.width > INT32_MAX || desktop.height > INT32_MAX || !q8(desktop.x, &output->x) ||
        !q8(desktop.y, &output->y) || !q8(static_cast<int32_t>(desktop.width), &output->width) ||
        !q8(static_cast<int32_t>(desktop.height), &output->height))
        return SACCADE_ERROR_CAPACITY;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::apply_scope() noexcept {
    uint64_t display_id = 0;
    SaccadeResult result = resolve_scope(&scope_rect_, &display_id);
    if (result != SACCADE_OK)
        return result;
    scope_filter_enabled_ = scope_ != application::TargetScope::desktop;
    return runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
}

SaccadeResult DesktopPipeline::publish_grid(application::SceneCoordinatorAdvance* output) noexcept {
    geometry::RectQ8 scope{};
    uint64_t display_id = 0;
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    SaccadeResult result = resolve_scope(&scope, &display_id);
    if (result != SACCADE_OK)
        return result;
    scene::GridSceneConfig grid{};
    grid.scope = scope;
    grid.frame_id = next_grid_frame_id_++;
    if (next_grid_frame_id_ == 0)
        ++next_grid_frame_id_;
    grid.model_epoch = artifact_.view().stable_id;
    grid.session_epoch = config_.start_time_ns;
    grid.transform_epoch = snapshot.epoch;
    grid.topology_epoch = snapshot.epoch;
    grid.source_id = grid_source_id;
    grid.display_id = display_id;
    grid.rows = config_.settings->grid.rows;
    grid.columns = config_.settings->grid.columns;
    grid.margin_x_q8 = config_.settings->grid.margin_x_q8;
    grid.margin_y_q8 = config_.settings->grid.margin_y_q8;
    return runtime_.publish_grid(grid, output);
}

SaccadeResult DesktopPipeline::begin_grid_action(application::Command command, uint64_t now_ns) noexcept {
    application::SceneCoordinatorAdvance scene{};
    SaccadeResult result = publish_grid(&scene);
    if (result != SACCADE_OK)
        return fail(result);
    application::InteractionCommandResult action{};
    result = runtime_.dispatch(command, now_ns, &action);
    if (result != SACCADE_OK)
        return fail(result);
    return action.action_started ? start_overlay() : SACCADE_OK;
}

SaccadeResult DesktopPipeline::restart_action(uint64_t now_ns) noexcept {
    SaccadeResult result = runtime_.set_source(scene_source(source_));
    if (result != SACCADE_OK)
        return fail(result);
    window_scene_active_ = false;
    pending_ = application::Command::repeat_action;
    pending_command_ = true;
    pending_deadline_ns_ = now_ns + activation_timeout_ns;
    if (pending_deadline_ns_ <= now_ns) {
        pending_command_ = false;
        return fail(SACCADE_ERROR_CAPACITY);
    }
    if (source_ == application::TargetSource::grid) {
        pending_command_ = false;
        result = begin_grid_action(application::Command::repeat_action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    if (result != SACCADE_OK) {
        pending_command_ = false;
        return result;
    }
    next_capture_ns_ = now_ns;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::change_source(application::TargetSource source, uint64_t now_ns) noexcept {
    if (source_ == source)
        return SACCADE_OK;
    const bool restart = runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    SaccadeResult result = runtime_.set_source(scene_source(source));
    if (result != SACCADE_OK)
        return fail(result);
    source_ = source;
    if (source_ != application::TargetSource::semantic && source_ != application::TargetSource::fused)
        semantic_window_id_ = 0;
    (void)stop_overlay();
    if (restart)
        return restart_action(now_ns);
    if (!pending_command_)
        return SACCADE_OK;
    if (source_ == application::TargetSource::grid) {
        const application::Command action = pending_;
        pending_command_ = false;
        result = begin_grid_action(action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    next_capture_ns_ = now_ns;
    return result;
}

SaccadeResult DesktopPipeline::change_scope(application::TargetScope scope, uint64_t now_ns) noexcept {
    if (scope_ == scope)
        return SACCADE_OK;
    const bool restart = runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    scope_ = scope;
    SaccadeResult result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result);
    semantic_window_id_ = 0;
    (void)stop_overlay();
    if (restart)
        return restart_action(now_ns);
    if (!pending_command_)
        return SACCADE_OK;
    if (source_ == application::TargetSource::grid) {
        const application::Command action = pending_;
        pending_command_ = false;
        result = begin_grid_action(action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    next_capture_ns_ = now_ns;
    return result;
}

SaccadeResult DesktopPipeline::offer_frames(uint64_t now_ns, uint32_t* offered) noexcept {
    *offered = 0;
    ++stats_.capture_attempts;
    SaccadeResult injected = debugger_.consume_fault(application::DebugFaultPoint::capture);
    if (injected != SACCADE_OK)
        return fail(injected, DesktopPipelineStage::capture_set);
    injected = debugger_.consume_fault(application::DebugFaultPoint::inference);
    if (injected != SACCADE_OK)
        return fail(injected, DesktopPipelineStage::inference);
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    uint32_t source_count = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        source_count += !scope_filter_enabled_ || intersects(display.desktop_bounds, scope_rect_);
    }
    if (source_count == 0)
        return fail(SACCADE_ERROR_NOT_FOUND, DesktopPipelineStage::capture_set);
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        if (scope_filter_enabled_ && !intersects(display.desktop_bounds, scope_rect_))
            continue;
        SceneCaptureFrame capture{};
        SaccadeResult result = captures_.acquire(display.display_id, &capture);
        if (result == SACCADE_ERROR_BUSY)
            continue;
        if (result != SACCADE_OK)
            return fail(result);
        scheduler::DesktopNeuralFrame frame{};
        result = bridge_.import(&captures_, capture, display, snapshot.epoch, &frame);
        if (result != SACCADE_OK) {
            (void)captures_.release(capture);
            return fail(result);
        }
        frame.source_count = source_count;
        result = runtime_.offer(frame);
        if (result != SACCADE_OK) {
            if (result != SACCADE_ERROR_STALE_HANDLE) {
                (void)saccade_frame_release(inference_.runtime(), frame.frame);
                frame.retire(frame.retire_context, frame.frame);
            }
            return fail(result);
        }
        ++*offered;
        ++stats_.frames_offered;
    }
    constexpr uint64_t period = scheduler::scene_period_30hz_ns;
    const uint64_t elapsed = now_ns - next_capture_ns_;
    next_capture_ns_ += (elapsed / period + 1U) * period;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::request_semantic(uint64_t frame_id, uint64_t transform_epoch, uint64_t topology_epoch) noexcept {
    if (runtime_.semantic_running())
        return SACCADE_OK;
    HWND window = GetForegroundWindow();
    RECT bounds{};
    if (window == nullptr || GetWindowRect(window, &bounds) == 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    const int64_t width = static_cast<int64_t>(bounds.right) - bounds.left;
    const int64_t height = static_cast<int64_t>(bounds.bottom) - bounds.top;
    if (width > INT32_MAX || height > INT32_MAX)
        return SACCADE_ERROR_CAPACITY;
    SaccadeAccessibilityQueryDesc query{};
    query.struct_size = sizeof(query);
    query.api_version = SACCADE_API_VERSION;
    query.window_id = reinterpret_cast<uintptr_t>(window);
    query.scope = {bounds.left, bounds.top, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    query.target_capacity = artifact_.view().max_targets;
    query.session_epoch = config_.start_time_ns;
    query.transform_epoch = transform_epoch;
    query.topology_epoch = topology_epoch;
    query.frame_id = frame_id;
    const SaccadeResult result = runtime_.request_semantic(query);
    if (result == SACCADE_OK) {
        semantic_window_id_ = query.window_id;
        ++stats_.semantic_requests;
    }
    return result;
}

SaccadeResult DesktopPipeline::invalidate_semantic() noexcept {
    const SaccadeResult result = runtime_.cancel_semantic();
    if (result == SACCADE_OK)
        semantic_window_id_ = 0;
    return result;
}

SaccadeResult DesktopPipeline::advance(uint64_t now_ns, DesktopPipelineAdvance* output) noexcept {
    if (!initialized_ || output == nullptr || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    ++stats_.advances;
    const SaccadeResult language = refresh_hint_language(now_ns);
    if (language != SACCADE_OK)
        return fail(language, DesktopPipelineStage::keys);
    if (!input_available_ || input_.synthetic_input_active()) {
        const bool surface_qualified = qualify_surface(action_point_qualifier_).disposition == ActionPointDisposition::qualified;
        if (!input_available_) {
            if (!surface_qualified)
                return SACCADE_ERROR_PERMISSION;
            const SaccadeResult restored = set_input_available(true, now_ns);
            if (restored != SACCADE_OK)
                return restored;
        } else if (!surface_qualified) {
            const SaccadeResult lost = set_input_available(false, now_ns);
            return lost == SACCADE_OK ? SACCADE_ERROR_PERMISSION : lost;
        }
    }
    if (topology_pending_) {
        const SaccadeResult synchronized = synchronize_topology();
        if (synchronized != SACCADE_OK && synchronized != SACCADE_ERROR_BUSY) {
            return fail(synchronized, DesktopPipelineStage::topology);
        }
    }
    const bool topology_ready = !topology_pending_;
    if (semantic_window_id_ != 0 && semantic_window_id_ != reinterpret_cast<uintptr_t>(GetForegroundWindow())) {
        const SaccadeResult invalidated = invalidate_semantic();
        if (invalidated != SACCADE_OK)
            return fail(invalidated, DesktopPipelineStage::accessibility);
    }
    if (input_.synthetic_input_active()) {
        InputExecutionResult timed{};
        const SaccadeResult advanced = input_.advance(now_ns, &timed);
        if (advanced != SACCADE_OK && advanced != SACCADE_ERROR_NOT_FOUND)
            return fail(advanced, DesktopPipelineStage::input);
    }
    if (pending_command_ && now_ns >= pending_deadline_ns_) {
        pending_command_ = false;
        bool stopped = false;
        (void)stop_capture(&stopped);
        output->became_idle = stopped;
        return fail(SACCADE_ERROR_TIMEOUT);
    }
    const bool semantic_source = source_ == application::TargetSource::semantic || source_ == application::TargetSource::fused;
    if (topology_ready && capture_running_ && now_ns >= next_capture_ns_ && !(semantic_source && runtime_.semantic_running())) {
        SaccadeResult result = offer_frames(now_ns, &output->frames_offered);
        if (result != SACCADE_OK)
            return result;
    }
    SaccadeResult result = debugger_.consume_fault(application::DebugFaultPoint::scene);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    result = runtime_.advance(now_ns, &output->runtime);
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION)
        return fail(result);
    if (runtime_.active() && (output->runtime.neural.scene_published || output->runtime.scene.scene_published)) {
        overlay_dirty_ = true;
    }
    if (semantic_source && output->runtime.neural.scene_published) {
        result = request_semantic(output->runtime.neural.frame_id, output->runtime.neural.transform_epoch,
                                  output->runtime.neural.topology_epoch);
        if (result != SACCADE_OK)
            return fail(result);
    }
    const bool scene_ready = output->runtime.scene.scene_published && (!semantic_source || output->runtime.scene.semantic_collected);
    if (pending_command_ && scene_ready) {
        application::InteractionCommandResult command{};
        result = runtime_.dispatch(pending_, now_ns, &command);
        if (result != SACCADE_OK)
            return fail(result);
        pending_command_ = false;
        output->command_started = command.action_started;
        if (command.action_started) {
            overlay_dirty_ = true;
            result = start_overlay();
            if (result != SACCADE_OK)
                return result;
        }
    }
    if (topology_ready && overlay_running_) {
        result = debugger_.consume_fault(application::DebugFaultPoint::overlay);
        if (result != SACCADE_OK)
            return fail(result, DesktopPipelineStage::overlay);
        if (overlay_dirty_) {
            result = publish_overlays();
            if (result != SACCADE_OK)
                return fail(result, DesktopPipelineStage::overlay);
        }
        const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
        for (uint32_t index = 0; index < snapshot.count; ++index) {
            OverlayFrameSlot& slot = overlay_frames_[index];
            const bool active_animation = slot.active_target_index_ != SACCADE_OVERLAY_ACTIVE_TARGET_NONE &&
                                          (overlay_style_.flags & SACCADE_OVERLAY_STYLE_ANIMATED) != 0;
            if (!slot.present_pending_ && slot.reveal_ticks_ == 0 && !active_animation)
                continue;
            const SaccadeResult presented = overlays_.present(snapshot.displays[index].display_id, now_ns);
            if (presented != SACCADE_OK && presented != SACCADE_ERROR_BUSY && presented != SACCADE_ERROR_NOT_FOUND)
                return fail(presented);
            if (presented == SACCADE_OK) {
                ++stats_.overlay_presents;
                slot.present_pending_ = false;
                if (slot.reveal_ticks_ != 0)
                    --slot.reveal_ticks_;
            }
        }
    }
    if (!runtime_.active()) {
        if (window_scene_active_) {
            (void)runtime_.set_source(scene_source(source_));
            window_scene_active_ = false;
        }
        const SaccadeResult hidden = stop_overlay();
        if (hidden != SACCADE_OK)
            return hidden;
    }
    bool stopped = false;
    const SaccadeResult idle = stop_capture(&stopped);
    output->became_idle = stopped;
    return idle != SACCADE_OK ? idle : result;
}

bool DesktopPipeline::route_key(const application::KeyEvent& event) noexcept {
    if (!initialized_)
        return false;
    application::SessionKeyRoute route{};
    const bool handled = keys_.route(event, &route) == SACCADE_OK && route.handled;
    if (handled && runtime_.active())
        overlay_dirty_ = true;
    if (handled && route.session_ended) {
        (void)stop_overlay();
        if (window_scene_active_) {
            (void)runtime_.set_source(scene_source(source_));
            window_scene_active_ = false;
        }
        bool stopped = false;
        (void)stop_capture(&stopped);
    }
    return handled;
}

SaccadeResult DesktopPipeline::neutralize_synthetic_input() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    const geometry::PointQ8 pointer = pointer_position();
    const SaccadeResult result = input_.physical_override(pointer.x, pointer.y);
    if (result != SACCADE_OK)
        return fail(result);
    ++stats_.physical_inputs;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::observe_physical_input(uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result = neutralize_synthetic_input();
    if (result != SACCADE_OK)
        return result;
    result = runtime_.observe_physical_input(now_ns);
    if (result != SACCADE_OK)
        return fail(result);
    pending_command_ = false;
    if (window_scene_active_) {
        (void)runtime_.set_source(scene_source(source_));
        window_scene_active_ = false;
    }
    (void)stop_overlay();
    bool stopped = false;
    result = stop_capture(&stopped);
    return result;
}

SaccadeResult DesktopPipeline::set_input_available(bool available, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (input_available_ == available)
        return SACCADE_OK;
    ++permission_epoch_;
    if (permission_epoch_ == 0)
        ++permission_epoch_;
    SaccadeResult result = input_.permission_lost(permission_epoch_);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    input_available_ = available;
    ++stats_.permission_changes;
    if (available)
        return SACCADE_OK;
    result = invalidate_semantic();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::accessibility);
    result = runtime_.observe_physical_input(now_ns);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    pending_command_ = false;
    if (window_scene_active_) {
        (void)runtime_.set_source(scene_source(source_));
        window_scene_active_ = false;
    }
    result = stop_overlay();
    bool stopped = false;
    if (result == SACCADE_OK)
        result = stop_capture(&stopped);
    return result;
}

SaccadeResult DesktopPipeline::process_agent(SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                                             SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    return agent_initialized_ ? agent_.process(request, client_capabilities, now_ns, output, output_size) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::update_input_desktop() noexcept {
    VirtualDesktop desktop{};
    if (!virtual_desktop(displays_.snapshot(), &desktop))
        return SACCADE_ERROR_CAPACITY;
    SaccadeResult result = input_.release_all();
    if (result == SACCADE_OK)
        result = input_.update_desktop(desktop);
    return result;
}

SaccadeResult DesktopPipeline::synchronize_topology() noexcept {
    SaccadeResult result = update_input_desktop();
    if (result == SACCADE_OK)
        result = captures_.synchronize(displays_.snapshot());
    if (result == SACCADE_OK && overlays_initialized_)
        result = overlays_.synchronize(displays_.snapshot());
    if (result == SACCADE_OK) {
        overlay_frames_.fill(OverlayFrameSlot{});
        for (uint32_t index = 0; index < displays_.snapshot().count; ++index)
            overlay_frames_[index].display_id_ = displays_.snapshot().displays[index].display_id;
        overlay_dirty_ = true;
        topology_pending_ = false;
    }
    return result;
}

SaccadeResult DesktopPipeline::refresh_topology() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    ++stats_.topology_refreshes;
    const uint64_t previous = displays_.snapshot().epoch;
    SaccadeResult result = display_collector_.refresh(&displays_);
    if (result != SACCADE_OK)
        return fail(result);
    if (displays_.snapshot().epoch != previous) {
        ++stats_.topology_changes;
        if (runtime_.active()) {
            application::InteractionCommandResult command{};
            (void)runtime_.dispatch(application::Command::cancel, 1, &command);
        }
        pending_command_ = false;
        result = invalidate_semantic();
        if (result != SACCADE_OK)
            return fail(result, DesktopPipelineStage::accessibility);
        topology_pending_ = true;
    }
    if (!topology_pending_)
        return SACCADE_OK;
    result = synchronize_topology();
    if (result == SACCADE_ERROR_BUSY)
        return SACCADE_OK;
    return result == SACCADE_OK ? result : fail(result);
}

SaccadeResult DesktopPipeline::execute_plan(void* context, SaccadeSpanU8 plan, uint32_t permissions, uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    /* Quick early check only. The real validation runs per command in
       input::validate_execution_preflight below. Keep this a subset of it so
       the two cannot disagree. */
    if (!pipeline->input_available_ || qualify_surface(pipeline->action_point_qualifier_).disposition != ActionPointDisposition::qualified)
        return SACCADE_ERROR_PERMISSION;
    const SaccadeResult injected = pipeline->debugger_.consume_fault(application::DebugFaultPoint::input);
    if (injected != SACCADE_OK)
        return pipeline->fail(injected, DesktopPipelineStage::input);
    InputExecutionResult output{};
    const SaccadeResult result = pipeline->input_.execute(plan, permissions, now_ns, &output);
    if (result == SACCADE_OK)
        ++pipeline->stats_.input_plans;
    return result;
}

SaccadeResult DesktopPipeline::preflight_input(void* context, const input::PlanView& plan, uint32_t command_index,
                                               uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    scene::PacketView scene{};
    SaccadeResult result = pipeline->runtime_.acquire_scene(&scene);
    if (result != SACCADE_OK)
        return result;
    const SaccadePhysicalInputState physical = pipeline->input_.physical_state().state();
    input::ExecutionPreflightState state{};
    state.scene = scene;
    const HWND active_window = GetForegroundWindow();
    DWORD active_process = 0;
    if (active_window != nullptr)
        (void)GetWindowThreadProcessId(active_window, &active_process);
    state.focus_id = active_process;
    state.window_id = reinterpret_cast<uintptr_t>(active_window);
    state.topology_epoch = pipeline->displays_.snapshot().epoch;
    state.permission_epoch = pipeline->permission_epoch_;
    state.buttons = physical.buttons;
    state.input_available = pipeline->input_available_;
    state.surface_secure = secure_desktop();
    state.target_window_available = target_window_available(plan.header->window_id);
    state.validate_active_window = command_index < plan.header->command_count && plan.header->window_id != 0 &&
                                   plan.commands[command_index].kind != SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE;
    const bool absolute =
        command_index < plan.header->command_count && (plan.commands[command_index].flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0;
    const bool window_activation =
        command_index < plan.header->command_count && plan.commands[command_index].kind == SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE;
    if (!state.surface_secure && !absolute) {
        const uint64_t qualified_window = plan.header->window_id == 0 || window_activation ? state.window_id : plan.header->window_id;
        state.surface_secure = pipeline->action_point_qualifier_.qualify_focus(qualified_window) != ActionPointDisposition::qualified;
    }
    if (absolute) {
        state.target_point_secure =
            pipeline->action_point_qualifier_.qualify(plan.commands[command_index].x_q8, plan.commands[command_index].y_q8,
                                                      plan.header->window_id) != ActionPointDisposition::qualified;
    }
    state.validate_initial_buttons = command_index == 0;
    return input::validate_execution_preflight(plan, state, now_ns);
}

SaccadeResult DesktopPipeline::read_environment(void* context, application::InteractionState* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    output->permission_epoch = pipeline->permission_epoch_;
    const HWND window = GetForegroundWindow();
    DWORD process_id = 0;
    if (window != nullptr)
        (void)GetWindowThreadProcessId(window, &process_id);
    output->focus_id = process_id;
    if (!pipeline->input_available_ || output->focus_id == 0 ||
        qualify_surface(pipeline->action_point_qualifier_).disposition != ActionPointDisposition::qualified)
        return SACCADE_ERROR_PERMISSION;
    RECT bounds{};
    if (GetWindowRect(window, &bounds) == 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        !q8(bounds.left, &output->window_bounds.x) || !q8(bounds.top, &output->window_bounds.y) ||
        !q8(bounds.right - bounds.left, &output->window_bounds.width) || !q8(bounds.bottom - bounds.top, &output->window_bounds.height))
        return SACCADE_ERROR_NOT_FOUND;
    output->window_id = reinterpret_cast<uintptr_t>(window);
    output->display_id = display_id_for_window(window);
    output->permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT |
                          SACCADE_INPUT_PERMISSION_WINDOW;
    const geometry::PointQ8 pointer = pointer_position();
    output->pointer_x_q8 = pointer.x;
    output->pointer_y_q8 = pointer.y;
    output->expected_buttons = pipeline->input_.physical_state().state().buttons;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::acquire_agent_scene(void* context, const SaccadeAgentScope& scope, scene::PacketView* scene,
                                                   application::InteractionState* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || scene == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (scope.kind == SACCADE_AGENT_SCOPE_WINDOW)
        return SACCADE_ERROR_UNSUPPORTED;

    SaccadeResult result = pipeline->runtime_.acquire_scene(scene);
    if (result != SACCADE_OK)
        return result;

    *output = {};
    output->permission_epoch = pipeline->permission_epoch_;
    const HWND window = GetForegroundWindow();
    DWORD process_id = 0;
    if (window != nullptr)
        (void)GetWindowThreadProcessId(window, &process_id);
    output->focus_id = process_id;
    if (output->focus_id == 0 || qualify_surface(pipeline->action_point_qualifier_).disposition != ActionPointDisposition::qualified) {
        return SACCADE_ERROR_PERMISSION;
    }

    RECT bounds{};
    if (GetWindowRect(window, &bounds) == 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        !q8(bounds.left, &output->window_bounds.x) || !q8(bounds.top, &output->window_bounds.y) ||
        !q8(bounds.right - bounds.left, &output->window_bounds.width) || !q8(bounds.bottom - bounds.top, &output->window_bounds.height)) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    output->window_id = reinterpret_cast<uintptr_t>(window);
    output->process_id = output->focus_id;
    output->foreground_process_id = output->focus_id;
    output->display_id = display_id_for_window(window);
    if (pipeline->input_available_) {
        output->permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT |
                              SACCADE_INPUT_PERMISSION_WINDOW;
    }
    const geometry::PointQ8 pointer = pointer_position();
    output->pointer_x_q8 = pointer.x;
    output->pointer_y_q8 = pointer.y;
    output->expected_buttons = pipeline->input_.physical_state().state().buttons;
    output->scene_epoch = scene->header->scene_epoch;
    output->transform_epoch = scene->header->transform_epoch;
    output->topology_epoch = scene->header->topology_epoch;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::read_agent_physical_state(void* context, SaccadeAgentPhysicalState* output) noexcept {
    if (context == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    const SaccadePhysicalInputState state = pipeline->input_.physical_state().state();
    *output = {};
    output->pointer = {state.pointer_x_q8, state.pointer_y_q8};
    output->buttons = state.buttons;
    output->modifiers = state.modifiers;
    output->active_lease_id = state.active_lease_id;
    output->permission_epoch = state.permission_epoch;
    output->physical_sequence = state.physical_sequence;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::abort_agent_input(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.release_all();
}

SaccadeResult DesktopPipeline::cycle_agent_window(void* context, bool backward) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    return pipeline->navigate_window(backward ? application::Command::window_cycle_backward : application::Command::window_cycle_forward, 0,
                                     true);
}

SaccadeResult DesktopPipeline::forward_command(void* context, application::Command command, uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    return pipeline->config_.forward_shell == nullptr ? SACCADE_ERROR_UNSUPPORTED
                                                      : pipeline->config_.forward_shell(pipeline->config_.shell_context, command, now_ns);
}

SaccadeResult DesktopPipeline::route_session_command(void* context, application::Command command, uint64_t now_ns) noexcept {
    return static_cast<DesktopPipeline*>(context)->request(command, now_ns);
}

bool DesktopPipeline::semantic_query_current(void* context, const SaccadeAccessibilityQueryDesc& query) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || !pipeline->input_available_ || query.session_epoch != pipeline->config_.start_time_ns ||
        query.topology_epoch != pipeline->displays_.snapshot().epoch ||
        (pipeline->source_ != application::TargetSource::semantic && pipeline->source_ != application::TargetSource::fused)) {
        return false;
    }

    const HWND window = GetForegroundWindow();
    if (reinterpret_cast<uintptr_t>(window) != query.window_id)
        return false;
    RECT bounds{};
    return window != nullptr && GetWindowRect(window, &bounds) != 0 && bounds.left == query.scope.x && bounds.top == query.scope.y &&
           bounds.right - bounds.left == query.scope.width && bounds.bottom - bounds.top == query.scope.height;
}

bool DesktopPipeline::input_lease_active(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.synthetic_input_active();
}

SaccadeResult DesktopPipeline::neutralize_input(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.release_all();
}

SaccadeResult DesktopPipeline::activate_window(void*, uint64_t window_id) noexcept {
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(window_id));
    if (window == nullptr || IsWindow(window) == 0)
        return SACCADE_ERROR_NOT_FOUND;
    if (IsIconic(window) != 0)
        (void)ShowWindowAsync(window, SW_RESTORE);
    return SetForegroundWindow(window) != 0 ? SACCADE_OK : SACCADE_ERROR_PERMISSION;
}

SaccadeResult DesktopPipeline::load_overlay(void* context, uint64_t display_id, SaccadeOverlayFrameDesc* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!pipeline->runtime_.active())
        return SACCADE_ERROR_NOT_FOUND;
    const OverlayFrameSlot* slot = nullptr;
    for (const OverlayFrameSlot& candidate : pipeline->overlay_frames_)
        if (candidate.display_id_ == display_id) {
            slot = &candidate;
            break;
        }
    if (slot == nullptr || slot->byte_size_ == 0)
        return SACCADE_ERROR_NOT_FOUND;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = SACCADE_API_VERSION;
    output->scene_epoch = slot->scene_epoch_;
    output->transform_epoch = slot->transform_epoch_;
    output->packet = {pipeline->overlay_arena_.data() + slot->offset_, slot->byte_size_};
    if (slot->active_target_index_ != SACCADE_OVERLAY_ACTIVE_TARGET_NONE) {
        output->flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
        output->active_target_index = slot->active_target_index_;
    }
    return SACCADE_OK;
}

void DesktopPipeline::observe_overlay(void* context, uint64_t, SaccadeResult result, const backend::d3d12::OverlaySubmission*) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline != nullptr && result != SACCADE_OK && result != SACCADE_ERROR_BUSY && result != SACCADE_ERROR_NOT_FOUND) {
        (void)pipeline->fail(result);
    }
}

SaccadeResult DesktopPipeline::read_diagnostics(DesktopPipelineDiagnostics* output) const noexcept {
    if (!initialized_ || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    output->pipeline = stats_;
    output->runtime = runtime_.diagnostics();
    SaccadeResult result = captures_.read_stats(&output->capture);
    if (result == SACCADE_OK)
        result = overlays_.read_stats(&output->overlay_set);
    output->inference_memory.struct_size = sizeof(output->inference_memory);
    output->inference_memory.api_version = SACCADE_API_VERSION;
    if (result == SACCADE_OK)
        result = saccade_inference_memory_stats(inference_.runtime(), inference_.session(), &output->inference_memory);
    if (result == SACCADE_OK)
        result = captures_.read_memory_stats(&output->capture_memory);
    if (result != SACCADE_OK)
        return result;
    output->model = inference_.info();
    output->debugger_frames_transforms = debugger_.frames_transforms();
    output->debugger_scene_fusion = debugger_.scene_fusion();
    const geometry::DisplaySnapshot& displays = displays_.snapshot();
    for (uint32_t index = 0; index < displays.count; ++index) {
        DesktopDisplayDiagnostics& display = output->displays[index];
        display.display = displays.displays[index];
        result = overlays_.read_surface_stats(display.display.display_id, &display.overlay);
        if (result == SACCADE_OK)
            result = overlays_.read_surface_memory_stats(display.display.display_id, &display.memory);
        if (result == SACCADE_OK)
            result = overlays_.read_surface_renderer_stats(display.display.display_id, &display.gpu);
        if (result != SACCADE_OK)
            return result;
        output->overlay.ticks += display.overlay.presentation_attempts;
        output->overlay.rendered += display.overlay.rendered_frames;
        output->overlay.presented += display.overlay.presented_frames;
        output->overlay.no_frame += display.overlay.no_frame_ticks;
        output->overlay.busy += display.overlay.busy_frames;
        output->overlay.failures += display.overlay.failures;
        output->overlay.known_memory_bytes += display.memory.total_known_and_estimated;
    }
    output->topology_epoch = displays.epoch;
    output->display_count = displays.count;
    output->permissions = diagnostic_capture_permission | diagnostic_accessibility_permission;
    const SurfaceQualification surface = qualify_surface(action_point_qualifier_);
    output->surface = surface.disposition;
    output->surface_reason_bits = surface.reason_bits;
    if (input_available_ && surface.disposition == ActionPointDisposition::qualified)
        output->permissions |= diagnostic_input_permission;
    output->source = source_;
    output->scope = scope_;
    output->compute = settings_.compute.policy;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::debug_capture_scene() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;

    const geometry::DisplaySnapshot& displays = displays_.snapshot();
    std::array<application::DebuggerTransformRecord, geometry::display_capacity> transforms{};
    for (uint32_t index = 0; index < displays.count; ++index) {
        geometry::CoordinateTransform transform;
        const SaccadeResult result = geometry::make_desktop_to_surface_transform(displays.displays[index], displays.epoch, &transform);
        if (result != SACCADE_OK)
            return result;
        transforms[index] = {displays.displays[index].display_id, displays.displays[index].display_id, transform.descriptor()};
    }
    const application::DebuggerCaptureContext context{0, transforms.data(), nullptr, displays.count, 0};
    return runtime_.capture_debugger_scene(&debugger_, context);
}

SaccadeResult DesktopPipeline::debug_dry_run(uint64_t now_ns, application::DebuggerPlanView* output) noexcept {
    return initialized_ ? debugger_.dry_run_first_click(now_ns, output) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_replay(application::DebuggerPlanView* output) noexcept {
    return initialized_ ? debugger_.replay(output) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_arm_fault(application::DebugFaultPoint point, uint32_t count, SaccadeResult result) noexcept {
    return initialized_ ? debugger_.arm_fault(point, count, result) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_clear() noexcept {
    return initialized_ ? debugger_.clear() : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::shutdown() noexcept {
    SaccadeResult result = SACCADE_OK;
    if (overlay_running_)
        preserve_first_error(stop_overlay(), &result);

    if (keys_initialized_) {
        const SaccadeResult stopped = keys_.shutdown();
        if (stopped == SACCADE_OK)
            keys_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (agent_initialized_) {
        const SaccadeResult stopped = agent_.shutdown();
        if (stopped == SACCADE_OK)
            agent_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (input_initialized_) {
        const SaccadeResult stopped = input_.shutdown();
        if (stopped == SACCADE_OK)
            input_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (runtime_initialized_) {
        const SaccadeResult stopped = runtime_.shutdown();
        if (stopped == SACCADE_OK)
            runtime_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (bridge_initialized_) {
        const SaccadeResult stopped = bridge_.shutdown();
        if (stopped == SACCADE_OK)
            bridge_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (accessibility_initialized_) {
        const SaccadeResult stopped = accessibility_.shutdown();
        if (stopped == SACCADE_OK)
            accessibility_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    preserve_first_error(action_point_qualifier_.shutdown(), &result);

    if (captures_initialized_) {
        const SaccadeResult stopped = captures_.shutdown();
        if (stopped == SACCADE_OK)
            captures_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    preserve_first_error(inference_.shutdown(), &result);

    if (overlays_initialized_) {
        const SaccadeResult stopped = overlays_.shutdown();
        if (stopped == SACCADE_OK)
            overlays_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (!overlays_initialized_ && provider_initialized_) {
        const SaccadeResult stopped = provider_.shutdown();
        if (stopped == SACCADE_OK)
            provider_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    preserve_first_error(artifact_.shutdown(), &result);
    if (!overlays_initialized_ && overlay_queue_ != nullptr) {
        static_cast<ID3D12CommandQueue*>(overlay_queue_)->Release();
        overlay_queue_ = nullptr;
    }
    if (result != SACCADE_OK)
        return fail(result);
    pending_command_ = false;
    capture_running_ = false;
    overlay_running_ = false;
    input_available_ = true;
    topology_pending_ = false;
    permission_epoch_ = 1;
    semantic_window_id_ = 0;
    initialized_ = false;
    config_ = {};
    settings_ = {};
    resolved_hints_ = {};
    next_keyboard_layout_check_ns_ = 0;
    keyboard_layout_token_ = 0;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

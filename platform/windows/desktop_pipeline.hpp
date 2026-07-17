#ifndef SACCADE_PLATFORM_WINDOWS_DESKTOP_PIPELINE_HPP
#define SACCADE_PLATFORM_WINDOWS_DESKTOP_PIPELINE_HPP

#include "agent/service.hpp"
#include "application/desktop_runtime.hpp"
#include "application/inference_runtime.hpp"
#include "application/session_key_router.hpp"
#include "application/settings.hpp"
#include "application/window_navigator.hpp"
#include "backends/d3d12/directml_provider.hpp"
#include "geometry/display_catalog.hpp"
#include "model/mapped_artifact.hpp"
#include "overlay/glyph_atlas.hpp"
#include "platform/windows/accessibility_provider.hpp"
#include "platform/windows/action_point_qualifier.hpp"
#include "platform/windows/display_topology.hpp"
#include "platform/windows/input_executor.hpp"
#include "platform/windows/neural_bridge.hpp"
#include "platform/windows/overlay_surface.hpp"
#include "platform/windows/scene_capture.hpp"
#include "platform/windows/screen_capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::windows {

enum class DesktopPipelineStage : uint32_t {
    none = 0,
    artifact,
    provider,
    inference,
    capture_provider,
    topology,
    capture_set,
    accessibility,
    input,
    bridge,
    runtime,
    keys,
    overlay
};

struct DesktopPipelineConfig {
    const char* artifact_path = nullptr;
    const char* shader_directory = nullptr;
    const application::SettingsDocument* settings = nullptr;
    model::ArtifactVerifier verifier{};
    void* shell_context = nullptr;
    application::ForwardCommandFn forward_shell = nullptr;
    uint64_t start_time_ns = 0;
};

struct DesktopPipelineAdvance {
    application::DesktopRuntimeAdvance runtime{};
    uint32_t frames_offered = 0;
    bool command_started = false;
    bool became_idle = false;
    uint8_t reserved[2]{};
};

struct DesktopPipelineStats {
    uint64_t activations = 0;
    uint64_t advances = 0;
    uint64_t capture_starts = 0;
    uint64_t capture_stops = 0;
    uint64_t capture_attempts = 0;
    uint64_t frames_offered = 0;
    uint64_t topology_refreshes = 0;
    uint64_t topology_changes = 0;
    uint64_t input_plans = 0;
    uint64_t physical_inputs = 0;
    uint64_t overlay_starts = 0;
    uint64_t overlay_stops = 0;
    uint64_t overlay_presents = 0;
    uint64_t semantic_requests = 0;
    uint64_t permission_changes = 0;
    uint64_t failures = 0;
};

enum : uint32_t {
    diagnostic_capture_permission = UINT32_C(1) << 0,
    diagnostic_accessibility_permission = UINT32_C(1) << 1,
    diagnostic_input_permission = UINT32_C(1) << 2
};

enum : uint32_t {
    surface_reason_none = 0,
    surface_reason_secure_desktop = UINT32_C(1) << 0,
    surface_reason_foreground_missing = UINT32_C(1) << 1,
    surface_reason_focus_unavailable = UINT32_C(1) << 2,
    surface_reason_secure_focus = UINT32_C(1) << 3
};

struct DesktopOverlayDiagnostics {
    uint64_t ticks = 0;
    uint64_t rendered = 0;
    uint64_t presented = 0;
    uint64_t no_frame = 0;
    uint64_t busy = 0;
    uint64_t deadline_misses = 0;
    uint64_t failures = 0;
    uint64_t known_memory_bytes = 0;
};

struct DesktopDisplayDiagnostics {
    geometry::DisplaySurface display{};
    OverlaySurfaceStats overlay{};
    OverlaySurfaceMemoryStats memory{};
    backend::d3d12::OverlayStats gpu{};
};

struct DesktopPipelineDiagnostics {
    DesktopPipelineStats pipeline{};
    application::DesktopRuntimeDiagnostics runtime{};
    SceneCaptureStats capture{};
    OverlaySurfaceSetStats overlay_set{};
    DesktopOverlayDiagnostics overlay{};
    SaccadeMemoryStats inference_memory{};
    SaccadeMemoryStats capture_memory{};
    SaccadeInferenceSessionInfo model{};
    application::DebuggerFramesTransformsView debugger_frames_transforms{};
    application::DebuggerSceneFusionView debugger_scene_fusion{};
    std::array<DesktopDisplayDiagnostics, geometry::display_capacity> displays{};
    uint64_t topology_epoch = 0;
    uint32_t display_count = 0;
    uint32_t permissions = 0;
    uint32_t surface_reason_bits = surface_reason_none;
    application::TargetSource source = application::TargetSource::pixel;
    application::TargetScope scope = application::TargetScope::desktop;
    application::ComputePolicy compute = application::ComputePolicy::automatic;
    ActionPointDisposition surface = ActionPointDisposition::unavailable;
    uint8_t reserved[3]{};
};

class DesktopPipeline final {
  public:
    DesktopPipeline() noexcept = default;
    ~DesktopPipeline();

    DesktopPipeline(const DesktopPipeline&) = delete;
    DesktopPipeline& operator=(const DesktopPipeline&) = delete;
    DesktopPipeline(DesktopPipeline&&) = delete;
    DesktopPipeline& operator=(DesktopPipeline&&) = delete;

    SaccadeResult initialize(const DesktopPipelineConfig&) noexcept;
    SaccadeResult apply_settings(const application::SettingsDocument&, uint64_t now_ns) noexcept;
    SaccadeResult set_text(SaccadeSpanU8) noexcept;
    SaccadeResult request(application::Command, uint64_t now_ns) noexcept;
    SaccadeResult advance(uint64_t now_ns, DesktopPipelineAdvance*) noexcept;
    bool route_key(const application::KeyEvent&) noexcept;
    SaccadeResult neutralize_synthetic_input() noexcept;
    SaccadeResult observe_physical_input(uint64_t now_ns) noexcept;
    SaccadeResult set_input_available(bool available, uint64_t now_ns) noexcept;
    SaccadeResult process_agent(SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                                SaccadeMutableSpanU8 output, size_t* output_size) noexcept;
    SaccadeResult refresh_topology() noexcept;
    SaccadeResult read_diagnostics(DesktopPipelineDiagnostics*) const noexcept;
    SaccadeResult debug_capture_scene() noexcept;
    SaccadeResult debug_dry_run(uint64_t now_ns, application::DebuggerPlanView*) noexcept;
    SaccadeResult debug_replay(application::DebuggerPlanView*) noexcept;
    SaccadeResult debug_arm_fault(application::DebugFaultPoint, uint32_t count, SaccadeResult result) noexcept;
    SaccadeResult debug_clear() noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept {
        return pending_command_ || runtime_.active() || input_.synthetic_input_active();
    }

    [[nodiscard]] DesktopPipelineStats stats() const noexcept { return stats_; }

    [[nodiscard]] SaccadeResult last_result() const noexcept { return last_result_; }

    [[nodiscard]] DesktopPipelineStage last_stage() const noexcept { return last_stage_; }

  private:
    static constexpr size_t overlay_packet_overhead = sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayStyle);
    static constexpr size_t overlay_arena_capacity =
        geometry::display_capacity * overlay_packet_overhead +
        static_cast<size_t>(SACCADE_OVERLAY_MAX_TARGETS) * sizeof(SaccadeOverlayTarget);

    struct OverlayFrameSlot {
        size_t offset_ = 0;
        size_t byte_size_ = 0;
        uint64_t scene_epoch_ = 0;
        uint64_t transform_epoch_ = 0;
        uint64_t display_id_ = 0;
        uint32_t active_target_index_ = SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
        uint32_t reveal_ticks_ = 0;
        bool present_pending_ = false;
    };

    static SaccadeResult execute_plan(void*, SaccadeSpanU8, uint32_t, uint64_t) noexcept;
    static SaccadeResult preflight_input(void*, const input::PlanView&, uint32_t command_index,
                                         uint64_t now_ns) noexcept;
    static SaccadeResult read_environment(void*, application::InteractionState*) noexcept;
    static SaccadeResult acquire_agent_scene(void*, scene::PacketView*) noexcept;
    static SaccadeResult read_agent_physical_state(void*, SaccadeAgentPhysicalState*) noexcept;
    static SaccadeResult abort_agent_input(void*) noexcept;
    static SaccadeResult cycle_agent_window(void*, bool backward) noexcept;
    static SaccadeResult forward_command(void*, application::Command, uint64_t) noexcept;
    static SaccadeResult route_session_command(void*, application::Command, uint64_t) noexcept;
    static bool semantic_query_current(void*, const SaccadeAccessibilityQueryDesc&) noexcept;
    static bool input_lease_active(void*) noexcept;
    static SaccadeResult neutralize_input(void*) noexcept;
    static SaccadeResult activate_window(void*, uint64_t) noexcept;
    static SaccadeResult load_overlay(void*, uint64_t, SaccadeOverlayFrameDesc*) noexcept;
    static void observe_overlay(void*, uint64_t, SaccadeResult, const backend::d3d12::OverlaySubmission*) noexcept;

    SaccadeResult offer_frames(uint64_t, uint32_t*) noexcept;
    SaccadeResult request_semantic(uint64_t frame_id, uint64_t transform_epoch, uint64_t topology_epoch) noexcept;
    SaccadeResult invalidate_semantic() noexcept;
    SaccadeResult publish_grid(application::SceneCoordinatorAdvance*) noexcept;
    SaccadeResult change_source(application::TargetSource, uint64_t) noexcept;
    SaccadeResult change_scope(application::TargetScope, uint64_t) noexcept;
    SaccadeResult begin_grid_action(application::Command, uint64_t) noexcept;
    SaccadeResult begin_window_action(uint64_t) noexcept;
    SaccadeResult navigate_window(application::Command, uint64_t now_ns, bool agent_preflight) noexcept;
    SaccadeResult restart_action(uint64_t now_ns) noexcept;
    SaccadeResult resolve_scope(geometry::RectQ8*, uint64_t*) noexcept;
    SaccadeResult apply_scope() noexcept;
    SaccadeResult refresh_hint_language(uint64_t now_ns) noexcept;
    SaccadeResult start_capture() noexcept;
    SaccadeResult stop_capture(bool*) noexcept;
    SaccadeResult update_input_desktop() noexcept;
    SaccadeResult synchronize_topology() noexcept;
    SaccadeResult publish_overlays() noexcept;
    SaccadeResult start_overlay() noexcept;
    SaccadeResult stop_overlay() noexcept;
    SaccadeResult fail(SaccadeResult, DesktopPipelineStage = DesktopPipelineStage::none) noexcept;

    DesktopPipelineConfig config_{};
    application::SettingsDocument settings_{};
    application::HintSettings resolved_hints_{};
    model::MappedArtifact artifact_{};
    backend::d3d12::DirectMlInferenceProvider provider_{};
    application::InferenceRuntime inference_{};
    ScreenCaptureProvider capture_provider_{};
    SceneCaptureSet captures_{};
    NeuralBridge bridge_{};
    AccessibilityProvider accessibility_{};
    ActionPointQualifier action_point_qualifier_{};
    InputExecutor input_{};
    geometry::DisplayCatalog displays_{};
    DisplayCollector display_collector_{};
    application::DesktopRuntime runtime_{};
    agent::Service agent_{};
    application::SessionKeyRouter keys_{};
    application::WindowNavigator window_navigator_{};
    application::WindowSnapshot windows_{};
    OverlaySurfaceSet overlays_{};
    application::DesktopRuntimeStorage runtime_storage_{};
    application::DebuggerStorage debugger_storage_{};
    application::Debugger debugger_{};
    application::OverlayComposeWorkspace overlay_workspace_{};
    alignas(128) std::array<uint8_t, overlay_arena_capacity> overlay_arena_{};
    std::array<OverlayFrameSlot, geometry::display_capacity> overlay_frames_{};
    std::array<overlay::GlyphAtlasStorage, 2> glyph_atlases_{};
    uint32_t current_glyph_atlas_ = 0;
    SaccadeOverlayStyle overlay_style_{};
    geometry::CoordinateTransform overlay_transform_{};
    void* overlay_queue_ = nullptr;
    DesktopPipelineStats stats_{};
    uint64_t permission_epoch_ = 1;
    uint64_t next_capture_ns_ = 0;
    uint64_t pending_deadline_ns_ = 0;
    uint64_t next_keyboard_layout_check_ns_ = 0;
    uint64_t keyboard_layout_token_ = 0;
    uint64_t next_grid_frame_id_ = 1;
    uint64_t next_window_frame_id_ = 1;
    uint64_t semantic_window_id_ = 0;
    application::Command pending_ = application::Command::pointer_move;
    application::TargetSource source_ = application::TargetSource::pixel;
    application::TargetScope scope_ = application::TargetScope::desktop;
    geometry::RectQ8 scope_rect_{};
    bool scope_filter_enabled_ = false;
    bool window_scene_active_ = false;
    SaccadeResult last_result_ = SACCADE_OK;
    DesktopPipelineStage last_stage_ = DesktopPipelineStage::none;
    bool pending_command_ = false;
    bool capture_running_ = false;
    bool initialized_ = false;
    bool provider_initialized_ = false;
    bool captures_initialized_ = false;
    bool bridge_initialized_ = false;
    bool accessibility_initialized_ = false;
    bool input_initialized_ = false;
    bool runtime_initialized_ = false;
    bool agent_initialized_ = false;
    bool keys_initialized_ = false;
    bool overlays_initialized_ = false;
    bool overlay_running_ = false;
    bool overlay_dirty_ = true;
    bool input_available_ = true;
    bool topology_pending_ = false;
};

static_assert(sizeof(DesktopPipelineAdvance) == 128);
static_assert(sizeof(DesktopPipelineStats) == 128);

} // namespace saccade::platform::windows

#endif

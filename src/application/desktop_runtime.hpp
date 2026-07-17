#ifndef SACCADE_APPLICATION_DESKTOP_RUNTIME_HPP
#define SACCADE_APPLICATION_DESKTOP_RUNTIME_HPP

#include "application/debug_trace.hpp"
#include "application/debugger.hpp"
#include "application/interaction_controller.hpp"
#include "application/overlay_composer.hpp"
#include "application/scene_coordinator.hpp"
#include "scheduler/desktop_neural_coordinator.hpp"

#include <cstdint>

namespace saccade::application {

struct DesktopRuntimeStorage {
    scene::SceneStoreStorage neural_scenes{};
    scene::SceneStoreStorage output_scenes{};
    scheduler::DesktopNeuralCoordinatorStorage neural{};
    SceneCoordinatorStorage scene{};
    SessionStorage session{};
};

struct DesktopRuntimeConfig {
    scheduler::DesktopNeuralCoordinatorConfig neural{};
    SaccadeAccessibilityProviderDesc accessibility{};
    scene::FusionConfig fusion{};
    TargetFilterConfig scene_filter{};
    SemanticFreshness semantic_freshness{};
    SceneSource source = SceneSource::pixel;
    PlanExecutor executor{};
    InteractionProfile interaction{};
    InteractionStateSource environment{};
    InteractionControllerSink sink{};
};

struct DesktopRuntimeAdvance {
    scheduler::DesktopNeuralAdvance neural{};
    SceneCoordinatorAdvance scene{};
    bool interaction_ticked = false;
    uint8_t reserved[7]{};
};

struct DesktopRuntimeStats {
    uint64_t frames_offered = 0;
    uint64_t semantic_requests = 0;
    uint64_t advances = 0;
    uint64_t commands = 0;
    uint64_t symbols = 0;
    uint64_t overlay_compositions = 0;
    uint64_t failures = 0;
};

struct DesktopRuntimeDiagnostics {
    DesktopRuntimeStats runtime{};
    scheduler::DesktopNeuralCoordinatorStats neural{};
    scheduler::DualRateStats scheduler{};
    SceneCoordinatorStats scene{};
    SceneCoordinatorStatus scene_status{};
    SessionStats session{};
    InteractionControllerStats interaction{};
    OverlayComposeStats overlay{};
    DebugTraceSnapshot trace{};
};

class DesktopRuntime final {
  public:
    DesktopRuntime() noexcept = default;
    ~DesktopRuntime();

    DesktopRuntime(const DesktopRuntime&) = delete;
    DesktopRuntime& operator=(const DesktopRuntime&) = delete;
    DesktopRuntime(DesktopRuntime&&) = delete;
    DesktopRuntime& operator=(DesktopRuntime&&) = delete;

    SaccadeResult initialize(const DesktopRuntimeConfig&, DesktopRuntimeStorage*) noexcept;
    SaccadeResult offer(scheduler::DesktopNeuralFrame) noexcept;
    SaccadeResult request_semantic(const SaccadeAccessibilityQueryDesc&) noexcept;
    SaccadeResult cancel_semantic() noexcept;
    SaccadeResult set_interaction_profile(InteractionProfile) noexcept;
    SaccadeResult set_text(SaccadeSpanU8) noexcept;
    SaccadeResult set_target_filter(TargetFilterConfig) noexcept;
    SaccadeResult set_fusion(scene::FusionConfig) noexcept;
    SaccadeResult set_source(SceneSource) noexcept;
    SaccadeResult set_scope(const geometry::RectQ8*) noexcept;
    SaccadeResult publish_grid(scene::GridSceneConfig, SceneCoordinatorAdvance*) noexcept;
    SaccadeResult publish_windows(scene::WindowSceneConfig, const SaccadeWindowInfo*, uint32_t,
                                  SceneCoordinatorAdvance*) noexcept;
    SaccadeResult advance(uint64_t now_ns, DesktopRuntimeAdvance*) noexcept;
    SaccadeResult dispatch(Command, uint64_t now_ns, InteractionCommandResult*) noexcept;
    SaccadeResult enter_symbol(uint16_t, uint64_t now_ns, SessionEvent*) noexcept;
    SaccadeResult observe_physical_input(uint64_t now_ns) noexcept;
    SaccadeResult acquire_scene(scene::PacketView*) noexcept;
    SaccadeResult compose_overlay(const OverlayComposeConfig&, OverlayComposeWorkspace*, SaccadeMutableSpanU8,
                                  OverlayComposeResult*) noexcept;
    SaccadeResult capture_debugger_scene(Debugger*) noexcept;
    SaccadeResult capture_debugger_scene(Debugger*, const DebuggerCaptureContext&) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept { return session_.active(); }

    [[nodiscard]] bool semantic_running() const noexcept { return scene_.semantic_running(); }

    [[nodiscard]] uint16_t hint_symbol_for_physical_key(uint32_t physical_key) const noexcept;

    [[nodiscard]] SceneSource source() const noexcept { return scene_.source(); }

    [[nodiscard]] const scene::SceneStore& scenes() const noexcept { return output_scenes_; }

    [[nodiscard]] DesktopRuntimeStats stats() const noexcept { return stats_; }

    [[nodiscard]] DesktopRuntimeDiagnostics diagnostics() const noexcept;

  private:
    static SaccadeResult read_state(void*, InteractionState*) noexcept;
    static SaccadeResult forward(void*, Command, uint64_t) noexcept;
    static bool input_lease_active(void*) noexcept;
    static SaccadeResult neutralize_input(void*) noexcept;

    SaccadeResult read_environment(InteractionState*) noexcept;
    SaccadeResult forward_command(Command, uint64_t) noexcept;
    SaccadeResult fail(SaccadeResult) noexcept;

    DesktopRuntimeConfig config_{};
    scene::SceneStore neural_scenes_{};
    scene::SceneStore output_scenes_{};
    scheduler::DesktopNeuralCoordinator neural_{};
    SceneCoordinator scene_{};
    SessionEngine session_{};
    InteractionController interaction_{};
    OverlayComposer overlay_{};
    DebugTrace trace_{};
    DesktopRuntimeStats stats_{};
    bool initialized_ = false;
    bool neural_initialized_ = false;
    bool scene_initialized_ = false;
    bool session_initialized_ = false;
    bool interaction_initialized_ = false;
};

static_assert(sizeof(DesktopRuntimeAdvance) == 120);
static_assert(sizeof(DesktopRuntimeStats) == 56);
static_assert(sizeof(DesktopRuntimeDiagnostics) == 1704);

} // namespace saccade::application

#endif

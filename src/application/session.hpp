#ifndef SACCADE_APPLICATION_SESSION_HPP
#define SACCADE_APPLICATION_SESSION_HPP

#include "interaction/action_planner.hpp"
#include "interaction/hints.hpp"
#include "interaction/selection_reducer.hpp"
#include "scene/store.hpp"

#include <array>
#include <cstdint>

namespace saccade::application {

using ExecutePlanFn = SaccadeResult (*)(void*, SaccadeSpanU8, uint32_t available_permissions, uint64_t now_ns) noexcept;

struct PlanExecutor {
    void* context = nullptr;
    ExecutePlanFn execute = nullptr;
};

struct SessionAction {
    interaction::ActionKind kind = interaction::ActionKind::click;
    uint32_t button = SACCADE_INPUT_BUTTON_LEFT;
    uint32_t repeat_count = 1;
    uint32_t key_usage = 0;
    uint32_t modifiers = 0;
    int32_t delta_x_q8 = 0;
    int32_t delta_y_q8 = 0;
    uint64_t duration_ns = 0;
    uint64_t pointer_duration_ns = 0;
    geometry::PointQ8 final_pointer{};
    SaccadeSpanU8 text{};
    bool move_to_final_pointer = false;
    bool defer_execution = false;
    uint8_t reserved[6]{};
};

struct SessionConfig {
    interaction::SelectionMode mode = interaction::SelectionMode::single;
    interaction::HintConfig hints{};
    interaction::ActionContext action{};
    SessionAction request{};
};

struct SessionEpochs {
    uint64_t scene_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
    uint64_t focus_id = 0;
};

struct SessionEvent {
    uint64_t target_id = 0;
    uint32_t candidate_count = 0;
    uint32_t selected_count = 0;
    bool exact = false;
    bool action_executed = false;
    uint8_t reserved[6]{};
};

struct SessionStats {
    uint64_t sessions_started = 0;
    uint64_t sessions_completed = 0;
    uint64_t sessions_cancelled = 0;
    uint64_t symbols_entered = 0;
    uint64_t prefixes_rejected = 0;
    uint64_t plans_executed = 0;
    uint64_t execution_failures = 0;
    uint64_t stale_cancellations = 0;
    uint64_t scene_refreshes = 0;
    uint64_t refresh_failures = 0;
};

enum class TargetSnapDirection : uint32_t { left = 0, right = 1, up = 2, down = 3 };

struct SessionStorage {
    alignas(64) std::array<uint8_t, scene::target_packet_max_bytes> scene{};
    interaction::HintSessionStorage hints{};
    interaction::SelectionStorage selection{};
    interaction::ActionPlanStorage plan{};
    std::array<geometry::PointQ8, interaction::maximum_action_targets> action_points{};
    std::array<uint16_t, interaction::maximum_hint_symbols> prefix{};
    std::array<uint8_t, interaction::maximum_action_payload_bytes> text{};
};

class SessionEngine final {
  public:
    SaccadeResult initialize(scene::SceneStore*, SessionStorage*, PlanExecutor) noexcept;
    SaccadeResult begin(const SessionConfig&) noexcept;
    SaccadeResult begin_latest(SessionConfig) noexcept;
    SaccadeResult enter_symbol(uint16_t, uint64_t now_ns, SessionEvent*) noexcept;
    SaccadeResult backspace(SessionEvent*) noexcept;
    SaccadeResult confirm(uint64_t now_ns, SessionEvent*) noexcept;
    SaccadeResult cycle_target_position() noexcept;
    SaccadeResult set_target_position(uint32_t grid_index) noexcept;
    SaccadeResult snap_target(TargetSnapDirection) noexcept;
    SaccadeResult nudge_target(int32_t delta_x_q8, int32_t delta_y_q8) noexcept;
    SaccadeResult tick(const SessionEpochs&, uint64_t now_ns) noexcept;
    SaccadeResult cancel(interaction::SelectionCancelReason) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] uint32_t prefix_count() const noexcept { return prefix_count_; }

    [[nodiscard]] interaction::SelectionView selection() const noexcept { return selection_.view(); }

    [[nodiscard]] const interaction::HintLabel* labels() const noexcept { return hints_.labels(); }

    [[nodiscard]] uint32_t label_count() const noexcept { return hints_.label_count(); }

    [[nodiscard]] const scene::PacketView& scene_view() const noexcept { return scene_; }

    [[nodiscard]] SessionStats stats() const noexcept { return stats_; }

  private:
    SaccadeResult begin_scene(const SessionConfig&, const scene::PacketView&) noexcept;
    SaccadeResult execute_selection(uint64_t, SessionEvent*) noexcept;
    SaccadeResult resolve_prefix(SessionEvent*) noexcept;
    SaccadeResult copy_scene(const scene::PacketView&, scene::PacketView*) noexcept;
    SaccadeResult refresh_latest(const SessionEpochs&) noexcept;
    [[nodiscard]] geometry::PointQ8 adjusted_point(const SaccadeTargetRecord&) const noexcept;
    void reset_session() noexcept;

    scene::SceneStore* scenes_ = nullptr;
    SessionStorage* storage_ = nullptr;
    PlanExecutor executor_{};
    scene::PacketView scene_{};
    interaction::HintSession hints_{};
    interaction::SelectionReducer selection_{};
    interaction::ActionPlanner planner_{};
    interaction::ActionContext action_context_{};
    SessionAction action_{};
    SessionStats stats_{};
    uint32_t prefix_count_ = 0;
    uint32_t target_position_ = UINT32_MAX;
    int32_t nudge_x_q8_ = 0;
    int32_t nudge_y_q8_ = 0;
    bool initialized_ = false;
    bool active_ = false;
    bool deferred_execution_ = false;
};

static_assert(sizeof(SessionEvent) == 24);
static_assert(sizeof(SessionStats) == 80);

} // namespace saccade::application

#endif

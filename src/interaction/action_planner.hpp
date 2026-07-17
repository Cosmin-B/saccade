#ifndef SACCADE_INTERACTION_ACTION_PLANNER_HPP
#define SACCADE_INTERACTION_ACTION_PLANNER_HPP

#include "geometry/coordinate_transform.hpp"
#include "input/plan.hpp"
#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::interaction {

constexpr uint32_t maximum_action_targets = SACCADE_INPUT_PLAN_MAX_TARGETS;
constexpr size_t maximum_action_payload_bytes = input::maximum_plan_payload_bytes;
constexpr size_t maximum_action_plan_bytes = input::maximum_plan_bytes;

enum class ActionKind : uint32_t {
    pointer_move = 1,
    click = 2,
    hold = 3,
    drag = 4,
    scroll = 5,
    key = 6,
    text = 7,
    window_activate = 8,
    release = 9,
    text_select = 10,
    invoke = 11
};

struct ActionContext {
    uint64_t plan_id = 0;
    uint64_t scene_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
    uint64_t focus_id = 0;
    uint64_t window_id = 0;
    uint64_t display_id = 0;
    uint64_t now_ns = 0;
    uint64_t deadline_ns = 0;
    uint32_t permissions = 0;
    uint32_t expected_buttons = 0;
    uint32_t plan_flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    uint32_t reserved = 0;
};

struct ActionRequest {
    ActionKind kind = ActionKind::pointer_move;
    const uint64_t* target_ids = nullptr;
    const geometry::PointQ8* target_points = nullptr;
    uint32_t target_count = 0;
    uint32_t target_point_count = 0;
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
    bool allow_point_outside_target = false;
    uint8_t reserved[6]{};
};

using ActionPlanStorage = input::PlanStorage;

struct ActionPlannerStats {
    uint64_t plans_built = 0;
    uint64_t commands_built = 0;
    uint64_t targets_resolved = 0;
    uint64_t rejected_stale = 0;
    uint64_t rejected_unsafe = 0;
    uint64_t rejected_permission = 0;
};

class ActionPlanner final {
  public:
    SaccadeResult build(const scene::PacketView&, const ActionContext&, const ActionRequest&, ActionPlanStorage*,
                        SaccadeSpanU8*) noexcept;

    [[nodiscard]] ActionPlannerStats stats() const noexcept { return stats_; }

  private:
    ActionPlannerStats stats_{};
};

} // namespace saccade::interaction

#endif

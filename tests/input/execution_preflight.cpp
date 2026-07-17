#include "input/execution_preflight.hpp"

#include <array>
#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    valid_failed,
    deadline_failed,
    stale_failed,
    secure_failed,
    window_failed,
    target_failed
};

constexpr uint64_t now_ns = 100;
constexpr uint64_t deadline_ns = 200;
constexpr uint64_t target_id = 11;
constexpr uint64_t scene_epoch = 12;
constexpr uint64_t frame_id = 13;
constexpr uint64_t model_epoch = 14;
constexpr uint64_t session_epoch = 15;
constexpr uint64_t transform_epoch = 16;
constexpr uint64_t topology_epoch = 17;
constexpr uint64_t permission_epoch = 18;
constexpr uint64_t source_id = 19;
constexpr uint64_t focus_id = 20;
constexpr uint64_t window_id = 21;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct Fixture {
    SaccadeTargetPacketHeader scene_header{};
    std::array<SaccadeTargetRecord, 1> targets{};
    SaccadeInputPlanHeader plan_header{};
    std::array<SaccadeInputCommand, 1> commands{};
    saccade::input::PlanView plan{};
    saccade::input::ExecutionPreflightState state{};

    Fixture() noexcept {
        scene_header.target_count = static_cast<uint32_t>(targets.size());
        scene_header.scene_epoch = scene_epoch;
        scene_header.frame_id = frame_id;
        scene_header.model_epoch = model_epoch;
        scene_header.session_epoch = session_epoch;
        scene_header.transform_epoch = transform_epoch;
        scene_header.topology_epoch = topology_epoch;
        scene_header.source_id = source_id;
        targets[0].target_id = target_id;
        targets[0].window_id = window_id;
        targets[0].flags = SACCADE_TARGET_ACTIONABLE;

        plan_header.command_count = static_cast<uint32_t>(commands.size());
        plan_header.scene_epoch = scene_epoch;
        plan_header.frame_id = frame_id;
        plan_header.model_epoch = model_epoch;
        plan_header.session_epoch = session_epoch;
        plan_header.transform_epoch = transform_epoch;
        plan_header.topology_epoch = topology_epoch;
        plan_header.permission_epoch = permission_epoch;
        plan_header.source_id = source_id;
        plan_header.focus_id = focus_id;
        plan_header.window_id = window_id;
        plan_header.deadline_ns = deadline_ns;
        commands[0].target_id = target_id;
        plan = {&plan_header, commands.data(), sizeof(plan_header) + sizeof(commands)};

        state.scene.header = &scene_header;
        state.scene.targets = targets.data();
        state.scene.byte_size = sizeof(scene_header) + sizeof(targets);
        state.focus_id = focus_id;
        state.window_id = window_id;
        state.topology_epoch = topology_epoch;
        state.permission_epoch = permission_epoch;
        state.input_available = true;
        state.target_window_available = true;
        state.validate_active_window = true;
        state.validate_initial_buttons = true;
    }
};

} // namespace

int main() {
    Fixture fixture{};
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_OK)
        return result(TestResult::valid_failed);

    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, deadline_ns) != SACCADE_ERROR_TIMEOUT)
        return result(TestResult::deadline_failed);

    fixture.state.focus_id = focus_id + 1U;
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_ERROR_STALE_HANDLE)
        return result(TestResult::stale_failed);
    fixture.state.focus_id = focus_id;

    fixture.state.window_id = window_id + 1U;
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_ERROR_STALE_HANDLE)
        return result(TestResult::stale_failed);
    fixture.state.window_id = window_id;

    fixture.state.target_point_secure = true;
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_ERROR_PERMISSION)
        return result(TestResult::secure_failed);
    fixture.state.target_point_secure = false;

    fixture.state.target_window_available = false;
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_ERROR_NOT_FOUND)
        return result(TestResult::window_failed);
    fixture.state.target_window_available = true;

    fixture.targets[0].flags |= SACCADE_TARGET_SECURE;
    if (saccade::input::validate_execution_preflight(fixture.plan, fixture.state, now_ns) != SACCADE_ERROR_NOT_FOUND)
        return result(TestResult::target_failed);

    return result(TestResult::success);
}

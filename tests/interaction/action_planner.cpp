#include "input/physical_reducer.hpp"
#include "interaction/action_planner.hpp"
#include "scene/packet.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_scene = 1,
    drag_build = 2,
    drag_physical = 3,
    drag_abort = 4,
    hold_build = 5,
    release_build = 6,
    text_build = 7,
    click_build = 8,
    dry_run = 9,
    scroll_build = 10,
    key_build = 11,
    physical_override = 12,
    expire = 13,
    backend_failure = 14,
    stale_handle = 15,
    unsupported = 16,
    shutdown = 17,
    text_select = 18,
    outside_target = 19,
    maximum_click = 20,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr size_t packet_size =
    sizeof(SaccadeTargetPacketHeader) + SACCADE_INPUT_PLAN_MAX_TARGETS * sizeof(SaccadeTargetRecord);

struct alignas(8) SceneStorage {
    std::array<uint8_t, packet_size> bytes{};
};

void make_scene(SceneStorage* storage) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = SACCADE_INPUT_PLAN_MAX_TARGETS;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = 11;
    header.frame_id = 12;
    header.model_epoch = 13;
    header.session_epoch = 14;
    header.transform_epoch = 15;
    header.topology_epoch = 16;
    header.source_id = 17;
    header.targets_offset = sizeof(header);
    header.total_size = storage->bytes.size();
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(storage->bytes.data() + sizeof(header));
    targets[0].target_id = 101;
    targets[0].window_id = 201;
    targets[0].display_id = 301;
    targets[0].x_q8 = 1000;
    targets[0].y_q8 = 2000;
    targets[0].width_q8 = 1000;
    targets[0].height_q8 = 600;
    targets[0].safe_x_q8 = 1500;
    targets[0].safe_y_q8 = 2300;
    targets[0].confidence_q16 = UINT16_MAX;
    targets[0].role = SACCADE_TARGET_ROLE_TEXT_FIELD;
    targets[0].source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    targets[0].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                 SACCADE_TARGET_CAPABILITY_SCROLL | SACCADE_TARGET_CAPABILITY_DRAG_SOURCE |
                                 SACCADE_TARGET_CAPABILITY_TEXT | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    targets[0].flags = SACCADE_TARGET_ACTIONABLE;
    targets[1] = targets[0];
    targets[1].target_id = 102;
    targets[1].x_q8 = 4000;
    targets[1].safe_x_q8 = 4500;
    targets[1].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                 SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
                                 SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    targets[1].order = 1;
    for (uint32_t index = 2; index < SACCADE_INPUT_PLAN_MAX_TARGETS; ++index) {
        targets[index] = targets[0];
        targets[index].target_id = 101U + index;
        targets[index].x_q8 += static_cast<int32_t>(index * 1024U);
        targets[index].safe_x_q8 += static_cast<int32_t>(index * 1024U);
        targets[index].order = index;
    }
}

saccade::interaction::ActionContext context() noexcept {
    saccade::interaction::ActionContext value{};
    value.plan_id = 1;
    value.scene_epoch = 11;
    value.transform_epoch = 15;
    value.topology_epoch = 16;
    value.permission_epoch = 21;
    value.focus_id = 201;
    value.now_ns = 100;
    value.deadline_ns = 1000;
    value.permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD |
                        SACCADE_INPUT_PERMISSION_TEXT | SACCADE_INPUT_PERMISSION_WINDOW;
    return value;
}

} // namespace

int main() {
    static SceneStorage scene_storage;
    static saccade::interaction::ActionPlanStorage plan_storage;
    make_scene(&scene_storage);
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet({scene_storage.bytes.data(), scene_storage.bytes.size()}, &scene) !=
        SACCADE_OK) {
        return to_process_exit_code(ExitCode::invalid_scene);
    }
    saccade::interaction::ActionPlanner planner;
    auto action_context = context();
    const std::array<uint64_t, 2> drag_targets{101, 102};
    saccade::interaction::ActionRequest request{};
    request.kind = saccade::interaction::ActionKind::drag;
    request.target_ids = drag_targets.data();
    request.target_count = static_cast<uint32_t>(drag_targets.size());
    request.duration_ns = 50;
    SaccadeSpanU8 packet{};
    saccade::test::begin_allocation_tracking();
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK) {
        return to_process_exit_code(ExitCode::drag_build);
    }
    saccade::input::PlanView plan{};
    saccade::input::PhysicalInputReducer physical;
    if (saccade::input::validate_plan(packet, &plan) != SACCADE_OK || plan.header->command_count != 4 ||
        plan.commands[1].kind != SACCADE_INPUT_COMMAND_BUTTON_DOWN ||
        plan.commands[3].kind != SACCADE_INPUT_COMMAND_BUTTON_UP || physical.initialize(21, 0, 0) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(2) != SACCADE_OK || physical.state().buttons != SACCADE_INPUT_BUTTON_LEFT) {
        return to_process_exit_code(ExitCode::drag_physical);
    }
    saccade::input::SyntheticRelease release{};
    if (physical.abort(&release) != SACCADE_OK || release.buttons != SACCADE_INPUT_BUTTON_LEFT ||
        physical.state().buttons != 0) {
        return to_process_exit_code(ExitCode::drag_abort);
    }

    action_context.plan_id = 20;
    request = {};
    request.kind = saccade::interaction::ActionKind::text_select;
    request.target_ids = drag_targets.data();
    request.target_count = static_cast<uint32_t>(drag_targets.size());
    request.duration_ns = 50;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK || plan.header->command_count != 4 ||
        plan.commands[0].kind != SACCADE_INPUT_COMMAND_POINTER_MOVE ||
        plan.commands[1].kind != SACCADE_INPUT_COMMAND_BUTTON_DOWN ||
        plan.commands[2].kind != SACCADE_INPUT_COMMAND_POINTER_MOVE ||
        plan.commands[3].kind != SACCADE_INPUT_COMMAND_BUTTON_UP ||
        plan.commands[2].duration_ns != request.duration_ns) {
        return to_process_exit_code(ExitCode::text_select);
    }

    const uint64_t source = 101;
    action_context.plan_id = 2;
    request = {};
    request.kind = saccade::interaction::ActionKind::hold;
    request.target_ids = &source;
    request.target_count = 1;
    request.duration_ns = 500;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK ||
        physical.state().buttons != SACCADE_INPUT_BUTTON_LEFT || physical.state().active_lease_id != 2) {
        return to_process_exit_code(ExitCode::hold_build);
    }

    action_context.plan_id = 3;
    action_context.expected_buttons = SACCADE_INPUT_BUTTON_LEFT;
    request = {};
    request.kind = saccade::interaction::ActionKind::release;
    request.button = SACCADE_INPUT_BUTTON_LEFT;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK || plan.header->window_id != 0 ||
        plan.header->display_id != 0 ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK || physical.state().buttons != 0 ||
        physical.state().active_lease_id != 0) {
        return to_process_exit_code(ExitCode::release_build);
    }

    const std::array<uint8_t, 5> text{'h', 'e', 'l', 'l', 'o'};
    action_context = context();
    action_context.plan_id = 4;
    request = {};
    request.kind = saccade::interaction::ActionKind::text;
    request.target_ids = &source;
    request.target_count = 1;
    request.text = {text.data(), text.size()};
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK || plan.header->command_count != 2 ||
        plan.commands[1].payload_size != text.size() ||
        std::memcmp(packet.data + plan.commands[1].payload_offset, text.data(), text.size()) != 0) {
        return to_process_exit_code(ExitCode::text_build);
    }

    const std::array<uint64_t, 2> click_targets{101, 102};
    action_context.plan_id = 5;
    request = {};
    request.kind = saccade::interaction::ActionKind::click;
    request.target_ids = click_targets.data();
    request.target_count = static_cast<uint32_t>(click_targets.size());
    request.button = SACCADE_INPUT_BUTTON_RIGHT;
    request.repeat_count = 2;
    request.modifiers = SACCADE_INPUT_MODIFIER_CONTROL;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK || plan.header->command_count != 2 ||
        plan.commands[0].data2 != SACCADE_INPUT_MODIFIER_CONTROL || plan.commands[1].data1 != 2) {
        return to_process_exit_code(ExitCode::click_build);
    }

    std::array<uint64_t, SACCADE_INPUT_PLAN_MAX_TARGETS> maximum_click_targets{};
    for (uint32_t index = 0; index < maximum_click_targets.size(); ++index) {
        maximum_click_targets[index] = 101U + index;
    }
    action_context.plan_id = 21;
    request = {};
    request.kind = saccade::interaction::ActionKind::click;
    request.target_ids = maximum_click_targets.data();
    request.target_count = static_cast<uint32_t>(maximum_click_targets.size());
    request.pointer_duration_ns = 1;
    request.move_to_final_pointer = true;
    request.final_pointer = {700, 800};
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        plan.header->command_count != SACCADE_INPUT_PLAN_MAX_COMMANDS ||
        plan.commands[SACCADE_INPUT_PLAN_MAX_COMMANDS - 1U].kind != SACCADE_INPUT_COMMAND_POINTER_MOVE ||
        plan.commands[SACCADE_INPUT_PLAN_MAX_COMMANDS - 1U].target_id != 0) {
        return to_process_exit_code(ExitCode::maximum_click);
    }

    const saccade::geometry::PointQ8 outside_target{scene.targets[0].x_q8 + scene.targets[0].width_q8,
                                                    scene.targets[0].safe_y_q8};
    action_context.plan_id = 19;
    request.target_ids = &source;
    request.target_count = 1;
    request.target_points = &outside_target;
    request.target_point_count = 1;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_ERROR_NOT_FOUND) {
        return to_process_exit_code(ExitCode::outside_target);
    }

    action_context.plan_id = 6;
    action_context.plan_flags = SACCADE_INPUT_PLAN_DRY_RUN | SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    request.target_points = nullptr;
    request.target_point_count = 0;
    const int32_t pointer_before_dry_run = physical.state().pointer_x_q8;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK ||
        physical.state().pointer_x_q8 != pointer_before_dry_run) {
        return to_process_exit_code(ExitCode::dry_run);
    }

    action_context = context();
    action_context.plan_id = 7;
    request = {};
    request.kind = saccade::interaction::ActionKind::scroll;
    request.target_ids = &source;
    request.target_count = 1;
    request.delta_y_q8 = -512;
    request.duration_ns = 80;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        plan.commands[0].kind != SACCADE_INPUT_COMMAND_SCROLL ||
        (plan.commands[0].flags & SACCADE_INPUT_COMMAND_CONTINUOUS) == 0) {
        return to_process_exit_code(ExitCode::scroll_build);
    }

    action_context.plan_id = 8;
    request = {};
    request.kind = saccade::interaction::ActionKind::key;
    request.key_usage = 0x04;
    request.modifiers = SACCADE_INPUT_MODIFIER_SHIFT;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK || physical.state().active_lease_id != 0) {
        return to_process_exit_code(ExitCode::key_build);
    }

    action_context.plan_id = 9;
    request = {};
    request.kind = saccade::interaction::ActionKind::hold;
    request.target_ids = &source;
    request.target_count = 1;
    request.duration_ns = 500;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK ||
        physical.physical_override(900, 800, &release) != SACCADE_OK || release.buttons != SACCADE_INPUT_BUTTON_LEFT ||
        physical.state().pointer_x_q8 != 900) {
        return to_process_exit_code(ExitCode::physical_override);
    }

    action_context.plan_id = 10;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(plan.header->command_count) != SACCADE_OK ||
        physical.expire(action_context.deadline_ns, &release) != SACCADE_OK ||
        release.buttons != SACCADE_INPUT_BUTTON_LEFT) {
        return to_process_exit_code(ExitCode::expire);
    }

    action_context.plan_id = 11;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_OK ||
        saccade::input::validate_plan(packet, &plan) != SACCADE_OK ||
        physical.begin(plan, action_context.permissions, action_context.now_ns) != SACCADE_OK ||
        physical.advance(2) != SACCADE_OK || physical.backend_failure(&release) != SACCADE_OK ||
        release.buttons != SACCADE_INPUT_BUTTON_LEFT) {
        return to_process_exit_code(ExitCode::backend_failure);
    }

    action_context.transform_epoch = 99;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_ERROR_STALE_HANDLE) {
        return to_process_exit_code(ExitCode::stale_handle);
    }
    action_context = context();
    action_context.permissions = 0;
    if (planner.build(scene, action_context, request, &plan_storage, &packet) != SACCADE_ERROR_UNSUPPORTED) {
        return to_process_exit_code(ExitCode::unsupported);
    }
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (allocations != 0 || physical.shutdown(&release) != SACCADE_OK) {
        return to_process_exit_code(ExitCode::shutdown);
    }
    return to_process_exit_code(ExitCode::success);
}

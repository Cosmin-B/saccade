#include "application/interaction_controller.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    scene_failed,
    engine_failed,
    controller_failed,
    action_failed,
    mode_failed,
    repeat_failed,
    physical_input_failed,
    forward_failed,
    free_pointer_failed,
    text_failed,
    scroll_failed,
    tick_failed,
    shutdown_failed
};

constexpr uint64_t scene_epoch = 101;
constexpr uint64_t frame_id = 102;
constexpr uint64_t model_epoch = 103;
constexpr uint64_t session_epoch = 104;
constexpr uint64_t transform_epoch = 105;
constexpr uint64_t topology_epoch = 106;
constexpr uint64_t source_id = 107;
constexpr uint64_t permission_epoch = 108;
constexpr uint64_t focus_id = 109;
constexpr uint64_t first_timestamp_ns = 110;
constexpr uint64_t second_timestamp_ns = 111;
constexpr uint64_t final_timestamp_ns = 112;
constexpr uint64_t timeout_ns = 1000;
constexpr uint32_t expected_click_commands = 2;
constexpr int32_t adjusted_outside_target_x_q8 = 192;
constexpr uint32_t target_count = 2;
constexpr std::array<uint16_t, 2> hint_alphabet{static_cast<uint16_t>('A'), static_cast<uint16_t>('S')};
constexpr size_t packet_size = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord);

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct alignas(SaccadeTargetPacketHeader) ScenePacket {
    std::array<uint8_t, packet_size> bytes{};
};

struct Capture {
    saccade::application::InteractionState state{scene_epoch,
                                                 transform_epoch,
                                                 topology_epoch,
                                                 permission_epoch,
                                                 focus_id,
                                                 SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_WINDOW,
                                                 0};
    uint32_t executions = 0;
    uint32_t forwarded = 0;
    uint32_t neutralized = 0;
    bool lease_active = false;
    saccade::application::Command last_forwarded = saccade::application::Command::pointer_move;
    uint32_t command_count = 0;
    int32_t command_x_q8 = 0;
    uint32_t last_command_kind = 0;
    uint32_t last_payload_size = 0;
    int32_t last_delta_x_q8 = 0;
    int32_t last_delta_y_q8 = 0;
    uint64_t last_duration_ns = 0;
};

SaccadeResult execute(void* context, SaccadeSpanU8 plan, uint32_t, uint64_t) noexcept {
    auto* capture = static_cast<Capture*>(context);
    saccade::input::PlanView view{};
    if (saccade::input::validate_plan(plan, &view) != SACCADE_OK) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    capture->command_count = view.header->command_count;
    capture->command_x_q8 = view.header->command_count == 0 ? 0 : view.commands[0].x_q8;
    capture->last_command_kind =
        view.header->command_count == 0 ? 0 : view.commands[view.header->command_count - 1U].kind;
    capture->last_payload_size =
        view.header->command_count == 0 ? 0 : view.commands[view.header->command_count - 1U].payload_size;
    capture->last_delta_x_q8 =
        view.header->command_count == 0 ? 0 : view.commands[view.header->command_count - 1U].delta_x_q8;
    capture->last_delta_y_q8 =
        view.header->command_count == 0 ? 0 : view.commands[view.header->command_count - 1U].delta_y_q8;
    capture->last_duration_ns =
        view.header->command_count == 0 ? 0 : view.commands[view.header->command_count - 1U].duration_ns;
    ++capture->executions;
    return SACCADE_OK;
}

SaccadeResult read_state(void* context, saccade::application::InteractionState* output) noexcept {
    *output = static_cast<Capture*>(context)->state;
    return SACCADE_OK;
}

SaccadeResult forward(void* context, saccade::application::Command command, uint64_t) noexcept {
    auto* capture = static_cast<Capture*>(context);
    capture->last_forwarded = command;
    ++capture->forwarded;
    return SACCADE_OK;
}

bool input_lease_active(void* context) noexcept {
    return static_cast<Capture*>(context)->lease_active;
}

SaccadeResult neutralize_input(void* context) noexcept {
    auto* capture = static_cast<Capture*>(context);
    capture->lease_active = false;
    ++capture->neutralized;
    return SACCADE_OK;
}

SaccadeSpanU8 make_scene(ScenePacket* packet) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = scene_epoch;
    header.frame_id = frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.source_id = source_id;
    header.targets_offset = sizeof(header);
    header.total_size = packet->bytes.size();
    std::memcpy(packet->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(packet->bytes.data() + sizeof(header));
    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeTargetRecord& target = targets[index];
        target.target_id = index + 1U;
        target.x_q8 = static_cast<int32_t>((index + 1U) * 256U);
        target.y_q8 = 256;
        target.width_q8 = 256;
        target.height_q8 = 256;
        target.safe_x_q8 = target.x_q8 + 128;
        target.safe_y_q8 = target.y_q8 + 128;
        target.confidence_q16 = UINT16_MAX;
        target.role = SACCADE_TARGET_ROLE_BUTTON;
        target.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                 SACCADE_TARGET_CAPABILITY_SCROLL | SACCADE_TARGET_CAPABILITY_DRAG_SOURCE |
                                 SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
                                 SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }
    return {packet->bytes.data(), packet->bytes.size()};
}

SaccadeResult enter_label(saccade::application::SessionEngine* session, uint32_t index,
                          uint64_t timestamp_ns) noexcept {
    const saccade::interaction::HintLabel& label = session->labels()[index];
    saccade::application::SessionEvent event{};
    for (uint32_t symbol = 0; symbol < label.symbol_count; ++symbol) {
        const SaccadeResult entered = session->enter_symbol(label.symbols[symbol], timestamp_ns, &event);
        if (entered != SACCADE_OK) return entered;
    }
    return SACCADE_OK;
}

} // namespace

int main() {
    static saccade::scene::SceneStoreStorage scene_storage;
    static saccade::application::SessionStorage session_storage;
    static ScenePacket packet;
    saccade::scene::SceneStore scenes;
    Capture capture{};
    saccade::application::SessionEngine session;
    if (scenes.initialize(&scene_storage) != SACCADE_OK || scenes.publish_copy(make_scene(&packet)) != SACCADE_OK) {
        return result(TestResult::scene_failed);
    }
    if (session.initialize(&scenes, &session_storage, {&capture, execute}) != SACCADE_OK) {
        return result(TestResult::engine_failed);
    }
    saccade::application::InteractionProfile profile{};
    std::copy(hint_alphabet.begin(), hint_alphabet.end(), profile.hints.alphabet.begin());
    profile.hints.alphabet_count = static_cast<uint32_t>(hint_alphabet.size());
    profile.timeout_ns = timeout_ns;
    profile.scroll_duration_ns = timeout_ns / 2U;
    profile.initial_mode = saccade::interaction::SelectionMode::single;
    saccade::application::InteractionController controller;
    if (controller.initialize(&session, profile, {&capture, read_state},
                              {&capture, forward, input_lease_active, neutralize_input}) != SACCADE_OK) {
        return result(TestResult::controller_failed);
    }
    saccade::application::InteractionCommandResult command{};
    if (controller.dispatch(saccade::application::Command::left_click, first_timestamp_ns, &command) != SACCADE_OK ||
        !command.action_started || !session.active() ||
        controller.dispatch(saccade::application::Command::target_position_1, first_timestamp_ns, &command) !=
            SACCADE_OK ||
        !command.target_adjusted ||
        controller.dispatch(saccade::application::Command::edge_snap_right, first_timestamp_ns, &command) !=
            SACCADE_OK ||
        !command.target_adjusted ||
        controller.dispatch(saccade::application::Command::nudge_left, first_timestamp_ns, &command) != SACCADE_OK ||
        !command.target_adjusted || enter_label(&session, 0, first_timestamp_ns) != SACCADE_OK ||
        capture.executions != 1 || capture.command_count != 1 || capture.command_x_q8 != adjusted_outside_target_x_q8) {
        return result(TestResult::action_failed);
    }
    if (controller.dispatch(saccade::application::Command::double_click, second_timestamp_ns, &command) != SACCADE_OK ||
        !command.action_started || !session.active() ||
        controller.dispatch(saccade::application::Command::mode_multi, final_timestamp_ns, &command) != SACCADE_OK ||
        !command.mode_changed || session.active() ||
        controller.selection_mode() != saccade::interaction::SelectionMode::multi ||
        controller.dispatch(saccade::application::Command::repeat_action, final_timestamp_ns, &command) != SACCADE_OK ||
        !command.action_started || enter_label(&session, 0, final_timestamp_ns) != SACCADE_OK ||
        enter_label(&session, 1, final_timestamp_ns) != SACCADE_OK ||
        session.confirm(final_timestamp_ns, &command.session) != SACCADE_OK || capture.executions != 2 ||
        capture.command_count != expected_click_commands) {
        return result(TestResult::mode_failed);
    }
    if (controller.dispatch(saccade::application::Command::repeat_action, final_timestamp_ns + 1U, &command) !=
            SACCADE_OK ||
        !command.action_started || controller.observe_physical_input(final_timestamp_ns + 1U) != SACCADE_OK ||
        session.active()) {
        return result(TestResult::repeat_failed);
    }
    capture.lease_active = true;
    if (saccade::application::start_interaction_command(&controller, saccade::application::Command::drag,
                                                        final_timestamp_ns + 1U) != SACCADE_OK ||
        !session.active()) {
        return result(TestResult::physical_input_failed);
    }
    saccade::application::observe_interaction_input(&controller, final_timestamp_ns + 2U);
    if (session.active() || capture.neutralized != 1) {
        return result(TestResult::physical_input_failed);
    }
    if (controller.dispatch(saccade::application::Command::open_settings, final_timestamp_ns + 3U, &command) !=
            SACCADE_OK ||
        !command.forwarded || capture.forwarded != 1 ||
        capture.last_forwarded != saccade::application::Command::open_settings) {
        return result(TestResult::forward_failed);
    }
    if (controller.dispatch(saccade::application::Command::free_pointer, final_timestamp_ns + 4U, &command) !=
            SACCADE_OK ||
        !command.action_started || enter_label(&session, 0, final_timestamp_ns + 4U) != SACCADE_OK ||
        !session.active() || capture.executions != 2 ||
        controller.dispatch(saccade::application::Command::nudge_right, final_timestamp_ns + 5U, &command) !=
            SACCADE_OK ||
        !command.target_adjusted || session.confirm(final_timestamp_ns + 6U, &command.session) != SACCADE_OK ||
        session.active() || capture.executions != 3)
        return result(TestResult::free_pointer_failed);
    constexpr std::array<uint8_t, 2> text{{'H', 'i'}};
    capture.state.permissions |= SACCADE_INPUT_PERMISSION_TEXT;
    if (controller.set_text({text.data(), text.size()}) != SACCADE_OK ||
        controller.dispatch(saccade::application::Command::type_text, final_timestamp_ns + 7U, &command) !=
            SACCADE_OK ||
        !session.active() || enter_label(&session, 0, final_timestamp_ns + 7U) != SACCADE_OK ||
        capture.executions != 4 || capture.command_count != 2 ||
        capture.last_command_kind != SACCADE_INPUT_COMMAND_TEXT || capture.last_payload_size != text.size())
        return result(TestResult::text_failed);
    if (controller.dispatch(saccade::application::Command::scroll_up, final_timestamp_ns + 8U, &command) !=
            SACCADE_OK ||
        !session.active() || enter_label(&session, 0, final_timestamp_ns + 8U) != SACCADE_OK ||
        capture.executions != 5 || capture.last_command_kind != SACCADE_INPUT_COMMAND_SCROLL ||
        capture.last_delta_x_q8 != 0 || capture.last_delta_y_q8 != -saccade::application::default_scroll_step_q8 ||
        capture.last_duration_ns != 0 ||
        controller.dispatch(saccade::application::Command::scroll_right_continuous, final_timestamp_ns + 9U,
                            &command) != SACCADE_OK ||
        !session.active() || enter_label(&session, 0, final_timestamp_ns + 9U) != SACCADE_OK ||
        capture.executions != 6 || capture.last_command_kind != SACCADE_INPUT_COMMAND_SCROLL ||
        capture.last_delta_x_q8 != saccade::application::default_scroll_step_q8 || capture.last_delta_y_q8 != 0 ||
        capture.last_duration_ns != timeout_ns / 2U) {
        return result(TestResult::scroll_failed);
    }
    if (controller.dispatch(saccade::application::Command::pointer_move, final_timestamp_ns + 10U, &command) !=
            SACCADE_OK ||
        !session.active()) {
        return result(TestResult::tick_failed);
    }
    capture.state.permission_epoch = 0;
    if (controller.tick(final_timestamp_ns + 11U) != SACCADE_ERROR_PERMISSION || session.active()) {
        return result(TestResult::tick_failed);
    }
    if (controller.shutdown() != SACCADE_OK || session.shutdown() != SACCADE_OK) {
        return result(TestResult::shutdown_failed);
    }
    return result(TestResult::success);
}

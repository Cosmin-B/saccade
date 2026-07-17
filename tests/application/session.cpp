#include "application/session.hpp"
#include "input/plan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    store_initialization_failed,
    scene_publish_failed,
    engine_initialization_failed,
    single_begin_failed,
    invalid_prefix_failed,
    single_execution_failed,
    dual_execution_failed,
    multi_execution_failed,
    text_copy_failed,
    stable_refresh_failed,
    stale_permission_failed,
    shutdown_failed
};

constexpr uint64_t scene_epoch = 11;
constexpr uint64_t frame_id = 12;
constexpr uint64_t model_epoch = 13;
constexpr uint64_t session_epoch = 14;
constexpr uint64_t transform_epoch = 15;
constexpr uint64_t topology_epoch = 16;
constexpr uint64_t source_id = 17;
constexpr uint64_t permission_epoch = 18;
constexpr uint64_t focus_id = 19;
constexpr uint64_t action_time_ns = 20;
constexpr uint64_t action_deadline_ns = 1000;
constexpr uint64_t first_plan_id = 21;
constexpr uint64_t second_plan_id = 22;
constexpr uint64_t third_plan_id = 23;
constexpr uint64_t fourth_plan_id = 24;
constexpr uint64_t fifth_plan_id = 25;
constexpr uint64_t sixth_plan_id = 26;
constexpr uint64_t refreshed_scene_epoch = scene_epoch + 1U;
constexpr uint64_t refreshed_frame_id = frame_id + 1U;
constexpr uint32_t target_count = 3;
constexpr uint32_t expected_drag_commands = 4;
constexpr uint32_t text_command_index = 1;
constexpr uint16_t invalid_symbol = static_cast<uint16_t>('Z');
constexpr std::array<uint16_t, 2> hint_alphabet{static_cast<uint16_t>('A'), static_cast<uint16_t>('S')};
constexpr std::array<uint8_t, 5> original_text{static_cast<uint8_t>('h'), static_cast<uint8_t>('e'),
                                               static_cast<uint8_t>('l'), static_cast<uint8_t>('l'),
                                               static_cast<uint8_t>('o')};
constexpr size_t scene_packet_size = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord);

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct alignas(SaccadeTargetPacketHeader) ScenePacket {
    std::array<uint8_t, scene_packet_size> bytes{};
};

struct ExecutionCapture {
    std::array<uint8_t, saccade::interaction::maximum_action_plan_bytes> bytes{};
    size_t size = 0;
    uint32_t available_permissions = 0;
    uint64_t now_ns = 0;
    uint32_t calls = 0;
};

SaccadeResult capture_execution(void* context, SaccadeSpanU8 plan, uint32_t available_permissions,
                                uint64_t now_ns) noexcept {
    auto* capture = static_cast<ExecutionCapture*>(context);
    saccade::input::PlanView view{};
    const SaccadeResult validated = saccade::input::validate_plan(plan, &view);
    if (validated != SACCADE_OK || plan.size > capture->bytes.size()) return validated;
    std::memcpy(capture->bytes.data(), plan.data, plan.size);
    capture->size = plan.size;
    capture->available_permissions = available_permissions;
    capture->now_ns = now_ns;
    ++capture->calls;
    return SACCADE_OK;
}

SaccadeSpanU8 make_scene(ScenePacket* packet, uint64_t current_scene_epoch = scene_epoch,
                         uint64_t current_frame_id = frame_id) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = current_scene_epoch;
    header.frame_id = current_frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.source_id = source_id;
    header.targets_offset = sizeof(header);
    header.total_size = packet->bytes.size();
    std::memcpy(packet->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(packet->bytes.data() + sizeof(header));
    constexpr uint32_t capabilities = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                      SACCADE_TARGET_CAPABILITY_DRAG_SOURCE | SACCADE_TARGET_CAPABILITY_DROP_TARGET |
                                      SACCADE_TARGET_CAPABILITY_TEXT | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
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
        target.capability_bits = capabilities;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }
    return {packet->bytes.data(), packet->bytes.size()};
}

saccade::application::SessionConfig config(uint64_t plan_id, saccade::interaction::SelectionMode mode,
                                           saccade::interaction::ActionKind action) noexcept {
    saccade::application::SessionConfig value{};
    value.mode = mode;
    std::copy(hint_alphabet.begin(), hint_alphabet.end(), value.hints.alphabet.begin());
    value.hints.alphabet_count = static_cast<uint32_t>(hint_alphabet.size());
    value.action.plan_id = plan_id;
    value.action.scene_epoch = scene_epoch;
    value.action.transform_epoch = transform_epoch;
    value.action.topology_epoch = topology_epoch;
    value.action.permission_epoch = permission_epoch;
    value.action.focus_id = focus_id;
    value.action.now_ns = action_time_ns;
    value.action.deadline_ns = action_deadline_ns;
    value.action.permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_TEXT;
    value.request.kind = action;
    return value;
}

SaccadeResult enter_label(saccade::application::SessionEngine* engine, uint32_t label_index,
                          saccade::application::SessionEvent* event) noexcept {
    const saccade::interaction::HintLabel& source = engine->labels()[label_index];
    std::array<uint16_t, saccade::interaction::maximum_hint_symbols> symbols{};
    std::memcpy(symbols.data(), source.symbols.data(), static_cast<size_t>(source.symbol_count) * sizeof(uint16_t));
    SaccadeResult result_code = SACCADE_OK;
    for (uint32_t index = 0; index < source.symbol_count; ++index) {
        result_code = engine->enter_symbol(symbols[index], action_time_ns, event);
        if (result_code != SACCADE_OK) return result_code;
    }
    return result_code;
}

const SaccadeInputPlanHeader* captured_header(const ExecutionCapture& capture) noexcept {
    return reinterpret_cast<const SaccadeInputPlanHeader*>(capture.bytes.data());
}

const SaccadeInputCommand* captured_commands(const ExecutionCapture& capture) noexcept {
    return reinterpret_cast<const SaccadeInputCommand*>(capture.bytes.data() +
                                                        captured_header(capture)->commands_offset);
}

} // namespace

int main() {
    static saccade::scene::SceneStoreStorage scene_storage;
    static saccade::application::SessionStorage session_storage;
    static ScenePacket packet;
    saccade::scene::SceneStore scenes;
    if (scenes.initialize(&scene_storage) != SACCADE_OK) return result(TestResult::store_initialization_failed);
    if (scenes.publish_copy(make_scene(&packet)) != SACCADE_OK) return result(TestResult::scene_publish_failed);

    ExecutionCapture capture{};
    saccade::application::SessionEngine engine;
    if (engine.initialize(&scenes, &session_storage, {&capture, capture_execution}) != SACCADE_OK)
        return result(TestResult::engine_initialization_failed);

    auto single =
        config(first_plan_id, saccade::interaction::SelectionMode::single, saccade::interaction::ActionKind::click);
    if (engine.begin(single) != SACCADE_OK) return result(TestResult::single_begin_failed);
    saccade::application::SessionEvent event{};
    if (engine.set_target_position(9) != SACCADE_ERROR_INVALID_ARGUMENT ||
        engine.set_target_position(0) != SACCADE_OK ||
        engine.enter_symbol(invalid_symbol, action_time_ns, &event) != SACCADE_ERROR_NOT_FOUND ||
        engine.prefix_count() != 0)
        return result(TestResult::invalid_prefix_failed);
    if (enter_label(&engine, 0, &event) != SACCADE_OK || !event.action_executed || engine.active() ||
        capture.calls != 1 || captured_header(capture)->plan_id != first_plan_id ||
        captured_header(capture)->command_count != 1 ||
        captured_commands(capture)[0].kind != SACCADE_INPUT_COMMAND_CLICK ||
        captured_commands(capture)[0].x_q8 != 319 || captured_commands(capture)[0].y_q8 != 319)
        return result(TestResult::single_execution_failed);

    auto dual =
        config(second_plan_id, saccade::interaction::SelectionMode::dual, saccade::interaction::ActionKind::drag);
    if (engine.begin(dual) != SACCADE_OK || enter_label(&engine, 0, &event) != SACCADE_OK ||
        enter_label(&engine, 1, &event) != SACCADE_OK || !event.action_executed ||
        captured_header(capture)->command_count != expected_drag_commands ||
        captured_commands(capture)[1].kind != SACCADE_INPUT_COMMAND_BUTTON_DOWN ||
        captured_commands(capture)[3].kind != SACCADE_INPUT_COMMAND_BUTTON_UP)
        return result(TestResult::dual_execution_failed);

    auto multi =
        config(third_plan_id, saccade::interaction::SelectionMode::multi, saccade::interaction::ActionKind::click);
    if (engine.begin(multi) != SACCADE_OK || enter_label(&engine, 0, &event) != SACCADE_OK ||
        enter_label(&engine, 1, &event) != SACCADE_OK || engine.confirm(action_time_ns, &event) != SACCADE_OK ||
        !event.action_executed || captured_header(capture)->command_count != 2)
        return result(TestResult::multi_execution_failed);

    std::array<uint8_t, original_text.size()> mutable_text = original_text;
    auto text =
        config(fourth_plan_id, saccade::interaction::SelectionMode::single, saccade::interaction::ActionKind::text);
    text.request.text = {mutable_text.data(), mutable_text.size()};
    if (engine.begin(text) != SACCADE_OK) return result(TestResult::text_copy_failed);
    mutable_text.fill(static_cast<uint8_t>('x'));
    if (enter_label(&engine, 0, &event) != SACCADE_OK || !event.action_executed) {
        return result(TestResult::text_copy_failed);
    }
    const SaccadeInputCommand& text_command = captured_commands(capture)[text_command_index];
    if (text_command.kind != SACCADE_INPUT_COMMAND_TEXT || text_command.payload_size != original_text.size() ||
        std::memcmp(capture.bytes.data() + text_command.payload_offset, original_text.data(), original_text.size()) !=
            0)
        return result(TestResult::text_copy_failed);

    auto stable_refresh =
        config(sixth_plan_id, saccade::interaction::SelectionMode::dual, saccade::interaction::ActionKind::drag);
    if (engine.begin(stable_refresh) != SACCADE_OK) return result(TestResult::stable_refresh_failed);

    std::array<saccade::interaction::HintLabel, target_count> frozen_labels{};
    std::copy_n(engine.labels(), target_count, frozen_labels.begin());
    if (enter_label(&engine, 0, &event) != SACCADE_OK || event.action_executed || event.selected_count != 1)
        return result(TestResult::stable_refresh_failed);

    make_scene(&packet, refreshed_scene_epoch, refreshed_frame_id);
    auto* refreshed_targets =
        reinterpret_cast<SaccadeTargetRecord*>(packet.bytes.data() + sizeof(SaccadeTargetPacketHeader));
    std::swap(refreshed_targets[0], refreshed_targets[2]);

    int32_t refreshed_drop_x_q8 = 0;
    for (uint32_t index = 0; index < target_count; ++index) {
        if (refreshed_targets[index].target_id != frozen_labels[1].target_id) continue;
        refreshed_targets[index].safe_x_q8 += 64;
        refreshed_drop_x_q8 = refreshed_targets[index].safe_x_q8;
        break;
    }
    if (refreshed_drop_x_q8 == 0 || scenes.publish_copy({packet.bytes.data(), packet.bytes.size()}) != SACCADE_OK)
        return result(TestResult::stable_refresh_failed);

    const saccade::application::SessionEpochs refreshed_epochs{refreshed_scene_epoch, transform_epoch, topology_epoch,
                                                               permission_epoch, focus_id};
    if (engine.tick(refreshed_epochs, action_time_ns) != SACCADE_OK || !engine.active() ||
        engine.scene_view().header->scene_epoch != refreshed_scene_epoch || engine.selection().target_count != 1)
        return result(TestResult::stable_refresh_failed);

    for (uint32_t index = 0; index < target_count; ++index) {
        const saccade::interaction::HintLabel& refreshed_label = engine.labels()[index];
        if (refreshed_label.target_id != frozen_labels[index].target_id ||
            refreshed_label.symbol_count != frozen_labels[index].symbol_count ||
            refreshed_label.symbols != frozen_labels[index].symbols ||
            engine.scene_view().targets[refreshed_label.target_index].target_id != refreshed_label.target_id) {
            return result(TestResult::stable_refresh_failed);
        }
    }

    if (enter_label(&engine, 1, &event) != SACCADE_OK || !event.action_executed || engine.active() ||
        captured_header(capture)->plan_id != sixth_plan_id ||
        captured_header(capture)->command_count != expected_drag_commands ||
        captured_commands(capture)[2].x_q8 != refreshed_drop_x_q8) {
        return result(TestResult::stable_refresh_failed);
    }

    auto stale =
        config(fifth_plan_id, saccade::interaction::SelectionMode::single, saccade::interaction::ActionKind::click);
    if (engine.begin_latest(stale) != SACCADE_OK) return result(TestResult::stale_permission_failed);
    const saccade::application::SessionEpochs changed_permission{refreshed_scene_epoch, transform_epoch, topology_epoch,
                                                                 permission_epoch + 1U, focus_id};
    if (engine.tick(changed_permission, action_time_ns) != SACCADE_ERROR_STALE_HANDLE || engine.active())
        return result(TestResult::stale_permission_failed);

    return engine.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

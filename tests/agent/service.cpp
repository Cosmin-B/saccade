#include "agent/service.hpp"
#include "input/plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    initialization_failed,
    observation_failed,
    query_failed,
    action_failed,
    capability_failed,
    text_capability_failed,
    stale_failed,
    freshness_failed,
    verification_failed,
    shutdown_failed
};

constexpr std::array<uint8_t, 12> target_text{'B', 'u', 't', 't', 'o', 'n', 'C', 'a', 'n', 'c', 'e', 'l'};
constexpr size_t packet_size =
    sizeof(SaccadeTargetPacketHeader) + 2U * sizeof(SaccadeTargetRecord) + target_text.size();

struct Fixture {
    alignas(8) std::array<uint8_t, packet_size> packet{};
    saccade::scene::PacketView scene{};
    saccade::interaction::InteractionState state{};
    SaccadeAgentPhysicalState physical{};
    uint32_t plans = 0;
    uint32_t aborts = 0;
    int32_t window_cycles = 0;
    int32_t last_x_q8 = 0;
    int32_t last_y_q8 = 0;
    uint64_t last_window_id = 0;
    uint64_t last_display_id = 0;
    bool advance_scene_on_plan = false;
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult acquire_scene(void* context, saccade::scene::PacketView* output) noexcept {
    *output = static_cast<Fixture*>(context)->scene;
    return SACCADE_OK;
}

SaccadeResult read_state(void* context, saccade::interaction::InteractionState* output) noexcept {
    *output = static_cast<Fixture*>(context)->state;
    return SACCADE_OK;
}

SaccadeResult execute_plan(void* context, SaccadeSpanU8 bytes, uint32_t permissions, uint64_t now_ns) noexcept {
    saccade::input::PlanView plan{};
    if (permissions != SACCADE_INPUT_PERMISSION_POINTER || now_ns != 1 ||
        saccade::input::validate_plan(bytes, &plan) != SACCADE_OK || plan.header->plan_id == 0 ||
        plan.header->command_count == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* fixture = static_cast<Fixture*>(context);
    fixture->last_window_id = plan.header->window_id;
    fixture->last_display_id = plan.header->display_id;
    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        if ((plan.commands[index].flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0) {
            fixture->last_x_q8 = plan.commands[index].x_q8;
            fixture->last_y_q8 = plan.commands[index].y_q8;
        }
    }
    if ((plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) == 0) {
        for (uint32_t index = 0; index < plan.header->command_count; ++index) {
            if (plan.commands[index].kind == SACCADE_INPUT_COMMAND_BUTTON_DOWN)
                fixture->state.expected_buttons |= plan.commands[index].data0;
            if (plan.commands[index].kind == SACCADE_INPUT_COMMAND_BUTTON_UP)
                fixture->state.expected_buttons &= ~plan.commands[index].data0;
        }
    }
    ++fixture->plans;
    if (fixture->advance_scene_on_plan) {
        auto* header = const_cast<SaccadeTargetPacketHeader*>(fixture->scene.header);
        ++header->scene_epoch;
        ++header->frame_id;
        fixture->state.scene_epoch = header->scene_epoch;
    }
    return SACCADE_OK;
}

SaccadeResult read_physical_state(void* context, SaccadeAgentPhysicalState* output) noexcept {
    *output = static_cast<Fixture*>(context)->physical;
    return SACCADE_OK;
}

SaccadeResult abort_input(void* context) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    fixture->state.expected_buttons = 0;
    ++fixture->aborts;
    return SACCADE_OK;
}

SaccadeResult cycle_window(void* context, bool backward) noexcept {
    static_cast<Fixture*>(context)->window_cycles += backward ? -1 : 1;
    return SACCADE_OK;
}

void make_scene(Fixture* fixture) noexcept {
    auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(fixture->packet.data());
    *header = {};
    header->struct_size = sizeof(*header);
    header->packet_version = SACCADE_TARGET_PACKET_VERSION;
    header->target_count = 2;
    header->target_stride = sizeof(SaccadeTargetRecord);
    header->flags = SACCADE_TARGET_PACKET_INCOMPLETE;
    header->coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header->scene_epoch = 42;
    header->frame_id = 43;
    header->model_epoch = 44;
    header->session_epoch = 45;
    header->transform_epoch = 46;
    header->topology_epoch = 47;
    header->source_id = 48;
    header->targets_offset = sizeof(*header);
    header->total_size = packet_size;
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(fixture->packet.data() + header->targets_offset);
    targets[0].target_id = 11;
    targets[0].window_id = 7;
    targets[0].display_id = 8;
    targets[0].x_q8 = 100;
    targets[0].y_q8 = 200;
    targets[0].width_q8 = 300;
    targets[0].height_q8 = 400;
    targets[0].safe_x_q8 = 250;
    targets[0].safe_y_q8 = 300;
    targets[0].confidence_q16 = 65000;
    targets[0].role = SACCADE_TARGET_ROLE_BUTTON;
    targets[0].source_bits = SACCADE_TARGET_SOURCE_NEURAL;
    targets[0].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                 SACCADE_TARGET_CAPABILITY_INVOKE | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    targets[0].flags = SACCADE_TARGET_ACTIONABLE;
    targets[0].text = {0, 6};
    targets[1] = targets[0];
    targets[1].target_id = 12;
    targets[1].window_id = 0;
    targets[1].display_id = 0;
    targets[1].role = SACCADE_TARGET_ROLE_LINK;
    targets[1].text = {6, 6};
    uint8_t* text = fixture->packet.data() + sizeof(*header) + 2U * sizeof(SaccadeTargetRecord);
    std::memcpy(text, target_text.data(), target_text.size());
    fixture->scene = {header, targets, packet_size, text, static_cast<uint32_t>(target_text.size())};
    fixture->state.scene_epoch = 42;
    fixture->state.transform_epoch = 46;
    fixture->state.topology_epoch = 47;
    fixture->state.permission_epoch = 9;
    fixture->state.focus_id = 700;
    fixture->state.window_id = 7;
    fixture->state.display_id = 8;
    fixture->state.window_bounds = {10, 20, 1000, 800};
    fixture->state.permissions = SACCADE_INPUT_PERMISSION_POINTER;
    fixture->physical.permission_epoch = 9;
}

void set_scene_generation(Fixture* fixture, uint64_t scene_epoch, uint64_t frame_id) noexcept {
    auto* header = const_cast<SaccadeTargetPacketHeader*>(fixture->scene.header);
    header->scene_epoch = scene_epoch;
    header->frame_id = frame_id;
    fixture->state.scene_epoch = scene_epoch;
}

} // namespace

int main() {
    static Fixture fixture;
    make_scene(&fixture);
    saccade::agent::Service service;
    const SaccadeAgentCapabilityBits capabilities = SACCADE_AGENT_CAPABILITY_OBSERVE |
                                                    SACCADE_AGENT_CAPABILITY_POINTER |
                                                    SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_WINDOW;
    if (service.initialize({&fixture, acquire_scene, read_state, execute_plan, read_physical_state, abort_input,
                            cycle_window, capabilities}) != SACCADE_OK)
        return result(TestResult::initialization_failed);

    static std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> output{};
    SaccadeAgentObserveRequest observe{};
    observe.header.struct_size = sizeof(observe);
    observe.header.api_version = SACCADE_AGENT_API_VERSION;
    observe.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    observe.request_id = 1;
    observe.scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    observe.freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    observe.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    observe.maximum_targets = 10;
    observe.target_stride = static_cast<uint32_t>(sizeof(SaccadeAgentTarget));
    observe.total_capacity = static_cast<uint32_t>(output.size());
    size_t output_size = 0;
    if (service.process({reinterpret_cast<const uint8_t*>(&observe), sizeof(observe)}, capabilities, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK)
        return result(TestResult::observation_failed);
    const auto* observed = reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data());
    const auto* observed_target = reinterpret_cast<const SaccadeAgentTarget*>(output.data() + observed->targets_offset);
    if (observed->target_count != 2 || observed->generation.generation != 42 || observed->generation.focus_id != 700 ||
        (observed->header.flags & SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE) == 0 ||
        observed->generation.window_id != 7 || observed->generation.display_id != 8 ||
        observed->generation.topology_epoch != 47 || observed_target->target_id != 11 ||
        (observed_target->capability_bits & SACCADE_AGENT_TARGET_TEXT_SELECT) == 0 || observed_target->text_size != 6 ||
        std::memcmp(output.data() + observed_target->text_offset, "Button", 6) != 0)
        return result(TestResult::observation_failed);

    observe.freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    observe.freshness.after_generation = 42;
    observe.freshness.timeout_ns = 1000;
    if (service.process({reinterpret_cast<const uint8_t*>(&observe), sizeof(observe)}, capabilities, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data())->result != SACCADE_AGENT_ERROR_TIMEOUT)
        return result(TestResult::freshness_failed);
    set_scene_generation(&fixture, 43, 44);
    if (service.process({reinterpret_cast<const uint8_t*>(&observe), sizeof(observe)}, capabilities, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data())->generation.generation != 43)
        return result(TestResult::freshness_failed);
    set_scene_generation(&fixture, 42, 43);
    observe.freshness = {SACCADE_AGENT_FRESHNESS_LATEST_VALID, 0, 0, 0};

    observe.scope.kind = SACCADE_AGENT_SCOPE_ACTIVE_WINDOW;
    if (service.process({reinterpret_cast<const uint8_t*>(&observe), sizeof(observe)}, capabilities, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK)
        return result(TestResult::observation_failed);
    observed = reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data());
    observed_target = reinterpret_cast<const SaccadeAgentTarget*>(output.data() + observed->targets_offset);
    if (observed->target_count != 2 || observed->scope.stable_id != fixture.state.window_id ||
        observed->scope.rect.x_q8 != fixture.state.window_bounds.x ||
        observed->scope.rect.y_q8 != fixture.state.window_bounds.y ||
        observed->scope.rect.width_q8 != fixture.state.window_bounds.width ||
        observed->scope.rect.height_q8 != fixture.state.window_bounds.height ||
        observed_target[1].window_id != fixture.state.window_id ||
        observed_target[1].display_id != fixture.state.display_id)
        return result(TestResult::observation_failed);
    auto* scene_targets = const_cast<SaccadeTargetRecord*>(fixture.scene.targets);
    scene_targets[1].x_q8 = 2000;
    if (service.process({reinterpret_cast<const uint8_t*>(&observe), sizeof(observe)}, capabilities, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentObserveCompletion*>(output.data())->target_count != 1)
        return result(TestResult::observation_failed);
    scene_targets[1].x_q8 = scene_targets[0].x_q8;

    alignas(8) std::array<uint8_t, sizeof(SaccadeAgentQueryRequest) + sizeof(SaccadeAgentQueryFilter) + 6U>
        query_bytes{};
    auto* query = reinterpret_cast<SaccadeAgentQueryRequest*>(query_bytes.data());
    query->header.struct_size = sizeof(*query);
    query->header.api_version = SACCADE_AGENT_API_VERSION;
    query->header.message_kind = SACCADE_AGENT_MESSAGE_QUERY_REQUEST;
    query->request_id = 2;
    query->generation = 42;
    query->scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    query->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    query->maximum_results = 10;
    query->filter_count = 1;
    query->filter_stride = static_cast<uint32_t>(sizeof(SaccadeAgentQueryFilter));
    query->filters_offset = static_cast<uint32_t>(sizeof(*query));
    query->total_size = static_cast<uint32_t>(query_bytes.size());
    query->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    auto* filter = reinterpret_cast<SaccadeAgentQueryFilter*>(query_bytes.data() + query->filters_offset);
    filter->text_offset = static_cast<uint32_t>(sizeof(SaccadeAgentQueryRequest) + sizeof(SaccadeAgentQueryFilter));
    std::memcpy(query_bytes.data() + filter->text_offset, "Button", 6);
    filter->flags = SACCADE_AGENT_QUERY_ROLE;
    filter->role = SACCADE_AGENT_ROLE_BUTTON;
    query->filters_offset = 0;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::query_failed);
    query->filters_offset = static_cast<uint32_t>(sizeof(*query));
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->target_count != 1 ||
        (reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->header.flags &
         SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE) == 0)
        return result(TestResult::query_failed);
    filter->flags = SACCADE_AGENT_QUERY_TEXT;
    filter->text_match = SACCADE_AGENT_TEXT_EXACT;
    filter->text_size = 6;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->target_count != 1)
        return result(TestResult::query_failed);

    query->generation = 0;
    query->freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    query->freshness.after_generation = 42;
    query->freshness.timeout_ns = 1000;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->result != SACCADE_AGENT_ERROR_TIMEOUT)
        return result(TestResult::freshness_failed);
    set_scene_generation(&fixture, 43, 44);
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->result != SACCADE_AGENT_OK)
        return result(TestResult::freshness_failed);
    set_scene_generation(&fixture, 42, 43);

    query->freshness.policy = SACCADE_AGENT_FRESHNESS_FORCE_REFRESH;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED)
        return result(TestResult::freshness_failed);
    query->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    filter->text_match = SACCADE_AGENT_TEXT_PREFIX;
    filter->text_size = 3;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->target_count != 1)
        return result(TestResult::query_failed);
    filter->text_match = SACCADE_AGENT_TEXT_SUBSTRING;
    filter->text_offset += 2;
    filter->text_size = 3;
    if (service.process({query_bytes.data(), query_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentQueryCompletion*>(output.data())->target_count != 1)
        return result(TestResult::query_failed);

    alignas(8) std::array<uint8_t, sizeof(SaccadeAgentActionBatch) + 2U * sizeof(SaccadeAgentAction)> action_bytes{};
    auto* batch = reinterpret_cast<SaccadeAgentActionBatch*>(action_bytes.data());
    batch->header.struct_size = sizeof(*batch);
    batch->header.api_version = SACCADE_AGENT_API_VERSION;
    batch->header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_BATCH;
    batch->header.flags = SACCADE_AGENT_BATCH_DRY_RUN;
    batch->request_id = 3;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_POINTER;
    batch->policy = SACCADE_AGENT_BATCH_STOP_ON_FAILURE;
    batch->deadline_ns = 1000;
    batch->preconditions.flags = SACCADE_AGENT_PRECONDITION_GENERATION | SACCADE_AGENT_PRECONDITION_FOCUS |
                                 SACCADE_AGENT_PRECONDITION_WINDOW | SACCADE_AGENT_PRECONDITION_DISPLAY |
                                 SACCADE_AGENT_PRECONDITION_TRANSFORM | SACCADE_AGENT_PRECONDITION_PERMISSION;
    batch->preconditions.generation = 42;
    batch->preconditions.focus_id = 700;
    batch->preconditions.window_id = 7;
    batch->preconditions.display_id = 8;
    batch->preconditions.transform_epoch = 46;
    batch->preconditions.permission_epoch = 9;
    batch->action_count = 1;
    batch->action_stride = static_cast<uint32_t>(sizeof(SaccadeAgentAction));
    batch->actions_offset = static_cast<uint32_t>(sizeof(*batch));
    batch->payload_offset = static_cast<uint32_t>(action_bytes.size());
    batch->total_size = static_cast<uint32_t>(action_bytes.size());
    auto* action = reinterpret_cast<SaccadeAgentAction*>(action_bytes.data() + batch->actions_offset);
    action->kind = SACCADE_AGENT_ACTION_CLICK;
    action->target_id = 12;
    action->button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action->repeat_count = 1;
    batch->actions_offset = 0;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::action_failed);
    batch->actions_offset = static_cast<uint32_t>(sizeof(*batch));
    batch->payload_offset = batch->actions_offset;
    batch->payload_size = 1;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::action_failed);
    batch->payload_offset = static_cast<uint32_t>(action_bytes.size());
    batch->payload_size = 0;
    batch->header.flags = SACCADE_AGENT_BATCH_DRY_RUN | SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION;
    if (service.process({action_bytes.data(), action_bytes.size()}, SACCADE_AGENT_CAPABILITY_POINTER, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_CAPABILITY_DENIED ||
        fixture.plans != 0)
        return result(TestResult::verification_failed);
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_ERROR_TIMEOUT ||
        fixture.plans != 1)
        return result(TestResult::verification_failed);

    fixture.advance_scene_on_plan = true;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK)
        return result(TestResult::verification_failed);
    const auto* verified = reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data());
    if (verified->result != SACCADE_AGENT_OK || verified->completed_action_count != 1 || fixture.plans != 2 ||
        verified->next_generation.generation != 43 ||
        (verified->header.flags & SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE) == 0)
        return result(TestResult::verification_failed);
    fixture.advance_scene_on_plan = false;
    set_scene_generation(&fixture, 42, 43);

    batch->header.flags = SACCADE_AGENT_BATCH_DRY_RUN;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.plans != 3 || fixture.last_window_id != fixture.state.window_id ||
        fixture.last_display_id != fixture.state.display_id)
        return result(TestResult::action_failed);

    action[0].kind = SACCADE_AGENT_ACTION_INVOKE;
    action[0].flags = SACCADE_AGENT_ACTION_EXPLICIT_POINTS;
    action[0].point = {150, 250};
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.plans != 4 || fixture.last_x_q8 != 150 || fixture.last_y_q8 != 250)
        return result(TestResult::action_failed);

    action[0].point.x_q8 = 99;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_TARGET_NOT_FOUND ||
        fixture.plans != 4)
        return result(TestResult::action_failed);

    batch->header.flags = 0;
    batch->action_count = 2;
    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_HOLD;
    action[0].target_id = 11;
    action[0].button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action[0].repeat_count = 1;
    action[1] = {};
    action[1].kind = SACCADE_AGENT_ACTION_RELEASE;
    action[1].button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action[1].repeat_count = 1;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->completed_action_count != 2 ||
        fixture.plans != 6 || fixture.state.expected_buttons != 0)
        return result(TestResult::action_failed);

    batch->action_count = 1;
    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_HOLD;
    action[0].target_id = 11;
    action[0].button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action[0].duration_ns = 1;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED ||
        fixture.plans != 6)
        return result(TestResult::action_failed);

    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_ABORT;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_POINTER;
    if (service.process({action_bytes.data(), action_bytes.size()}, SACCADE_AGENT_CAPABILITY_POINTER, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.aborts != 1)
        return result(TestResult::action_failed);

    action[0].kind = SACCADE_AGENT_ACTION_WINDOW_CYCLE;
    action[0].flags = SACCADE_AGENT_ACTION_CYCLE_BACKWARD;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_WINDOW;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.window_cycles != -1)
        return result(TestResult::action_failed);

    // A dry run validates abort and cycle but must not run their side
    // effects. The counters must stay where the live calls above left them.
    batch->header.flags = SACCADE_AGENT_BATCH_DRY_RUN;
    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_ABORT;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_POINTER;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.aborts != 1)
        return result(TestResult::action_failed);

    action[0].kind = SACCADE_AGENT_ACTION_WINDOW_CYCLE;
    action[0].flags = SACCADE_AGENT_ACTION_CYCLE_BACKWARD;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_WINDOW;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result != SACCADE_AGENT_OK ||
        fixture.window_cycles != -1)
        return result(TestResult::action_failed);
    batch->header.flags = 0;

    fixture.physical.pointer = {123, 456};
    fixture.physical.physical_sequence = 17;
    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK) {
        return result(TestResult::action_failed);
    }
    const auto* physical_completion = reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data());
    if (physical_completion->result != SACCADE_AGENT_OK || physical_completion->physical_state.pointer.x_q8 != 123 ||
        physical_completion->physical_state.physical_sequence != 17)
        return result(TestResult::action_failed);

    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_CLIPBOARD;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_CAPABILITY_DENIED)
        return result(TestResult::capability_failed);

    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_TEXT;
    action[0].target_id = 11;
    action[0].payload_size = 1;
    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_KEYBOARD;
    if (service.process({action_bytes.data(), action_bytes.size()}, SACCADE_AGENT_CAPABILITY_KEYBOARD, 1,
                        {output.data(), output.size()}, &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_CAPABILITY_DENIED)
        return result(TestResult::text_capability_failed);

    batch->requested_capability_bits = SACCADE_AGENT_CAPABILITY_POINTER;
    action[0] = {};
    action[0].kind = SACCADE_AGENT_ACTION_CLICK;
    action[0].target_id = 11;
    action[0].button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action[0].repeat_count = 1;
    batch->preconditions.window_id = 6;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_STALE_GENERATION)
        return result(TestResult::stale_failed);
    batch->preconditions.window_id = 7;
    batch->preconditions.display_id = 9;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_STALE_GENERATION)
        return result(TestResult::stale_failed);
    batch->preconditions.display_id = 8;
    batch->preconditions.generation = 41;
    if (service.process({action_bytes.data(), action_bytes.size()}, capabilities, 1, {output.data(), output.size()},
                        &output_size) != SACCADE_OK ||
        reinterpret_cast<const SaccadeAgentActionCompletion*>(output.data())->result !=
            SACCADE_AGENT_ERROR_STALE_GENERATION)
        return result(TestResult::stale_failed);

    return service.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

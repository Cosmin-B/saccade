#include "platform/macos/input_executor.hpp"

#include <Carbon/Carbon.h>
#include <IOKit/hid/IOHIDUsageTables.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    initialization_failed,
    click_failed,
    dry_run_failed,
    release_failed,
    keyboard_failed,
    text_failed,
    scroll_failed,
    timed_move_failed,
    timed_hold_failed,
    stale_epoch_failed,
    activation_failed,
    transient_release_failed,
    permission_loss_failed,
    shutdown_failed
};

constexpr uint64_t topology_epoch = 7;
constexpr uint64_t permission_epoch = 8;
constexpr uint64_t execution_time_ns = 1;
constexpr uint64_t plan_deadline_ns = UINT64_C(1'000'000'000);
constexpr uint64_t timed_duration_ns = UINT64_C(20'000'000);
constexpr uint64_t activated_window_id = 42;
constexpr uint64_t scene_epoch = 1;
constexpr uint64_t frame_id = 2;
constexpr uint64_t model_epoch = 3;
constexpr uint64_t session_epoch = 4;
constexpr uint64_t transform_epoch = 5;
constexpr uint64_t source_id = 9;
constexpr uint64_t focus_id = 10;
constexpr uint64_t display_id = 12;
constexpr uint32_t capture_capacity = 256;
constexpr uint32_t payload_capacity = 256;
constexpr uint32_t maximum_test_commands = 8;
constexpr int64_t scroll_fixed_point_scale = INT64_C(1) << 8U;
constexpr size_t plan_capacity =
    sizeof(SaccadeInputPlanHeader) + maximum_test_commands * sizeof(SaccadeInputCommand) + payload_capacity;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct alignas(SaccadeInputPlanHeader) PlanStorage {
    std::array<uint8_t, plan_capacity> bytes{};
};

struct CapturedEvent {
    CGEventType type = kCGEventNull;
    CGPoint location{};
    CGEventFlags flags = 0;
    int64_t keycode = 0;
    int64_t mouse_button = 0;
    int64_t click_state = 0;
    int64_t scroll_x = 0;
    int64_t scroll_y = 0;
    std::array<UniChar, 2> unicode{};
    UniCharCount unicode_count = 0;
};

struct CaptureSink {
    std::array<CapturedEvent, capture_capacity> events{};
    uint32_t count = 0;
    uint32_t calls = 0;
    uint32_t failure_call = 0;
    uint64_t activated_window = 0;
};

bool capture_event(void* context, CGEventRef event) noexcept {
    auto* sink = static_cast<CaptureSink*>(context);
    ++sink->calls;
    if (sink->calls == sink->failure_call) return false;
    if (sink->count == sink->events.size()) return false;
    CapturedEvent& captured = sink->events[sink->count++];
    captured.type = CGEventGetType(event);
    captured.location = CGEventGetLocation(event);
    captured.flags = CGEventGetFlags(event);
    captured.keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    captured.mouse_button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
    captured.click_state = CGEventGetIntegerValueField(event, kCGMouseEventClickState);
    captured.scroll_x = CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2);
    captured.scroll_y = CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1);
    CGEventKeyboardGetUnicodeString(event, static_cast<UniCharCount>(captured.unicode.size()), &captured.unicode_count,
                                    captured.unicode.data());
    return true;
}

SaccadeResult capture_activation(void* context, uint64_t window_id) noexcept {
    static_cast<CaptureSink*>(context)->activated_window = window_id;
    return SACCADE_OK;
}

SaccadeSpanU8 make_plan(PlanStorage* storage, uint64_t plan_id, uint32_t permissions, uint32_t expected_buttons,
                        uint32_t flags, const SaccadeInputCommand* commands, uint32_t command_count,
                        const uint8_t* payload = nullptr, uint32_t payload_size = 0) noexcept {
    *storage = {};
    SaccadeInputPlanHeader header{};
    header.struct_size = sizeof(header);
    header.plan_version = SACCADE_INPUT_PLAN_VERSION;
    header.command_count = command_count;
    header.command_stride = sizeof(SaccadeInputCommand);
    header.flags = flags;
    header.required_permissions = permissions;
    header.expected_buttons = expected_buttons;
    header.plan_id = plan_id;
    header.scene_epoch = scene_epoch;
    header.frame_id = frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.permission_epoch = permission_epoch;
    header.source_id = source_id;
    header.focus_id = focus_id;
    header.window_id = activated_window_id;
    header.display_id = display_id;
    header.deadline_ns = plan_deadline_ns;
    header.commands_offset = sizeof(header);
    header.total_size =
        sizeof(header) + static_cast<uint64_t>(command_count) * sizeof(SaccadeInputCommand) + payload_size;
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    std::memcpy(storage->bytes.data() + sizeof(header), commands,
                static_cast<size_t>(command_count) * sizeof(SaccadeInputCommand));
    if (payload_size != 0) {
        std::memcpy(storage->bytes.data() + header.total_size - payload_size, payload, payload_size);
    }
    return {storage->bytes.data(), static_cast<size_t>(header.total_size)};
}

} // namespace

int main() {
    static PlanStorage storage;
    CaptureSink sink{};
    saccade::platform::macos::InputExecutor executor;
    const saccade::platform::macos::Desktop desktop{-1920, 0, 3840, 1080, topology_epoch};
    const saccade::platform::macos::InputSink input_sink{&sink, capture_event, capture_activation};
    if (executor.initialize(desktop, input_sink, permission_epoch, 0, 0) != SACCADE_OK)
        return result(TestResult::initialization_failed);

    SaccadeInputCommand click{};
    click.kind = SACCADE_INPUT_COMMAND_CLICK;
    click.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    click.target_id = 1;
    click.y_q8 = 256;
    click.data0 = SACCADE_INPUT_BUTTON_RIGHT;
    click.data1 = 2;
    click.data2 = SACCADE_INPUT_MODIFIER_CONTROL;
    SaccadeSpanU8 plan =
        make_plan(&storage, 1, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &click, 1);
    saccade::platform::macos::InputExecutionResult execution{};
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        execution.native_events != 7 || sink.count != 7 || sink.events[0].type != kCGEventMouseMoved ||
        sink.events[1].type != kCGEventFlagsChanged || sink.events[1].keycode != kVK_Control ||
        (sink.events[1].flags & kCGEventFlagMaskControl) == 0 || sink.events[2].type != kCGEventRightMouseDown ||
        sink.events[2].click_state != 1 || sink.events[4].click_state != 2 ||
        sink.events[5].type != kCGEventRightMouseUp || sink.events[6].type != kCGEventFlagsChanged ||
        sink.events[6].flags != 0) {
        return result(TestResult::click_failed);
    }

    const uint32_t before_dry_run = sink.count;
    plan = make_plan(&storage, 2, SACCADE_INPUT_PERMISSION_POINTER, 0,
                     SACCADE_INPUT_PLAN_DRY_RUN | SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &click, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        sink.count != before_dry_run || execution.native_events != 0)
        return result(TestResult::dry_run_failed);

    std::array<SaccadeInputCommand, 2> hold{};
    hold[0].kind = SACCADE_INPUT_COMMAND_POINTER_MOVE;
    hold[0].flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    hold[0].target_id = 1;
    hold[0].x_q8 = 256;
    hold[0].y_q8 = 256;
    hold[1] = hold[0];
    hold[1].kind = SACCADE_INPUT_COMMAND_BUTTON_DOWN;
    hold[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    plan = make_plan(&storage, 3, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, hold.data(),
                     static_cast<uint32_t>(hold.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        execution.buttons != SACCADE_INPUT_BUTTON_LEFT ||
        !saccade::platform::macos::synthetic_input_active(&executor) ||
        saccade::platform::macos::neutralize_synthetic_input(&executor) != SACCADE_OK ||
        executor.physical_state().state().buttons != 0 || sink.events[sink.count - 1U].type != kCGEventLeftMouseUp)
        return result(TestResult::release_failed);

    std::array<SaccadeInputCommand, 2> keys{};
    keys[0].kind = SACCADE_INPUT_COMMAND_KEY_DOWN;
    keys[0].flags = SACCADE_INPUT_COMMAND_PHYSICAL_KEY;
    keys[0].data0 = kHIDUsage_KeyboardA;
    keys[0].data1 = SACCADE_INPUT_MODIFIER_SHIFT;
    keys[1] = keys[0];
    keys[1].kind = SACCADE_INPUT_COMMAND_KEY_UP;
    const uint32_t key_start = sink.count;
    plan = make_plan(&storage, 4, SACCADE_INPUT_PERMISSION_KEYBOARD, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, keys.data(),
                     static_cast<uint32_t>(keys.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_KEYBOARD, execution_time_ns, &execution) != SACCADE_OK ||
        execution.native_events != 6 || sink.events[key_start].type != kCGEventFlagsChanged ||
        sink.events[key_start + 1U].type != kCGEventKeyDown || sink.events[key_start + 1U].keycode != kVK_ANSI_A ||
        sink.events[key_start + 4U].type != kCGEventKeyUp)
        return result(TestResult::keyboard_failed);

    constexpr std::array<uint8_t, 5> text_payload{
        {static_cast<uint8_t>('A'), UINT8_C(0xf0), UINT8_C(0x9f), UINT8_C(0x98), UINT8_C(0x80)}};
    SaccadeInputCommand text{};
    text.kind = SACCADE_INPUT_COMMAND_TEXT;
    text.target_id = 1;
    text.payload_offset = sizeof(SaccadeInputPlanHeader) + sizeof(SaccadeInputCommand);
    text.payload_size = static_cast<uint32_t>(text_payload.size());
    const uint32_t text_start = sink.count;
    plan = make_plan(&storage, 5, SACCADE_INPUT_PERMISSION_TEXT, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &text, 1,
                     text_payload.data(), static_cast<uint32_t>(text_payload.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_TEXT, execution_time_ns, &execution) != SACCADE_OK ||
        execution.native_events != 4 || sink.events[text_start].unicode_count != 1 ||
        sink.events[text_start].unicode[0] != static_cast<UniChar>('A') ||
        sink.events[text_start + 2U].unicode_count != 2)
        return result(TestResult::text_failed);

    SaccadeInputCommand scroll{};
    scroll.kind = SACCADE_INPUT_COMMAND_SCROLL;
    scroll.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    scroll.target_id = 1;
    scroll.x_q8 = 512;
    scroll.y_q8 = 512;
    scroll.delta_x_q8 = 256;
    scroll.delta_y_q8 = -512;
    const uint32_t scroll_start = sink.count;
    plan = make_plan(&storage, 6, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &scroll, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        execution.native_events != 2 || sink.events[scroll_start + 1U].type != kCGEventScrollWheel ||
        sink.events[scroll_start + 1U].scroll_x != INT64_C(256) * scroll_fixed_point_scale ||
        sink.events[scroll_start + 1U].scroll_y != -INT64_C(512) * scroll_fixed_point_scale)
        return result(TestResult::scroll_failed);

    SaccadeInputCommand timed_move{};
    timed_move.kind = SACCADE_INPUT_COMMAND_POINTER_MOVE;
    timed_move.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    timed_move.target_id = 1;
    timed_move.x_q8 = 20 * 256;
    timed_move.y_q8 = 10 * 256;
    timed_move.duration_ns = timed_duration_ns;
    const uint32_t timed_move_start = sink.count;
    plan = make_plan(&storage, 60, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &timed_move,
                     1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        (execution.flags & saccade::platform::macos::input_execution_pending) == 0 || sink.count != timed_move_start)
        return result(TestResult::timed_move_failed);
    reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data())->deadline_ns = 2;
    if (executor.advance(UINT64_C(10'000'001), &execution) != SACCADE_OK || sink.count != timed_move_start + 1U ||
        sink.events[timed_move_start].location.x < 9.0 || sink.events[timed_move_start].location.x > 11.0 ||
        executor.advance(UINT64_C(21'000'001), &execution) != SACCADE_OK || execution.flags != 0 ||
        sink.count != timed_move_start + 2U || sink.events[timed_move_start + 1U].location.x != 20.0)
        return result(TestResult::timed_move_failed);

    SaccadeInputCommand timed_hold{};
    timed_hold.kind = SACCADE_INPUT_COMMAND_BUTTON_DOWN;
    timed_hold.flags = SACCADE_INPUT_COMMAND_ABSOLUTE | SACCADE_INPUT_COMMAND_CONTINUOUS;
    timed_hold.target_id = 1;
    timed_hold.x_q8 = 20 * 256;
    timed_hold.y_q8 = 10 * 256;
    timed_hold.data0 = SACCADE_INPUT_BUTTON_LEFT;
    timed_hold.duration_ns = timed_duration_ns;
    plan = make_plan(&storage, 61, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &timed_hold,
                     1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        execution.buttons != SACCADE_INPUT_BUTTON_LEFT ||
        executor.advance(UINT64_C(21'000'001), &execution) != SACCADE_OK || executor.synthetic_input_active() ||
        sink.events[sink.count - 1U].type != kCGEventLeftMouseUp)
        return result(TestResult::timed_hold_failed);

    auto* header = reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data());
    header->topology_epoch = topology_epoch + 1U;
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) !=
        SACCADE_ERROR_STALE_HANDLE)
        return result(TestResult::stale_epoch_failed);

    SaccadeInputCommand activate{};
    activate.kind = SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE;
    activate.target_id = 1;
    plan = make_plan(&storage, 7, SACCADE_INPUT_PERMISSION_WINDOW, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &activate, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_WINDOW, execution_time_ns, &execution) != SACCADE_OK ||
        sink.activated_window != activated_window_id)
        return result(TestResult::activation_failed);

    SaccadeInputCommand failing_click = click;
    failing_click.data0 = SACCADE_INPUT_BUTTON_LEFT;
    failing_click.data1 = 1;
    failing_click.data2 = 0;
    plan = make_plan(&storage, 8, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE,
                     &failing_click, 1);
    sink.failure_call = sink.calls + 3U;
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) !=
            SACCADE_ERROR_BACKEND ||
        executor.physical_state().state().buttons != 0 || sink.events[sink.count - 1U].type != kCGEventLeftMouseUp)
        return result(TestResult::transient_release_failed);

    plan = make_plan(&storage, 62, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, hold.data(),
                     static_cast<uint32_t>(hold.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) != SACCADE_OK ||
        !executor.synthetic_input_active() || executor.permission_lost(permission_epoch + 1U) != SACCADE_OK ||
        executor.synthetic_input_active() || sink.events[sink.count - 1U].type != kCGEventLeftMouseUp ||
        executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution) !=
            SACCADE_ERROR_STALE_HANDLE)
        return result(TestResult::permission_loss_failed);

    return executor.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

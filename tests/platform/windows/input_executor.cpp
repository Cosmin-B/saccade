#include "platform/windows/input_executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr size_t plan_capacity = sizeof(SaccadeInputPlanHeader) + 8U * sizeof(SaccadeInputCommand) + 256U;
constexpr uint64_t timed_duration_ns = UINT64_C(20'000'000);
constexpr int timed_move_failed = 10;
constexpr int timed_hold_failed = 11;
constexpr int permission_loss_failed = 12;
constexpr int partial_modifier_failed = 13;
constexpr int partial_button_failed = 14;
constexpr int pending_release_retry_failed = 15;
constexpr int shutdown_release_retry_failed = 16;

struct alignas(8) PlanStorage {
    std::array<uint8_t, plan_capacity> bytes{};
};

struct CaptureSink {
    std::array<INPUT, 4096> events{};
    uint32_t count = 0;
    uint32_t calls = 0;
    uint32_t fail_first_call = 0;
    uint32_t fail_last_call = 0;
    uint64_t activated_window = 0;
};

void fail_next(CaptureSink* sink, uint32_t count) noexcept {
    sink->fail_first_call = sink->calls + 1U;
    sink->fail_last_call = sink->calls + count;
}

uint32_t capture_input(void* context, const INPUT* events, uint32_t count) noexcept {
    auto* sink = static_cast<CaptureSink*>(context);
    ++sink->calls;
    if (sink->calls >= sink->fail_first_call && sink->calls <= sink->fail_last_call) {
        const uint32_t submitted = count == 0 ? 0 : count - 1U;
        if (submitted > sink->events.size() - sink->count) return 0;
        std::memcpy(sink->events.data() + sink->count, events, static_cast<size_t>(submitted) * sizeof(INPUT));
        sink->count += submitted;
        return submitted;
    }
    if (count > sink->events.size() - sink->count) {
        return 0;
    }
    std::memcpy(sink->events.data() + sink->count, events, static_cast<size_t>(count) * sizeof(INPUT));
    sink->count += count;
    return count;
}

SaccadeResult activate_window(void* context, uint64_t window_id) noexcept {
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
    header.scene_epoch = 1;
    header.frame_id = 2;
    header.model_epoch = 3;
    header.session_epoch = 4;
    header.transform_epoch = 5;
    header.topology_epoch = 7;
    header.permission_epoch = 8;
    header.source_id = 9;
    header.focus_id = 10;
    header.window_id = 11;
    header.display_id = 12;
    header.deadline_ns = UINT64_C(1'000'000'000);
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

bool keyboard_event(const INPUT& event, uint16_t scan, DWORD flags) noexcept {
    return event.type == INPUT_KEYBOARD && event.ki.wScan == scan && event.ki.dwFlags == flags;
}

} // namespace

int main() {
    static PlanStorage storage;
    CaptureSink sink{};
    saccade::platform::windows::InputExecutor executor;
    const saccade::platform::windows::VirtualDesktop desktop{-1920, 0, 3840, 1080, 7};
    const saccade::platform::windows::InputSink input_sink{&sink, capture_input, activate_window};
    if (executor.initialize(desktop, input_sink, 8, 0, 0) != SACCADE_OK) {
        return 1;
    }

    SaccadeInputCommand click{};
    click.kind = SACCADE_INPUT_COMMAND_CLICK;
    click.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    click.target_id = 1;
    click.x_q8 = 0;
    click.y_q8 = 256;
    click.data0 = SACCADE_INPUT_BUTTON_RIGHT;
    click.data1 = 2;
    click.data2 = SACCADE_INPUT_MODIFIER_CONTROL;
    SaccadeSpanU8 plan =
        make_plan(&storage, 1, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &click, 1);
    saccade::platform::windows::InputExecutionResult result{};
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        result.native_events != 7 || sink.count != 7 ||
        (sink.events[0].mi.dwFlags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) !=
            (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK) ||
        sink.events[0].mi.dx < 32760 || sink.events[0].mi.dx > 32780 ||
        !keyboard_event(sink.events[1], 0x1d, KEYEVENTF_SCANCODE) ||
        sink.events[2].mi.dwFlags != MOUSEEVENTF_RIGHTDOWN || sink.events[5].mi.dwFlags != MOUSEEVENTF_RIGHTUP ||
        !keyboard_event(sink.events[6], 0x1d, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP)) {
        return 2;
    }

    fail_next(&sink, 2);
    plan = make_plan(&storage, 20, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &click, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_ERROR_BACKEND ||
        !executor.synthetic_input_active()) {
        return partial_modifier_failed;
    }
    if (executor.release_all() != SACCADE_OK || executor.synthetic_input_active() ||
        !keyboard_event(sink.events[sink.count - 1U], 0x1d, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP)) {
        return pending_release_retry_failed;
    }

    SaccadeInputCommand partial_click = click;
    partial_click.data2 = 0;
    fail_next(&sink, 1);
    plan = make_plan(&storage, 21, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE,
                     &partial_click, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_ERROR_BACKEND ||
        executor.synthetic_input_active() || sink.events[sink.count - 1U].mi.dwFlags != MOUSEEVENTF_RIGHTUP) {
        return partial_button_failed;
    }
    sink.fail_first_call = 0;
    sink.fail_last_call = 0;

    const uint32_t before_dry_run = sink.count;
    plan = make_plan(&storage, 2, SACCADE_INPUT_PERMISSION_POINTER, 0,
                     SACCADE_INPUT_PLAN_DRY_RUN | SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &click, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        sink.count != before_dry_run || result.native_events != 0) {
        return 3;
    }

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
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        result.buttons != SACCADE_INPUT_BUTTON_LEFT || !saccade::platform::windows::synthetic_input_active(&executor) ||
        saccade::platform::windows::neutralize_synthetic_input(&executor) != SACCADE_OK ||
        executor.physical_state().state().buttons != 0 ||
        sink.events[sink.count - 1U].mi.dwFlags != MOUSEEVENTF_LEFTUP) {
        return 4;
    }

    std::array<SaccadeInputCommand, 2> keys{};
    keys[0].kind = SACCADE_INPUT_COMMAND_KEY_DOWN;
    keys[0].flags = SACCADE_INPUT_COMMAND_PHYSICAL_KEY;
    keys[0].data0 = 0x04;
    keys[0].data1 = SACCADE_INPUT_MODIFIER_SHIFT;
    keys[1] = keys[0];
    keys[1].kind = SACCADE_INPUT_COMMAND_KEY_UP;
    const uint32_t key_start = sink.count;
    plan = make_plan(&storage, 4, SACCADE_INPUT_PERMISSION_KEYBOARD, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, keys.data(),
                     static_cast<uint32_t>(keys.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_KEYBOARD, 1, &result) != SACCADE_OK ||
        result.native_events != 6 || !keyboard_event(sink.events[key_start], 0x2a, KEYEVENTF_SCANCODE) ||
        !keyboard_event(sink.events[key_start + 1U], 0x1e, KEYEVENTF_SCANCODE) ||
        !keyboard_event(sink.events[key_start + 4U], 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP)) {
        return 5;
    }

    constexpr std::array<uint8_t, 5> text{'A', 0xf0, 0x9f, 0x98, 0x80};
    SaccadeInputCommand text_command{};
    text_command.kind = SACCADE_INPUT_COMMAND_TEXT;
    text_command.target_id = 1;
    text_command.payload_offset = sizeof(SaccadeInputPlanHeader) + sizeof(SaccadeInputCommand);
    text_command.payload_size = static_cast<uint32_t>(text.size());
    plan = make_plan(&storage, 5, SACCADE_INPUT_PERMISSION_TEXT, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &text_command,
                     1, text.data(), static_cast<uint32_t>(text.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_TEXT, 1, &result) != SACCADE_OK || result.native_events != 6) {
        return 6;
    }

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
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        (result.flags & saccade::platform::windows::input_execution_pending) == 0 || sink.count != timed_move_start)
        return timed_move_failed;
    reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data())->deadline_ns = 2;
    if (executor.advance(UINT64_C(10'000'001), &result) != SACCADE_OK || sink.count != timed_move_start + 1U ||
        executor.advance(UINT64_C(21'000'001), &result) != SACCADE_OK || result.flags != 0 ||
        sink.count != timed_move_start + 2U)
        return timed_move_failed;

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
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        result.buttons != SACCADE_INPUT_BUTTON_LEFT || executor.advance(UINT64_C(21'000'001), &result) != SACCADE_OK ||
        executor.synthetic_input_active() || sink.events[sink.count - 1U].mi.dwFlags != MOUSEEVENTF_LEFTUP)
        return timed_hold_failed;

    auto* header = reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data());
    header->topology_epoch = 99;
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_TEXT, 1, &result) != SACCADE_ERROR_STALE_HANDLE) {
        return 7;
    }

    SaccadeInputCommand activate{};
    activate.kind = SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE;
    activate.target_id = 1;
    plan = make_plan(&storage, 6, SACCADE_INPUT_PERMISSION_WINDOW, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, &activate, 1);
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_WINDOW, 1, &result) != SACCADE_OK ||
        sink.activated_window != 11) {
        return 8;
    }

    plan = make_plan(&storage, 62, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, hold.data(),
                     static_cast<uint32_t>(hold.size()));
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        !executor.synthetic_input_active() || executor.permission_lost(9) != SACCADE_OK ||
        executor.synthetic_input_active() || sink.events[sink.count - 1U].mi.dwFlags != MOUSEEVENTF_LEFTUP ||
        executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_ERROR_STALE_HANDLE) {
        return permission_loss_failed;
    }

    plan = make_plan(&storage, 63, SACCADE_INPUT_PERMISSION_POINTER, 0, SACCADE_INPUT_PLAN_STOP_ON_FAILURE, hold.data(),
                     static_cast<uint32_t>(hold.size()));
    reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data())->permission_epoch = 9;
    if (executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &result) != SACCADE_OK ||
        !executor.synthetic_input_active())
        return shutdown_release_retry_failed;
    fail_next(&sink, 1);
    if (executor.shutdown() != SACCADE_ERROR_BACKEND || !executor.synthetic_input_active() ||
        executor.shutdown() != SACCADE_OK || sink.events[sink.count - 1U].mi.dwFlags != MOUSEEVENTF_LEFTUP)
        return shutdown_release_retry_failed;
    return 0;
}

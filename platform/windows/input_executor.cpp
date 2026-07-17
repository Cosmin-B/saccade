#include "platform/windows/input_executor.hpp"

#include "input/injected_marker.hpp"
#include "input/plan.hpp"
#include "input/utf8.hpp"
#include "platform/windows/keyboard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace saccade::platform::windows {
namespace {

constexpr uint32_t native_batch_capacity = 256;
constexpr uint32_t coordinate_fraction_bits = 8;
constexpr int64_t coordinate_scale = INT64_C(1) << coordinate_fraction_bits;
constexpr ULONG_PTR injected_marker = static_cast<ULONG_PTR>(input::injected_event_marker);
constexpr uint64_t timed_tick_ns = UINT64_C(8'333'333);

using ScanCode = KeyScan;

struct NativeEmitter {
    InputSink sink{};
    std::array<INPUT, native_batch_capacity> events{};
    std::array<INPUT, native_batch_capacity> held{};
    uint32_t count = 0;
    uint32_t held_count = 0;
    uint32_t total = 0;
    uint64_t submit_calls = 0;
    bool failed = false;

    static bool keyboard_release(const INPUT& down, const INPUT& event) noexcept {
        constexpr DWORD identity_flags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_SCANCODE | KEYEVENTF_UNICODE;
        return down.type == INPUT_KEYBOARD && event.type == INPUT_KEYBOARD &&
               (event.ki.dwFlags & KEYEVENTF_KEYUP) != 0 && down.ki.wScan == event.ki.wScan &&
               (down.ki.dwFlags & identity_flags) == (event.ki.dwFlags & identity_flags);
    }

    static DWORD mouse_up_for(DWORD flags) noexcept {
        if ((flags & MOUSEEVENTF_LEFTDOWN) != 0) return MOUSEEVENTF_LEFTUP;
        if ((flags & MOUSEEVENTF_RIGHTDOWN) != 0) return MOUSEEVENTF_RIGHTUP;
        if ((flags & MOUSEEVENTF_MIDDLEDOWN) != 0) return MOUSEEVENTF_MIDDLEUP;
        return 0;
    }

    static bool mouse_release(const INPUT& down, const INPUT& event) noexcept {
        const DWORD up = down.type == INPUT_MOUSE ? mouse_up_for(down.mi.dwFlags) : 0;
        return up != 0 && event.type == INPUT_MOUSE && (event.mi.dwFlags & up) != 0;
    }

    static bool down_event(const INPUT& event) noexcept {
        return (event.type == INPUT_KEYBOARD && (event.ki.dwFlags & KEYEVENTF_KEYUP) == 0) ||
               (event.type == INPUT_MOUSE && mouse_up_for(event.mi.dwFlags) != 0);
    }

    static INPUT release_event(const INPUT& down) noexcept {
        INPUT release = down;
        if (down.type == INPUT_KEYBOARD) {
            release.ki.dwFlags |= KEYEVENTF_KEYUP;
        } else {
            release.mi = {};
            release.mi.dwFlags = mouse_up_for(down.mi.dwFlags);
            release.mi.dwExtraInfo = injected_marker;
        }
        return release;
    }

    void observe(const INPUT& event) noexcept {
        if (event.type == INPUT_KEYBOARD && (event.ki.dwFlags & KEYEVENTF_KEYUP) != 0) {
            for (uint32_t index = held_count; index != 0; --index) {
                if (!keyboard_release(held[index - 1U], event)) continue;
                held[index - 1U] = held[--held_count];
                return;
            }
            return;
        }
        if (event.type == INPUT_MOUSE &&
            (event.mi.dwFlags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP)) != 0) {
            for (uint32_t index = held_count; index != 0; --index) {
                if (!mouse_release(held[index - 1U], event)) continue;
                held[index - 1U] = held[--held_count];
                return;
            }
            return;
        }
        if (down_event(event)) {
            if (held_count == held.size()) {
                failed = true;
                return;
            }
            held[held_count++] = event;
        }
    }

    bool flush() noexcept {
        if (failed) return false;
        if (count == 0) return true;
        ++submit_calls;
        const uint32_t submitted = sink.submit(sink.context, events.data(), count);
        for (uint32_t index = 0; index < submitted; ++index)
            observe(events[index]);
        total += submitted;
        failed = failed || submitted != count;
        count = 0;
        return !failed;
    }

    bool append(const INPUT& event) noexcept {
        if (count == native_batch_capacity && !flush()) {
            return false;
        }
        events[count++] = event;
        return true;
    }

    void commit() noexcept { held_count = 0; }
};

bool desktop_valid(const VirtualDesktop& desktop) noexcept {
    return desktop.width > 1 && desktop.height > 1 && desktop.topology_epoch != 0 &&
           static_cast<uint64_t>(desktop.width) <= static_cast<uint64_t>(INT32_MAX) &&
           static_cast<uint64_t>(desktop.height) <= static_cast<uint64_t>(INT32_MAX);
}

bool point_in_desktop(const VirtualDesktop& desktop, int32_t x_q8, int32_t y_q8) noexcept {
    const int64_t left = static_cast<int64_t>(desktop.x) * coordinate_scale;
    const int64_t top = static_cast<int64_t>(desktop.y) * coordinate_scale;
    const int64_t right = (static_cast<int64_t>(desktop.x) + desktop.width) * coordinate_scale;
    const int64_t bottom = (static_cast<int64_t>(desktop.y) + desktop.height) * coordinate_scale;
    return x_q8 >= left && x_q8 < right && y_q8 >= top && y_q8 < bottom;
}

LONG normalized_axis(int32_t value_q8, int32_t origin, uint32_t extent) noexcept {
    const int64_t relative = static_cast<int64_t>(value_q8) - static_cast<int64_t>(origin) * coordinate_scale;
    const int64_t denominator = static_cast<int64_t>(extent - 1U) * coordinate_scale;
    const int64_t result = (relative * UINT16_MAX + denominator / 2) / denominator;
    return static_cast<LONG>(result > UINT16_MAX ? UINT16_MAX : result);
}

INPUT mouse_event(DWORD flags, DWORD data = 0, LONG x = 0, LONG y = 0) noexcept {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dx = x;
    event.mi.dy = y;
    event.mi.mouseData = data;
    event.mi.dwFlags = flags;
    event.mi.dwExtraInfo = injected_marker;
    return event;
}

INPUT key_event(uint16_t scan, bool extended, bool up) noexcept {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = scan;
    event.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0U) | (up ? KEYEVENTF_KEYUP : 0U);
    event.ki.dwExtraInfo = injected_marker;
    return event;
}

INPUT unicode_event(uint16_t value, bool up) noexcept {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = value;
    event.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0U);
    event.ki.dwExtraInfo = injected_marker;
    return event;
}

bool mouse_button(uint32_t button, bool up, DWORD* flags) noexcept {
    if (button == SACCADE_INPUT_BUTTON_LEFT) {
        *flags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
    } else if (button == SACCADE_INPUT_BUTTON_RIGHT) {
        *flags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
    } else if (button == SACCADE_INPUT_BUTTON_MIDDLE) {
        *flags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
    } else {
        return false;
    }
    return true;
}

bool modifier_scan(uint32_t modifier, ScanCode* output) noexcept {
    return scan_from_modifier(modifier, output);
}

bool emit_modifiers(NativeEmitter* emitter, uint32_t modifiers, bool up) noexcept {
    constexpr std::array<uint32_t, 4> order{SACCADE_INPUT_MODIFIER_SHIFT, SACCADE_INPUT_MODIFIER_CONTROL,
                                            SACCADE_INPUT_MODIFIER_ALT, SACCADE_INPUT_MODIFIER_META};
    for (uint32_t offset = 0; offset < order.size(); ++offset) {
        const uint32_t index = up ? static_cast<uint32_t>(order.size()) - 1U - offset : offset;
        if ((modifiers & order[index]) == 0) {
            continue;
        }
        ScanCode scan{};
        (void)modifier_scan(order[index], &scan);
        if (!emitter->append(key_event(scan.value, scan.extended, up))) {
            return false;
        }
    }
    return true;
}

bool physical_scan(uint32_t usage, ScanCode* output) noexcept {
    return scan_from_hid_usage(usage, output);
}

bool emit_move(NativeEmitter* emitter, const VirtualDesktop& desktop, int32_t x_q8, int32_t y_q8) noexcept {
    if (!point_in_desktop(desktop, x_q8, y_q8)) {
        return false;
    }
    return emitter->append(mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, 0,
                                       normalized_axis(x_q8, desktop.x, desktop.width),
                                       normalized_axis(y_q8, desktop.y, desktop.height)));
}

bool emit_text(NativeEmitter* emitter, const uint8_t* data, size_t size) noexcept {
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    while (cursor != end) {
        uint32_t codepoint = 0;
        if (!input::decode_utf8(&cursor, end, &codepoint)) {
            return false;
        }
        if (codepoint <= UINT16_MAX) {
            const uint16_t unit = static_cast<uint16_t>(codepoint);
            if (!emitter->append(unicode_event(unit, false)) || !emitter->append(unicode_event(unit, true))) {
                return false;
            }
        } else {
            codepoint -= 0x10000U;
            const uint16_t high = static_cast<uint16_t>(0xd800U + (codepoint >> 10U));
            const uint16_t low = static_cast<uint16_t>(0xdc00U + (codepoint & 0x3ffU));
            if (!emitter->append(unicode_event(high, false)) || !emitter->append(unicode_event(high, true)) ||
                !emitter->append(unicode_event(low, false)) || !emitter->append(unicode_event(low, true))) {
                return false;
            }
        }
    }
    return true;
}

bool command_supported(const SaccadeInputCommand& command, const VirtualDesktop& desktop,
                       const SaccadeSpanU8& bytes) noexcept {
    if ((command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 &&
        !point_in_desktop(desktop, command.x_q8, command.y_q8)) {
        return false;
    }
    if (command.kind == SACCADE_INPUT_COMMAND_KEY_DOWN || command.kind == SACCADE_INPUT_COMMAND_KEY_UP) {
        ScanCode scan{};
        return physical_scan(command.data0, &scan);
    }
    if (command.kind == SACCADE_INPUT_COMMAND_TEXT) {
        const uint8_t* cursor = bytes.data + command.payload_offset;
        const uint8_t* end = cursor + command.payload_size;
        while (cursor != end) {
            uint32_t codepoint = 0;
            if (!input::decode_utf8(&cursor, end, &codepoint)) {
                return false;
            }
        }
    }
    return true;
}

bool emit_command(NativeEmitter* emitter, const VirtualDesktop& desktop, const SaccadeSpanU8& bytes,
                  const SaccadeInputPlanHeader& header, const SaccadeInputCommand& command) noexcept {
    switch (command.kind) {
    case SACCADE_INPUT_COMMAND_POINTER_MOVE:
        return emit_move(emitter, desktop, command.x_q8, command.y_q8);
    case SACCADE_INPUT_COMMAND_BUTTON_DOWN:
    case SACCADE_INPUT_COMMAND_BUTTON_UP: {
        if ((command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 &&
            !emit_move(emitter, desktop, command.x_q8, command.y_q8)) {
            return false;
        }
        DWORD flags = 0;
        (void)mouse_button(command.data0, command.kind == SACCADE_INPUT_COMMAND_BUTTON_UP, &flags);
        return emitter->append(mouse_event(flags));
    }
    case SACCADE_INPUT_COMMAND_CLICK: {
        if (!emit_move(emitter, desktop, command.x_q8, command.y_q8) ||
            !emit_modifiers(emitter, command.data2, false)) {
            return false;
        }
        DWORD down = 0;
        DWORD up = 0;
        (void)mouse_button(command.data0, false, &down);
        (void)mouse_button(command.data0, true, &up);
        for (uint32_t index = 0; index < command.data1; ++index) {
            if (!emitter->append(mouse_event(down)) || !emitter->append(mouse_event(up))) {
                return false;
            }
        }
        return emit_modifiers(emitter, command.data2, true);
    }
    case SACCADE_INPUT_COMMAND_SCROLL: {
        if (!emit_move(emitter, desktop, command.x_q8, command.y_q8)) {
            return false;
        }
        const int64_t vertical = static_cast<int64_t>(command.delta_y_q8) * WHEEL_DELTA / 256;
        const int64_t horizontal = static_cast<int64_t>(command.delta_x_q8) * WHEEL_DELTA / 256;
        if (vertical != 0 &&
            !emitter->append(mouse_event(MOUSEEVENTF_WHEEL, static_cast<DWORD>(static_cast<int32_t>(vertical))))) {
            return false;
        }
        return horizontal == 0 ||
               emitter->append(mouse_event(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(static_cast<int32_t>(horizontal))));
    }
    case SACCADE_INPUT_COMMAND_KEY_DOWN:
    case SACCADE_INPUT_COMMAND_KEY_UP: {
        ScanCode scan{};
        (void)physical_scan(command.data0, &scan);
        const bool up = command.kind == SACCADE_INPUT_COMMAND_KEY_UP;
        return emit_modifiers(emitter, command.data1, false) &&
               emitter->append(key_event(scan.value, scan.extended, up)) &&
               emit_modifiers(emitter, command.data1, true);
    }
    case SACCADE_INPUT_COMMAND_TEXT:
        return emit_text(emitter, bytes.data + command.payload_offset, command.payload_size);
    case SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE:
        return emitter->flush() && emitter->sink.activate_window != nullptr &&
               emitter->sink.activate_window(emitter->sink.context, header.window_id) == SACCADE_OK;
    default:
        return false;
    }
}

} // namespace

uint32_t submit_with_send_input(void*, const INPUT* events, uint32_t count) noexcept {
    return SendInput(count, const_cast<INPUT*>(events), sizeof(INPUT));
}

SaccadeResult InputExecutor::initialize(const VirtualDesktop& desktop, const InputSink& sink, uint64_t permission_epoch,
                                        int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept {
    if (initialized_ || !desktop_valid(desktop) || sink.submit == nullptr ||
        !point_in_desktop(desktop, pointer_x_q8, pointer_y_q8)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult result = physical_.initialize(permission_epoch, pointer_x_q8, pointer_y_q8);
    if (result != SACCADE_OK) {
        return result;
    }
    desktop_ = desktop;
    sink_ = sink;
    pending_release_count_ = 0;
    shutdown_pending_ = false;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::execute(SaccadeSpanU8 bytes, uint32_t available_permissions, uint64_t now_ns,
                                     InputExecutionResult* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    if (shutdown_pending_) return SACCADE_ERROR_STATE;
    if (pending_release_count_ != 0) {
        const SaccadeResult recovered = retry_pending_releases();
        if (recovered != SACCADE_OK) return recovered;
    }
    input::PlanView plan{};
    SaccadeResult result = input::validate_plan(bytes, &plan);
    if (result != SACCADE_OK) {
        return result;
    }
    if (plan.header->topology_epoch != desktop_.topology_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        if (!command_supported(plan.commands[index], desktop_, bytes) ||
            (plan.commands[index].kind == SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE && sink_.activate_window == nullptr)) {
            return SACCADE_ERROR_UNSUPPORTED;
        }
    }
    if ((plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) != 0 && sink_.preflight != nullptr) {
        result = sink_.preflight(sink_.context, plan, 0, now_ns);
        if (result != SACCADE_OK) return result;
    }
    if ((plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) != 0) {
        result = physical_.begin(plan, available_permissions, now_ns);
        if (result != SACCADE_OK) return result;
        ++stats_.plans;
        ++stats_.dry_runs;
        (void)physical_.advance(plan.header->command_count);
        output->plan_id = plan.header->plan_id;
        output->commands_completed = plan.header->command_count;
        return SACCADE_OK;
    }

    if (active_plan_.header != nullptr) return SACCADE_ERROR_BUSY;
    if (plan.byte_size > active_storage_.bytes.size()) return SACCADE_ERROR_CAPACITY;
    std::memcpy(active_storage_.bytes.data(), bytes.data, plan.byte_size);
    active_bytes_ = {active_storage_.bytes.data(), plan.byte_size};
    result = input::validate_plan(active_bytes_, &active_plan_);
    if (result != SACCADE_OK) {
        active_bytes_ = {};
        return result;
    }
    result = physical_.begin(active_plan_, available_permissions, now_ns);
    if (result != SACCADE_OK) {
        active_plan_ = {};
        active_bytes_ = {};
        return result;
    }
    ++stats_.plans;
    next_command_ = 0;
    return continue_execution(now_ns, output);
}

SaccadeResult InputExecutor::continue_execution(uint64_t now_ns, InputExecutionResult* output) noexcept {
    *output = {};
    if (active_plan_.header == nullptr) return SACCADE_ERROR_NOT_FOUND;
    output->plan_id = active_plan_.header->plan_id;
    NativeEmitter emitter{};
    emitter.sink = sink_;
    auto fail = [&](SaccadeResult failure) noexcept {
        stats_.native_events += emitter.total;
        stats_.submit_calls += emitter.submit_calls;
        SaccadeResult recovery = SACCADE_OK;
        if (emitter.held_count != 0) {
            ++stats_.releases;
            for (uint32_t offset = 0; offset < emitter.held_count; ++offset) {
                const INPUT release = NativeEmitter::release_event(emitter.held[emitter.held_count - offset - 1U]);
                const SaccadeResult queued = append_pending_release(release);
                if (queued != SACCADE_OK && recovery == SACCADE_OK) recovery = queued;
            }
            emitter.held_count = 0;
        }
        input::SyntheticRelease release{};
        const SaccadeResult reduced = physical_.backend_failure(&release);
        if (reduced == SACCADE_OK) {
            const SaccadeResult queued = queue_release(release);
            if (queued != SACCADE_OK && recovery == SACCADE_OK) recovery = queued;
        } else if (recovery == SACCADE_OK) {
            recovery = reduced;
        }
        const SaccadeResult retried = retry_pending_releases();
        if (retried != SACCADE_OK && recovery == SACCADE_OK) recovery = retried;
        active_plan_ = {};
        active_bytes_ = {};
        next_command_ = 0;
        timed_kind_ = TimedKind::none;
        ++stats_.failures;
        return recovery == SACCADE_OK ? failure : SACCADE_ERROR_BACKEND;
    };
    if (now_ns >= active_plan_.header->deadline_ns) {
        input::SyntheticRelease release{};
        SaccadeResult result = physical_.expire(now_ns, &release);
        if (result == SACCADE_OK) result = emit_release(release);
        active_plan_ = {};
        active_bytes_ = {};
        next_command_ = 0;
        timed_kind_ = TimedKind::none;
        return result == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : result;
    }
    for (;;) {
        if (sink_.preflight != nullptr) {
            const SaccadeResult result = sink_.preflight(sink_.context, active_plan_, next_command_, now_ns);
            if (result != SACCADE_OK) return fail(result);
        }
        if (timed_kind_ != TimedKind::none) {
            if (now_ns < next_tick_ns_) {
                output->commands_completed = next_command_;
                output->flags = input_execution_pending;
                output->buttons = physical_.state().buttons;
                return SACCADE_OK;
            }
            if (timed_kind_ == TimedKind::hold) {
                if (now_ns < timed_end_ns_) {
                    next_tick_ns_ = timed_end_ns_;
                    output->commands_completed = next_command_;
                    output->flags = input_execution_pending;
                    output->buttons = physical_.state().buttons;
                    return SACCADE_OK;
                }
                input::SyntheticRelease release{};
                SaccadeResult result = physical_.abort(&release);
                if (result == SACCADE_OK) result = emit_release(release);
                timed_kind_ = TimedKind::none;
                active_plan_ = {};
                active_bytes_ = {};
                return result;
            }
            const SaccadeInputCommand& source = active_plan_.commands[next_command_];
            const bool complete = now_ns >= timed_end_ns_;
            if (timed_kind_ == TimedKind::move) {
                SaccadeInputCommand command = source;
                const uint64_t elapsed = complete ? timed_end_ns_ - timed_start_ns_ : now_ns - timed_start_ns_;
                const uint64_t duration = timed_end_ns_ - timed_start_ns_;
                command.x_q8 =
                    static_cast<int32_t>(timed_start_x_q8_ + ((static_cast<int64_t>(source.x_q8) - timed_start_x_q8_) *
                                                              static_cast<int64_t>(elapsed)) /
                                                                 static_cast<int64_t>(duration));
                command.y_q8 =
                    static_cast<int32_t>(timed_start_y_q8_ + ((static_cast<int64_t>(source.y_q8) - timed_start_y_q8_) *
                                                              static_cast<int64_t>(elapsed)) /
                                                                 static_cast<int64_t>(duration));
                command.duration_ns = 0;
                if (!emit_command(&emitter, desktop_, active_bytes_, *active_plan_.header, command))
                    return fail(SACCADE_ERROR_BACKEND);
            } else if (timed_kind_ == TimedKind::scroll && !complete) {
                if (!emit_command(&emitter, desktop_, active_bytes_, *active_plan_.header, source))
                    return fail(SACCADE_ERROR_BACKEND);
            }
            if (!emitter.flush()) return fail(SACCADE_ERROR_BACKEND);
            stats_.native_events += emitter.total;
            stats_.submit_calls += emitter.submit_calls;
            output->native_events += emitter.total;
            emitter.total = 0;
            emitter.submit_calls = 0;
            ++stats_.timed_ticks;
            if (!complete) {
                next_tick_ns_ = now_ns + timed_tick_ns;
                output->commands_completed = next_command_;
                output->flags = input_execution_pending;
                output->buttons = physical_.state().buttons;
                return SACCADE_OK;
            }
            const SaccadeResult advanced = physical_.advance(next_command_ + 1U);
            if (advanced != SACCADE_OK) return fail(advanced);
            emitter.commit();
            ++next_command_;
            ++stats_.commands;
            timed_kind_ = TimedKind::none;
        }
        if (next_command_ == active_plan_.header->command_count) {
            output->commands_completed = next_command_;
            output->buttons = physical_.state().buttons;
            active_plan_ = {};
            active_bytes_ = {};
            next_command_ = 0;
            return SACCADE_OK;
        }
        const SaccadeInputCommand& command = active_plan_.commands[next_command_];
        if (command.duration_ns != 0) {
            timed_start_ns_ = now_ns;
            timed_end_ns_ = now_ns + command.duration_ns;
            if (timed_end_ns_ <= now_ns) return fail(SACCADE_ERROR_CAPACITY);
            next_tick_ns_ = now_ns + timed_tick_ns;
            const SaccadePhysicalInputState state = physical_.state();
            timed_start_x_q8_ = state.pointer_x_q8;
            timed_start_y_q8_ = state.pointer_y_q8;
            if (command.kind == SACCADE_INPUT_COMMAND_POINTER_MOVE)
                timed_kind_ = TimedKind::move;
            else if (command.kind == SACCADE_INPUT_COMMAND_SCROLL)
                timed_kind_ = TimedKind::scroll;
            else if (command.kind == SACCADE_INPUT_COMMAND_WAIT)
                timed_kind_ = TimedKind::wait;
            else if (command.kind == SACCADE_INPUT_COMMAND_BUTTON_DOWN &&
                     (command.flags & SACCADE_INPUT_COMMAND_CONTINUOUS) != 0) {
                if (!emit_command(&emitter, desktop_, active_bytes_, *active_plan_.header, command) || !emitter.flush())
                    return fail(SACCADE_ERROR_BACKEND);
                stats_.native_events += emitter.total;
                stats_.submit_calls += emitter.submit_calls;
                output->native_events += emitter.total;
                emitter.total = 0;
                emitter.submit_calls = 0;
                const SaccadeResult advanced = physical_.advance(next_command_ + 1U);
                if (advanced != SACCADE_OK) return fail(advanced);
                emitter.commit();
                ++next_command_;
                ++stats_.commands;
                timed_kind_ = TimedKind::hold;
                next_tick_ns_ = timed_end_ns_;
            } else {
                return fail(SACCADE_ERROR_UNSUPPORTED);
            }
            output->commands_completed = next_command_;
            output->flags = input_execution_pending;
            output->buttons = physical_.state().buttons;
            return SACCADE_OK;
        }
        if (!emit_command(&emitter, desktop_, active_bytes_, *active_plan_.header, command) || !emitter.flush())
            return fail(SACCADE_ERROR_BACKEND);
        stats_.native_events += emitter.total;
        stats_.submit_calls += emitter.submit_calls;
        output->native_events += emitter.total;
        emitter.total = 0;
        emitter.submit_calls = 0;
        const SaccadeResult advanced = physical_.advance(next_command_ + 1U);
        if (advanced != SACCADE_OK) return fail(advanced);
        emitter.commit();
        ++next_command_;
        ++stats_.commands;
    }
}

SaccadeResult InputExecutor::advance(uint64_t now_ns, InputExecutionResult* output) noexcept {
    if (!initialized_ || now_ns == 0 || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (shutdown_pending_) return SACCADE_ERROR_STATE;
    if (pending_release_count_ != 0) {
        const SaccadeResult recovered = retry_pending_releases();
        if (recovered != SACCADE_OK) return recovered;
    }
    return continue_execution(now_ns, output);
}

SaccadeResult InputExecutor::append_pending_release(const INPUT& event) noexcept {
    if (pending_release_count_ == pending_releases_.size()) return SACCADE_ERROR_CAPACITY;
    pending_releases_[pending_release_count_++] = event;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::queue_release(const input::SyntheticRelease& release) noexcept {
    uint32_t required = release.held_key_count;
    constexpr std::array<uint32_t, 3> buttons{SACCADE_INPUT_BUTTON_LEFT, SACCADE_INPUT_BUTTON_RIGHT,
                                              SACCADE_INPUT_BUTTON_MIDDLE};
    constexpr std::array<uint32_t, 4> modifiers{SACCADE_INPUT_MODIFIER_META, SACCADE_INPUT_MODIFIER_ALT,
                                                SACCADE_INPUT_MODIFIER_CONTROL, SACCADE_INPUT_MODIFIER_SHIFT};
    for (uint32_t button : buttons)
        required += (release.buttons & button) != 0 ? 1U : 0U;
    for (uint32_t modifier : modifiers)
        required += (release.modifiers & modifier) != 0 ? 1U : 0U;
    if (required > pending_releases_.size() - pending_release_count_) return SACCADE_ERROR_CAPACITY;
    if (required != 0) ++stats_.releases;
    for (uint32_t button : buttons) {
        if ((release.buttons & button) == 0) continue;
        DWORD flags = 0;
        (void)mouse_button(button, true, &flags);
        (void)append_pending_release(mouse_event(flags));
    }
    for (uint32_t index = 0; index < release.held_key_count; ++index) {
        ScanCode scan{};
        if (physical_scan(release.held_keys[index], &scan))
            (void)append_pending_release(key_event(scan.value, scan.extended, true));
    }
    for (uint32_t modifier : modifiers) {
        if ((release.modifiers & modifier) == 0) continue;
        ScanCode scan{};
        (void)modifier_scan(modifier, &scan);
        (void)append_pending_release(key_event(scan.value, scan.extended, true));
    }
    return SACCADE_OK;
}

SaccadeResult InputExecutor::retry_pending_releases() noexcept {
    if (pending_release_count_ == 0) return SACCADE_OK;
    ++stats_.submit_calls;
    const uint32_t submitted = sink_.submit(sink_.context, pending_releases_.data(), pending_release_count_);
    if (submitted > pending_release_count_) return SACCADE_ERROR_BACKEND;
    stats_.native_events += submitted;
    pending_release_count_ -= submitted;
    if (submitted != 0 && pending_release_count_ != 0) {
        std::memmove(pending_releases_.data(), pending_releases_.data() + submitted,
                     static_cast<size_t>(pending_release_count_) * sizeof(INPUT));
    }
    return pending_release_count_ == 0 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

SaccadeResult InputExecutor::emit_release(const input::SyntheticRelease& release) noexcept {
    const SaccadeResult queued = queue_release(release);
    return queued == SACCADE_OK ? retry_pending_releases() : queued;
}

SaccadeResult InputExecutor::release_all() noexcept {
    if (!initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (shutdown_pending_) return retry_pending_releases();
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.abort(&release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::update_desktop(const VirtualDesktop& desktop) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (!desktop_valid(desktop)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (synthetic_input_active()) return SACCADE_ERROR_BUSY;
    desktop_ = desktop;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::physical_override(int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept {
    if (!initialized_ || shutdown_pending_) {
        return SACCADE_ERROR_STATE;
    }
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.physical_override(pointer_x_q8, pointer_y_q8, &release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::permission_lost(uint64_t new_permission_epoch) noexcept {
    if (!initialized_ || shutdown_pending_) {
        return SACCADE_ERROR_STATE;
    }
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.permission_lost(new_permission_epoch, &release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::shutdown() noexcept {
    if (!initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (!shutdown_pending_) {
        input::SyntheticRelease release{};
        const SaccadeResult result = physical_.shutdown(&release);
        if (result != SACCADE_OK) return result;
        const SaccadeResult queued = queue_release(release);
        if (queued != SACCADE_OK) return queued;
        active_plan_ = {};
        active_bytes_ = {};
        next_command_ = 0;
        timed_kind_ = TimedKind::none;
        shutdown_pending_ = true;
    }
    const SaccadeResult release_result = retry_pending_releases();
    if (release_result != SACCADE_OK) return release_result;
    shutdown_pending_ = false;
    initialized_ = false;
    return SACCADE_OK;
}

bool synthetic_input_active(void* context) noexcept {
    const auto* executor = static_cast<const InputExecutor*>(context);
    return executor != nullptr && executor->synthetic_input_active();
}

SaccadeResult neutralize_synthetic_input(void* context) noexcept {
    auto* executor = static_cast<InputExecutor*>(context);
    return executor == nullptr ? SACCADE_ERROR_INVALID_ARGUMENT : executor->release_all();
}

} // namespace saccade::platform::windows

#include "platform/macos/input_executor.hpp"

#include "input/injected_marker.hpp"
#include "input/plan.hpp"
#include "input/utf8.hpp"
#include "platform/macos/keyboard.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace saccade::platform::macos {
namespace {

constexpr uint32_t coordinate_fraction_bits = 8;
constexpr int64_t coordinate_scale_integer = INT64_C(1) << coordinate_fraction_bits;
constexpr double coordinate_scale = static_cast<double>(coordinate_scale_integer);
constexpr int64_t scroll_fixed_point_scale = coordinate_scale_integer;
constexpr uint64_t injected_event_marker = input::injected_event_marker;
constexpr float window_messaging_timeout_seconds = 0.5F;
constexpr uint32_t first_click_count = 1;
constexpr int32_t scroll_axis_count = 2;
constexpr size_t unicode_units_per_scalar = 2;
constexpr uint64_t timed_tick_ns = UINT64_C(8'333'333);

struct ModifierMapping {
    uint32_t bit = 0;
    CGKeyCode keycode = 0;
    CGEventFlags flag = 0;
};

constexpr auto modifier_order =
    std::to_array<ModifierMapping>({{SACCADE_INPUT_MODIFIER_SHIFT, kVK_Shift, kCGEventFlagMaskShift},
                                    {SACCADE_INPUT_MODIFIER_CONTROL, kVK_Control, kCGEventFlagMaskControl},
                                    {SACCADE_INPUT_MODIFIER_ALT, kVK_Option, kCGEventFlagMaskAlternate},
                                    {SACCADE_INPUT_MODIFIER_META, kVK_Command, kCGEventFlagMaskCommand}});

constexpr auto button_order =
    std::to_array<uint32_t>({SACCADE_INPUT_BUTTON_LEFT, SACCADE_INPUT_BUTTON_RIGHT, SACCADE_INPUT_BUTTON_MIDDLE});

bool desktop_valid(const Desktop& desktop) noexcept {
    return desktop.width != 0 && desktop.height != 0 && desktop.topology_epoch != 0 &&
           static_cast<uint64_t>(desktop.width) <= static_cast<uint64_t>(INT32_MAX) &&
           static_cast<uint64_t>(desktop.height) <= static_cast<uint64_t>(INT32_MAX);
}

bool point_in_desktop(const Desktop& desktop, int32_t x_q8, int32_t y_q8) noexcept {
    const int64_t left = static_cast<int64_t>(desktop.x) * coordinate_scale_integer;
    const int64_t top = static_cast<int64_t>(desktop.y) * coordinate_scale_integer;
    const int64_t right = (static_cast<int64_t>(desktop.x) + desktop.width) * coordinate_scale_integer;
    const int64_t bottom = (static_cast<int64_t>(desktop.y) + desktop.height) * coordinate_scale_integer;
    return x_q8 >= left && x_q8 < right && y_q8 >= top && y_q8 < bottom;
}

CGPoint point_from_q8(int32_t x_q8, int32_t y_q8) noexcept {
    return {static_cast<double>(x_q8) / coordinate_scale, static_cast<double>(y_q8) / coordinate_scale};
}

bool button_mapping(uint32_t button, CGMouseButton* native_button, CGEventType* down, CGEventType* up,
                    CGEventType* dragged) noexcept {
    if (button == SACCADE_INPUT_BUTTON_LEFT) {
        *native_button = kCGMouseButtonLeft;
        *down = kCGEventLeftMouseDown;
        *up = kCGEventLeftMouseUp;
        *dragged = kCGEventLeftMouseDragged;
        return true;
    }
    if (button == SACCADE_INPUT_BUTTON_RIGHT) {
        *native_button = kCGMouseButtonRight;
        *down = kCGEventRightMouseDown;
        *up = kCGEventRightMouseUp;
        *dragged = kCGEventRightMouseDragged;
        return true;
    }
    if (button == SACCADE_INPUT_BUTTON_MIDDLE) {
        *native_button = kCGMouseButtonCenter;
        *down = kCGEventOtherMouseDown;
        *up = kCGEventOtherMouseUp;
        *dragged = kCGEventOtherMouseDragged;
        return true;
    }
    return false;
}

bool add_key(std::array<uint32_t, input::maximum_held_keys>* keys, uint32_t* count, uint32_t usage) noexcept {
    if (*count == input::maximum_held_keys ||
        std::find(keys->begin(), keys->begin() + *count, usage) != keys->begin() + *count) {
        return false;
    }
    (*keys)[(*count)++] = usage;
    return true;
}

void remove_key(std::array<uint32_t, input::maximum_held_keys>* keys, uint32_t* count, uint32_t usage) noexcept {
    const auto end = keys->begin() + *count;
    const auto found = std::find(keys->begin(), end, usage);
    if (found == end) return;
    *found = *(end - 1);
    --*count;
}

int64_t point_delta(int32_t q8) noexcept {
    const int64_t value = q8;
    const int64_t rounding = INT64_C(1) << (coordinate_fraction_bits - 1U);
    return value >= 0 ? (value + rounding) >> coordinate_fraction_bits
                      : -((-value + rounding) >> coordinate_fraction_bits);
}

class NativeEmitter final {
  public:
    NativeEmitter(const InputSink& sink, CGEventRef mouse, CGEventRef keyboard, CGEventRef scroll,
                  CGPoint point) noexcept
        : sink_(sink), mouse_(mouse), keyboard_(keyboard), scroll_(scroll), point_(point) {}

    bool move(int32_t x_q8, int32_t y_q8, uint32_t held_buttons) noexcept {
        point_ = point_from_q8(x_q8, y_q8);
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton native_button = kCGMouseButtonLeft;
        CGEventType down = kCGEventNull;
        CGEventType up = kCGEventNull;
        CGEventType dragged = kCGEventNull;
        for (uint32_t button : button_order) {
            if ((held_buttons & button) != 0) {
                button_mapping(button, &native_button, &down, &up, &dragged);
                type = dragged;
                break;
            }
        }
        return submit_mouse(type, native_button, 0);
    }

    bool button(uint32_t button, bool up, uint32_t click_count) noexcept {
        CGMouseButton native_button = kCGMouseButtonLeft;
        CGEventType down = kCGEventNull;
        CGEventType up_type = kCGEventNull;
        CGEventType dragged = kCGEventNull;
        if (!button_mapping(button, &native_button, &down, &up_type, &dragged)) return false;
        if (!up) transient_buttons_ |= button;
        if (!submit_mouse(up ? up_type : down, native_button, click_count)) return false;
        if (up) transient_buttons_ &= ~button;
        return true;
    }

    bool scroll(int32_t x_q8, int32_t y_q8) noexcept {
        CGEventSetLocation(scroll_, point_);
        CGEventSetFlags(scroll_, flags_);
        CGEventSetIntegerValueField(scroll_, kCGScrollWheelEventFixedPtDeltaAxis1,
                                    static_cast<int64_t>(y_q8) * scroll_fixed_point_scale);
        CGEventSetIntegerValueField(scroll_, kCGScrollWheelEventFixedPtDeltaAxis2,
                                    static_cast<int64_t>(x_q8) * scroll_fixed_point_scale);
        CGEventSetIntegerValueField(scroll_, kCGScrollWheelEventPointDeltaAxis1, point_delta(y_q8));
        CGEventSetIntegerValueField(scroll_, kCGScrollWheelEventPointDeltaAxis2, point_delta(x_q8));
        return submit(scroll_);
    }

    bool key(uint32_t usage, bool up) noexcept {
        CGKeyCode keycode = 0;
        if (!keycode_from_hid_usage(usage, &keycode)) return false;
        if (!up && !add_key(&transient_keys_, &transient_key_count_, usage)) return false;
        if (!submit_keyboard(up ? kCGEventKeyUp : kCGEventKeyDown, keycode, nullptr, 0)) return false;
        if (up) remove_key(&transient_keys_, &transient_key_count_, usage);
        return true;
    }

    bool unicode(const UniChar* units, UniCharCount count, bool up) noexcept {
        return submit_keyboard(up ? kCGEventKeyUp : kCGEventKeyDown, 0, units, count);
    }

    bool modifiers(uint32_t modifiers, bool up) noexcept {
        for (uint32_t offset = 0; offset < modifier_order.size(); ++offset) {
            const uint32_t index = up ? static_cast<uint32_t>(modifier_order.size()) - 1U - offset : offset;
            const ModifierMapping& mapping = modifier_order[index];
            if ((modifiers & mapping.bit) == 0) continue;
            if (up)
                flags_ &= ~mapping.flag;
            else
                flags_ |= mapping.flag;
            if (!submit_keyboard(kCGEventFlagsChanged, mapping.keycode, nullptr, 0)) return false;
        }
        return true;
    }

    void commit_command() noexcept {
        transient_buttons_ = 0;
        transient_key_count_ = 0;
    }

    void release_transient() noexcept {
        for (uint32_t button_value : button_order) {
            if ((transient_buttons_ & button_value) != 0) (void)button(button_value, true, first_click_count);
        }
        while (transient_key_count_ != 0) {
            const uint32_t usage = transient_keys_[transient_key_count_ - 1U];
            (void)key(usage, true);
        }
        uint32_t active_modifiers = 0;
        for (const ModifierMapping& mapping : modifier_order) {
            if ((flags_ & mapping.flag) != 0) active_modifiers |= mapping.bit;
        }
        (void)modifiers(active_modifiers, true);
    }

    [[nodiscard]] uint32_t total() const noexcept { return total_; }

    [[nodiscard]] uint64_t submit_calls() const noexcept { return submit_calls_; }

    [[nodiscard]] uint32_t transient_buttons() const noexcept { return transient_buttons_; }

  private:
    bool submit_mouse(CGEventType type, CGMouseButton button, uint32_t click_count) noexcept {
        CGEventSetType(mouse_, type);
        CGEventSetLocation(mouse_, point_);
        CGEventSetFlags(mouse_, flags_);
        CGEventSetIntegerValueField(mouse_, kCGMouseEventButtonNumber, button);
        CGEventSetIntegerValueField(mouse_, kCGMouseEventClickState, click_count);
        return submit(mouse_);
    }

    bool submit_keyboard(CGEventType type, CGKeyCode keycode, const UniChar* units, UniCharCount unit_count) noexcept {
        static constexpr UniChar empty_unit = 0;
        CGEventSetType(keyboard_, type);
        CGEventSetIntegerValueField(keyboard_, kCGKeyboardEventKeycode, keycode);
        CGEventSetIntegerValueField(keyboard_, kCGKeyboardEventAutorepeat, 0);
        CGEventSetFlags(keyboard_, flags_);
        CGEventKeyboardSetUnicodeString(keyboard_, unit_count, unit_count == 0 ? &empty_unit : units);
        return submit(keyboard_);
    }

    bool submit(CGEventRef event) noexcept {
        CGEventSetTimestamp(event, mach_absolute_time());
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, static_cast<int64_t>(injected_event_marker));
        ++submit_calls_;
        if (!sink_.submit(sink_.context, event)) return false;
        ++total_;
        return true;
    }

    InputSink sink_{};
    CGEventRef mouse_ = nullptr;
    CGEventRef keyboard_ = nullptr;
    CGEventRef scroll_ = nullptr;
    CGPoint point_{};
    CGEventFlags flags_ = 0;
    std::array<uint32_t, input::maximum_held_keys> transient_keys_{};
    uint32_t transient_buttons_ = 0;
    uint32_t transient_key_count_ = 0;
    uint32_t total_ = 0;
    uint64_t submit_calls_ = 0;
};

bool emit_text(NativeEmitter* emitter, const uint8_t* data, size_t size) noexcept {
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    constexpr uint32_t supplementary_offset = UINT32_C(0x10000);
    constexpr uint32_t high_surrogate_first = UINT32_C(0xd800);
    constexpr uint32_t low_surrogate_first = UINT32_C(0xdc00);
    constexpr uint32_t surrogate_shift = 10;
    constexpr uint32_t surrogate_mask = UINT32_C(0x3ff);
    while (cursor != end) {
        uint32_t codepoint = 0;
        if (!input::decode_utf8(&cursor, end, &codepoint)) return false;
        std::array<UniChar, unicode_units_per_scalar> units{};
        UniCharCount unit_count = 1;
        if (codepoint <= UINT16_MAX) {
            units[0] = static_cast<UniChar>(codepoint);
        } else {
            codepoint -= supplementary_offset;
            units[0] = static_cast<UniChar>(high_surrogate_first + (codepoint >> surrogate_shift));
            units[1] = static_cast<UniChar>(low_surrogate_first + (codepoint & surrogate_mask));
            unit_count = static_cast<UniCharCount>(units.size());
        }
        if (!emitter->unicode(units.data(), unit_count, false) || !emitter->unicode(units.data(), unit_count, true))
            return false;
    }
    return true;
}

bool command_supported(const SaccadeInputCommand& command, const Desktop& desktop, SaccadeSpanU8 bytes) noexcept {
    if ((command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 && !point_in_desktop(desktop, command.x_q8, command.y_q8))
        return false;
    if (command.kind == SACCADE_INPUT_COMMAND_KEY_DOWN || command.kind == SACCADE_INPUT_COMMAND_KEY_UP) {
        CGKeyCode keycode = 0;
        return keycode_from_hid_usage(command.data0, &keycode);
    }
    if (command.kind == SACCADE_INPUT_COMMAND_TEXT) {
        const uint8_t* cursor = bytes.data + command.payload_offset;
        const uint8_t* end = cursor + command.payload_size;
        while (cursor != end) {
            uint32_t codepoint = 0;
            if (!input::decode_utf8(&cursor, end, &codepoint)) return false;
        }
    }
    return true;
}

bool emit_command(NativeEmitter* emitter, SaccadeSpanU8 bytes, const SaccadeInputCommand& command,
                  uint32_t held_buttons) noexcept {
    switch (command.kind) {
    case SACCADE_INPUT_COMMAND_POINTER_MOVE:
        return emitter->move(command.x_q8, command.y_q8, held_buttons);
    case SACCADE_INPUT_COMMAND_BUTTON_DOWN:
    case SACCADE_INPUT_COMMAND_BUTTON_UP:
        if ((command.flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0 &&
            !emitter->move(command.x_q8, command.y_q8, held_buttons))
            return false;
        return emitter->button(command.data0, command.kind == SACCADE_INPUT_COMMAND_BUTTON_UP, first_click_count);
    case SACCADE_INPUT_COMMAND_CLICK:
        if (!emitter->move(command.x_q8, command.y_q8, held_buttons) || !emitter->modifiers(command.data2, false))
            return false;
        for (uint32_t click = 0; click < command.data1; ++click) {
            const uint32_t click_count = first_click_count + click;
            if (!emitter->button(command.data0, false, click_count) ||
                !emitter->button(command.data0, true, click_count))
                return false;
        }
        return emitter->modifiers(command.data2, true);
    case SACCADE_INPUT_COMMAND_SCROLL:
        return emitter->move(command.x_q8, command.y_q8, held_buttons) &&
               emitter->scroll(command.delta_x_q8, command.delta_y_q8);
    case SACCADE_INPUT_COMMAND_KEY_DOWN:
    case SACCADE_INPUT_COMMAND_KEY_UP:
        return emitter->modifiers(command.data1, false) &&
               emitter->key(command.data0, command.kind == SACCADE_INPUT_COMMAND_KEY_UP) &&
               emitter->modifiers(command.data1, true);
    case SACCADE_INPUT_COMMAND_TEXT:
        return emit_text(emitter, bytes.data + command.payload_offset, command.payload_size);
    default:
        return false;
    }
}

bool dictionary_number(CFDictionaryRef dictionary, CFStringRef key, int64_t* output) noexcept {
    const auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return number != nullptr && CFGetTypeID(number) == CFNumberGetTypeID() &&
           CFNumberGetValue(number, kCFNumberSInt64Type, output);
}

bool ax_geometry(AXUIElementRef element, CGRect* output) noexcept {
    CFTypeRef position_value = nullptr;
    CFTypeRef size_value = nullptr;
    const AXError position_result = AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value);
    const AXError size_result = AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value);
    CGPoint position{};
    CGSize size{};
    const bool valid = position_result == kAXErrorSuccess && size_result == kAXErrorSuccess &&
                       position_value != nullptr && size_value != nullptr &&
                       CFGetTypeID(position_value) == AXValueGetTypeID() &&
                       CFGetTypeID(size_value) == AXValueGetTypeID() &&
                       AXValueGetValue(static_cast<AXValueRef>(position_value), kAXValueTypeCGPoint, &position) &&
                       AXValueGetValue(static_cast<AXValueRef>(size_value), kAXValueTypeCGSize, &size) &&
                       size.width > 0 && size.height > 0;
    if (position_value != nullptr) CFRelease(position_value);
    if (size_value != nullptr) CFRelease(size_value);
    if (valid) *output = {position, size};
    return valid;
}

} // namespace

InputExecutor::~InputExecutor() {
    if (initialized_) (void)shutdown();
    destroy_events();
}

SaccadeResult InputExecutor::initialize(const Desktop& desktop, const InputSink& sink, uint64_t permission_epoch,
                                        int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept {
    if (initialized_ || !desktop_valid(desktop) || sink.submit == nullptr ||
        !point_in_desktop(desktop, pointer_x_q8, pointer_y_q8))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult initialized = physical_.initialize(permission_epoch, pointer_x_q8, pointer_y_q8);
    if (initialized != SACCADE_OK) return initialized;
    event_source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
    const CGPoint initial_point = point_from_q8(pointer_x_q8, pointer_y_q8);
    mouse_event_ = CGEventCreateMouseEvent(event_source_, kCGEventMouseMoved, initial_point, kCGMouseButtonLeft);
    keyboard_event_ = CGEventCreateKeyboardEvent(event_source_, 0, false);
    scroll_event_ = CGEventCreateScrollWheelEvent(event_source_, kCGScrollEventUnitPixel, scroll_axis_count, 0, 0);
    if (event_source_ == nullptr || mouse_event_ == nullptr || keyboard_event_ == nullptr || scroll_event_ == nullptr) {
        input::SyntheticRelease ignored{};
        (void)physical_.shutdown(&ignored);
        destroy_events();
        return SACCADE_ERROR_BACKEND;
    }
    desktop_ = desktop;
    sink_ = sink;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::execute(SaccadeSpanU8 bytes, uint32_t available_permissions, uint64_t now_ns,
                                     InputExecutionResult* output) noexcept {
    if (!initialized_ || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    input::PlanView plan{};
    SaccadeResult result = input::validate_plan(bytes, &plan);
    if (result != SACCADE_OK) return result;
    if (plan.header->topology_epoch != desktop_.topology_epoch) return SACCADE_ERROR_STALE_HANDLE;
    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        if (!command_supported(plan.commands[index], desktop_, bytes) ||
            (plan.commands[index].kind == SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE && sink_.activate_window == nullptr))
            return SACCADE_ERROR_UNSUPPORTED;
    }
    if ((plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) != 0 && sink_.preflight != nullptr) {
        // The live path preflights each command right before it runs, so a
        // dry run must check every command too, not only the first.
        for (uint32_t index = 0; index < plan.header->command_count; ++index) {
            result = sink_.preflight(sink_.context, plan, index, now_ns);
            if (result != SACCADE_OK) return result;
        }
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
    const SaccadePhysicalInputState initial_state = physical_.state();
    NativeEmitter emitter(sink_, mouse_event_, keyboard_event_, scroll_event_,
                          point_from_q8(initial_state.pointer_x_q8, initial_state.pointer_y_q8));
    uint32_t accounted_events = 0;
    uint64_t accounted_calls = 0;
    auto account = [&]() noexcept {
        const uint32_t events = emitter.total() - accounted_events;
        const uint64_t calls = emitter.submit_calls() - accounted_calls;
        stats_.native_events += events;
        stats_.submit_calls += calls;
        output->native_events += events;
        accounted_events = emitter.total();
        accounted_calls = emitter.submit_calls();
    };
    auto fail = [&](SaccadeResult failure) noexcept {
        emitter.release_transient();
        account();
        input::SyntheticRelease release{};
        (void)physical_.backend_failure(&release);
        (void)emit_release(release);
        active_plan_ = {};
        active_bytes_ = {};
        next_command_ = 0;
        timed_kind_ = TimedKind::none;
        ++stats_.failures;
        return failure;
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
    auto emit_one = [&](const SaccadeInputCommand& command) noexcept {
        return command.kind == SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE
                   ? sink_.activate_window != nullptr &&
                         sink_.activate_window(sink_.context, active_plan_.header->window_id) == SACCADE_OK
                   : emit_command(&emitter, active_bytes_, command,
                                  physical_.state().buttons | emitter.transient_buttons());
    };
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
                if (!emit_one(command)) return fail(SACCADE_ERROR_BACKEND);
            } else if (timed_kind_ == TimedKind::scroll && !complete) {
                if (!emit_one(source)) return fail(SACCADE_ERROR_BACKEND);
            }
            account();
            emitter.commit_command();
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
                if (!emit_one(command)) return fail(SACCADE_ERROR_BACKEND);
                const SaccadeResult advanced = physical_.advance(next_command_ + 1U);
                if (advanced != SACCADE_OK) return fail(advanced);
                emitter.commit_command();
                account();
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
        if (!emit_one(command)) return fail(SACCADE_ERROR_BACKEND);
        const SaccadeResult advanced = physical_.advance(next_command_ + 1U);
        if (advanced != SACCADE_OK) return fail(advanced);
        emitter.commit_command();
        account();
        ++next_command_;
        ++stats_.commands;
    }
}

SaccadeResult InputExecutor::advance(uint64_t now_ns, InputExecutionResult* output) noexcept {
    if (!initialized_ || now_ns == 0 || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    return continue_execution(now_ns, output);
}

SaccadeResult InputExecutor::emit_release(const input::SyntheticRelease& release) noexcept {
    const SaccadePhysicalInputState state = physical_.state();
    NativeEmitter emitter(sink_, mouse_event_, keyboard_event_, scroll_event_,
                          point_from_q8(state.pointer_x_q8, state.pointer_y_q8));
    for (uint32_t button : button_order) {
        if ((release.buttons & button) != 0 && !emitter.button(button, true, first_click_count))
            return SACCADE_ERROR_BACKEND;
    }
    for (uint32_t index = 0; index < release.held_key_count; ++index) {
        if (!emitter.key(release.held_keys[index], true)) return SACCADE_ERROR_BACKEND;
    }
    if (!emitter.modifiers(release.modifiers, true)) return SACCADE_ERROR_BACKEND;
    stats_.native_events += emitter.total();
    stats_.submit_calls += emitter.submit_calls();
    stats_.releases += emitter.total() != 0 ? 1U : 0U;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::release_all() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.abort(&release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::update_desktop(const Desktop& desktop) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (!desktop_valid(desktop)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (synthetic_input_active()) return SACCADE_ERROR_BUSY;
    desktop_ = desktop;
    return SACCADE_OK;
}

SaccadeResult InputExecutor::physical_override(int32_t pointer_x_q8, int32_t pointer_y_q8) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.physical_override(pointer_x_q8, pointer_y_q8, &release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::permission_lost(uint64_t new_permission_epoch) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.permission_lost(new_permission_epoch, &release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    return result == SACCADE_OK ? emit_release(release) : result;
}

SaccadeResult InputExecutor::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    input::SyntheticRelease release{};
    const SaccadeResult result = physical_.shutdown(&release);
    if (result != SACCADE_OK) return result;
    const SaccadeResult released = emit_release(release);
    active_plan_ = {};
    active_bytes_ = {};
    next_command_ = 0;
    timed_kind_ = TimedKind::none;
    initialized_ = false;
    destroy_events();
    return released;
}

void InputExecutor::destroy_events() noexcept {
    if (scroll_event_ != nullptr) {
        CFRelease(scroll_event_);
        scroll_event_ = nullptr;
    }
    if (keyboard_event_ != nullptr) {
        CFRelease(keyboard_event_);
        keyboard_event_ = nullptr;
    }
    if (mouse_event_ != nullptr) {
        CFRelease(mouse_event_);
        mouse_event_ = nullptr;
    }
    if (event_source_ != nullptr) {
        CFRelease(event_source_);
        event_source_ = nullptr;
    }
}

bool post_with_cg_event(void*, CGEventRef event) noexcept {
    if (event == nullptr) return false;
    CGEventPost(kCGHIDEventTap, event);
    return true;
}

bool input_permission_granted() noexcept {
    return CGPreflightPostEventAccess();
}

SaccadeResult activate_window_public(void*, uint64_t window_id) noexcept {
    if (window_id == 0 || window_id > UINT32_MAX) return SACCADE_ERROR_INVALID_ARGUMENT;
    @autoreleasepool {
        const CGWindowID native_id = static_cast<CGWindowID>(window_id);
        CFArrayRef windows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionIncludingWindow | kCGWindowListExcludeDesktopElements, native_id);
        if (windows == nullptr || CFArrayGetCount(windows) == 0) {
            if (windows != nullptr) CFRelease(windows);
            return SACCADE_ERROR_NOT_FOUND;
        }
        const auto description = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
        int64_t process_value = 0;
        const auto bounds_dictionary = static_cast<CFDictionaryRef>(CFDictionaryGetValue(description, kCGWindowBounds));
        CGRect target_bounds{};
        const bool valid = dictionary_number(description, kCGWindowOwnerPID, &process_value) && process_value > 0 &&
                           process_value <= INT32_MAX && bounds_dictionary != nullptr &&
                           CGRectMakeWithDictionaryRepresentation(bounds_dictionary, &target_bounds);
        CFRelease(windows);
        if (!valid) return SACCADE_ERROR_NOT_FOUND;
        if (!AXIsProcessTrusted()) return SACCADE_ERROR_PERMISSION;

        const pid_t process = static_cast<pid_t>(process_value);
        AXUIElementRef application_element = AXUIElementCreateApplication(process);
        if (application_element == nullptr) return SACCADE_ERROR_BACKEND;
        AXUIElementSetMessagingTimeout(application_element, window_messaging_timeout_seconds);
        CFTypeRef values = nullptr;
        const AXError copied = AXUIElementCopyAttributeValue(application_element, kAXWindowsAttribute, &values);
        AXUIElementRef best = nullptr;
        double best_distance = std::numeric_limits<double>::max();
        if (copied == kAXErrorSuccess && values != nullptr && CFGetTypeID(values) == CFArrayGetTypeID()) {
            const auto ax_windows = static_cast<CFArrayRef>(values);
            for (CFIndex index = 0; index < CFArrayGetCount(ax_windows); ++index) {
                const auto candidate =
                    static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(ax_windows, index)));
                CGRect bounds{};
                if (!ax_geometry(candidate, &bounds)) continue;
                const double distance = std::abs(bounds.origin.x - target_bounds.origin.x) +
                                        std::abs(bounds.origin.y - target_bounds.origin.y) +
                                        std::abs(bounds.size.width - target_bounds.size.width) +
                                        std::abs(bounds.size.height - target_bounds.size.height);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = candidate;
                }
            }
        }
        if (best != nullptr) CFRetain(best);
        if (values != nullptr) CFRelease(values);
        CFRelease(application_element);
        if (best == nullptr) return SACCADE_ERROR_NOT_FOUND;

        Boolean minimized_settable = false;
        if (AXUIElementIsAttributeSettable(best, kAXMinimizedAttribute, &minimized_settable) == kAXErrorSuccess &&
            minimized_settable) {
            (void)AXUIElementSetAttributeValue(best, kAXMinimizedAttribute, kCFBooleanFalse);
        }
        const AXError raised = AXUIElementPerformAction(best, kAXRaiseAction);
        (void)AXUIElementSetAttributeValue(best, kAXMainAttribute, kCFBooleanTrue);
        (void)AXUIElementSetAttributeValue(best, kAXFocusedAttribute, kCFBooleanTrue);
        CFRelease(best);
        NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:process];
        const BOOL activated = [application activateWithOptions:0];
        return raised == kAXErrorSuccess && activated ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    }
}

bool synthetic_input_active(void* context) noexcept {
    const auto* executor = static_cast<const InputExecutor*>(context);
    return executor != nullptr && executor->synthetic_input_active();
}

SaccadeResult neutralize_synthetic_input(void* context) noexcept {
    auto* executor = static_cast<InputExecutor*>(context);
    return executor == nullptr ? SACCADE_ERROR_INVALID_ARGUMENT : executor->release_all();
}

} // namespace saccade::platform::macos

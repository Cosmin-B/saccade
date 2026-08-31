#include "platform/macos/input_monitor.hpp"

#include "input/injected_marker.hpp"
#include "platform/macos/keyboard.hpp"

#import <ApplicationServices/ApplicationServices.h>
#include <mach/mach_time.h>

#include <cstdint>

namespace saccade::platform::macos {
namespace {

constexpr uint64_t no_timestamp_ns = 0;

CGEventMask monitored_event_mask() noexcept {
    return CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
           CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventOtherMouseDown) |
           CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kCGEventKeyDown) |
           CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
}

bool kind_from_type(CGEventType type, PhysicalInputKind* output) noexcept {
    if (output == nullptr)
        return false;
    switch (type) {
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged:
    case kCGEventOtherMouseDragged:
        *output = PhysicalInputKind::pointer;
        return true;
    case kCGEventLeftMouseDown:
    case kCGEventLeftMouseUp:
    case kCGEventRightMouseDown:
    case kCGEventRightMouseUp:
    case kCGEventOtherMouseDown:
    case kCGEventOtherMouseUp:
        *output = PhysicalInputKind::button;
        return true;
    case kCGEventScrollWheel:
        *output = PhysicalInputKind::scroll;
        return true;
    case kCGEventKeyDown:
    case kCGEventKeyUp:
        *output = PhysicalInputKind::key;
        return true;
    case kCGEventFlagsChanged:
        *output = PhysicalInputKind::modifier;
        return true;
    default:
        return false;
    }
}

uint64_t timestamp_ns(uint64_t ticks, uint32_t numer, uint32_t denom) noexcept {
    if (numer == 0 || denom == 0)
        return no_timestamp_ns;
    const uint64_t whole = ticks / denom;
    const uint64_t remainder = ticks % denom;
    return whole * numer + remainder * numer / denom;
}

uint32_t input_modifiers(CGEventFlags flags) noexcept {
    uint32_t result = 0;
    if ((flags & kCGEventFlagMaskShift) != 0)
        result |= SACCADE_INPUT_MODIFIER_SHIFT;
    if ((flags & kCGEventFlagMaskControl) != 0)
        result |= SACCADE_INPUT_MODIFIER_CONTROL;
    if ((flags & kCGEventFlagMaskAlternate) != 0)
        result |= SACCADE_INPUT_MODIFIER_ALT;
    if ((flags & kCGEventFlagMaskCommand) != 0)
        result |= SACCADE_INPUT_MODIFIER_META;
    return result;
}

uint16_t logical_symbol(CGEventRef event) noexcept {
    std::array<UniChar, 4> symbols{};
    UniCharCount count = 0;
    CGEventKeyboardGetUnicodeString(event, symbols.size(), &count, symbols.data());
    return count == 1 && (symbols[0] < 0xd800 || symbols[0] > 0xdfff) ? symbols[0] : 0;
}

} // namespace

bool physical_input_event_from_cg_event(CGEventRef event, uint32_t timebase_numer, uint32_t timebase_denom,
                                        PhysicalInputEvent* output) noexcept {
    if (event == nullptr || output == nullptr ||
        CGEventGetIntegerValueField(event, kCGEventSourceUserData) == static_cast<int64_t>(input::injected_event_marker))
        return false;
    PhysicalInputKind kind{};
    if (!kind_from_type(CGEventGetType(event), &kind))
        return false;
    PhysicalInputEvent value{};
    value.timestamp_ns = timestamp_ns(CGEventGetTimestamp(event), timebase_numer, timebase_denom);
    value.kind = kind;
    value.flags = static_cast<uint32_t>(CGEventGetFlags(event));
    *output = value;
    return true;
}

bool key_event_from_cg_event(CGEventRef event, uint64_t timestamp, bool include_logical_symbol, application::KeyEvent* output) noexcept {
    if (event == nullptr || output == nullptr || timestamp == 0)
        return false;
    const CGEventType type = CGEventGetType(event);
    if (type != kCGEventKeyDown && type != kCGEventKeyUp)
        return false;
    const auto keycode = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    uint32_t usage = 0;
    if (!hid_usage_from_keycode(keycode, &usage))
        return false;
    const uint16_t symbol = include_logical_symbol ? logical_symbol(event) : static_cast<uint16_t>(0);
    *output = {timestamp, usage, input_modifiers(CGEventGetFlags(event)), symbol, 0};
    return true;
}

InputMonitor::~InputMonitor() {
    if (initialized_ && owns_thread())
        (void)shutdown();
}

bool InputMonitor::owns_thread() const noexcept {
    return initialized_ && pthread_equal(owner_, pthread_self()) != 0;
}

SaccadeResult InputMonitor::initialize(InputMonitorSink sink) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (sink.input == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0)
        return SACCADE_ERROR_BACKEND;
    const CGEventTapOptions options = sink.key == nullptr ? kCGEventTapOptionListenOnly : kCGEventTapOptionDefault;
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, options, monitored_event_mask(), tap_callback, this);
    if (tap == nullptr)
        return SACCADE_ERROR_PERMISSION;
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    if (source == nullptr) {
        CFRelease(tap);
        return SACCADE_ERROR_BACKEND;
    }
    CFRunLoopRef run_loop = CFRunLoopGetCurrent();
    CFRetain(run_loop);
    CFRunLoopAddSource(run_loop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    sink_ = sink;
    tap_ = tap;
    source_ = source;
    run_loop_ = run_loop;
    owner_ = pthread_self();
    timebase_numer_ = timebase.numer;
    timebase_denom_ = timebase.denom;
    initialized_ = true;
    return SACCADE_OK;
}

bool InputMonitor::dispatch(CGEventRef event) noexcept {
    PhysicalInputEvent physical_event{};
    if (!physical_input_event_from_cg_event(event, timebase_numer_, timebase_denom_, &physical_event)) {
        if (event != nullptr &&
            CGEventGetIntegerValueField(event, kCGEventSourceUserData) == static_cast<int64_t>(input::injected_event_marker)) {
            ++stats_.injected_ignored;
        }
        return false;
    }
    ++stats_.events;
    switch (physical_event.kind) {
    case PhysicalInputKind::pointer:
        ++stats_.pointer_events;
        break;
    case PhysicalInputKind::button:
        ++stats_.button_events;
        break;
    case PhysicalInputKind::scroll:
        ++stats_.scroll_events;
        break;
    case PhysicalInputKind::key:
        ++stats_.key_events;
        break;
    case PhysicalInputKind::modifier:
        ++stats_.modifier_events;
        break;
    }
    if (physical_event.kind == PhysicalInputKind::key && sink_.key != nullptr) {
        const CGEventType type = CGEventGetType(event);
        const bool include_logical_symbol =
            type == kCGEventKeyDown && sink_.logical_input_active != nullptr && sink_.logical_input_active(sink_.context);
        application::KeyEvent key_event{};
        if (key_event_from_cg_event(event, physical_event.timestamp_ns, include_logical_symbol, &key_event) &&
            key_event.physical_key < session_pressed_.size()) {
            if (type == kCGEventKeyUp && session_pressed_[key_event.physical_key]) {
                session_pressed_[key_event.physical_key] = false;
                ++stats_.session_keys;
                return true;
            }
            if (type == kCGEventKeyDown && session_pressed_[key_event.physical_key]) {
                ++stats_.session_keys;
                return true;
            }
            if (type == kCGEventKeyDown && sink_.key(sink_.context, key_event)) {
                session_pressed_[key_event.physical_key] = true;
                ++stats_.session_keys;
                return true;
            }
        }
    }
    sink_.input(sink_.context, physical_event);
    return false;
}

CGEventRef InputMonitor::tap_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context) noexcept {
    auto* monitor = static_cast<InputMonitor*>(context);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(monitor->tap_, true);
        ++monitor->stats_.tap_reenabled;
        return event;
    }
    return monitor->dispatch(event) ? nullptr : event;
}

SaccadeResult InputMonitor::shutdown() noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    CGEventTapEnable(tap_, false);
    CFRunLoopRemoveSource(run_loop_, source_, kCFRunLoopCommonModes);
    CFRelease(source_);
    CFRelease(tap_);
    CFRelease(run_loop_);
    sink_ = {};
    source_ = nullptr;
    tap_ = nullptr;
    run_loop_ = nullptr;
    timebase_numer_ = 0;
    timebase_denom_ = 0;
    session_pressed_.fill(false);
    initialized_ = false;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

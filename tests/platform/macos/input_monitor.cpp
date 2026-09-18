#include "input/injected_marker.hpp"
#include "platform/macos/input_monitor.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    source_failed,
    pointer_failed,
    button_failed,
    scroll_failed,
    key_failed,
    modifier_failed,
    injected_event_failed
};

constexpr uint64_t event_ticks = 1000;

static_assert(sizeof(saccade::platform::macos::PhysicalInputEvent) == 16, "physical readiness events must not retain pointer coordinates");

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (source == nullptr)
        return result(TestResult::source_failed);
    CGEventRef pointer = CGEventCreateMouseEvent(source, kCGEventMouseMoved, {-1.5, 2.0}, kCGMouseButtonLeft);
    if (pointer == nullptr) {
        CFRelease(source);
        return result(TestResult::source_failed);
    }
    CGEventSetTimestamp(pointer, event_ticks);
    saccade::platform::macos::PhysicalInputEvent event{};
    if (!saccade::platform::macos::physical_input_event_from_cg_event(pointer, 1, 1, &event) ||
        event.kind != saccade::platform::macos::PhysicalInputKind::pointer || event.timestamp_ns != event_ticks)
        return result(TestResult::pointer_failed);
    CGEventSetType(pointer, kCGEventLeftMouseDown);
    if (!saccade::platform::macos::physical_input_event_from_cg_event(pointer, 1, 1, &event) ||
        event.kind != saccade::platform::macos::PhysicalInputKind::button)
        return result(TestResult::button_failed);
    CGEventSetType(pointer, kCGEventScrollWheel);
    if (!saccade::platform::macos::physical_input_event_from_cg_event(pointer, 1, 1, &event) ||
        event.kind != saccade::platform::macos::PhysicalInputKind::scroll)
        return result(TestResult::scroll_failed);
    CGEventRef key = CGEventCreateKeyboardEvent(source, 0, true);
    if (key == nullptr) {
        CFRelease(pointer);
        CFRelease(source);
        return result(TestResult::source_failed);
    }
    CGEventSetTimestamp(key, event_ticks);
    if (!saccade::platform::macos::physical_input_event_from_cg_event(key, 1, 1, &event) ||
        event.kind != saccade::platform::macos::PhysicalInputKind::key)
        return result(TestResult::key_failed);
    saccade::application::KeyEvent routed{};
    if (!saccade::platform::macos::key_event_from_cg_event(key, event_ticks, false, &routed) || routed.logical_symbol != 0)
        return result(TestResult::key_failed);
    if (!saccade::platform::macos::key_event_from_cg_event(key, event_ticks, true, &routed) || routed.logical_symbol == 0)
        return result(TestResult::key_failed);
    CGEventSetType(key, kCGEventFlagsChanged);
    if (!saccade::platform::macos::physical_input_event_from_cg_event(key, 1, 1, &event) ||
        event.kind != saccade::platform::macos::PhysicalInputKind::modifier)
        return result(TestResult::modifier_failed);
    CGEventSetIntegerValueField(key, kCGEventSourceUserData, static_cast<int64_t>(saccade::input::injected_event_marker));
    const bool injected = saccade::platform::macos::physical_input_event_from_cg_event(key, 1, 1, &event);
    CFRelease(key);
    CFRelease(pointer);
    CFRelease(source);
    return injected ? result(TestResult::injected_event_failed) : result(TestResult::success);
}

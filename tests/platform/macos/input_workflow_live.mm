#include "platform/macos/input_executor.hpp"

#import <AppKit/AppKit.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    permission_missing,
    display_query_failed,
    pointer_query_failed,
    window_creation_failed,
    window_focus_failed,
    window_geometry_failed,
    executor_initialization_failed,
    left_click_failed,
    alternate_click_failed,
    drag_failed,
    scroll_failed,
    text_failed,
    physical_override_failed,
    stale_plan_failed,
    pointer_restore_failed,
    shutdown_failed
};

constexpr int test_skipped = 77;
constexpr char live_test_category[] = "workflow";
constexpr uint64_t topology_epoch = 17;
constexpr uint64_t permission_epoch = 19;
constexpr uint32_t maximum_commands = 8;
constexpr uint32_t maximum_payload_bytes = 64;
constexpr uint32_t expected_text_length = 7;
constexpr size_t plan_capacity = sizeof(SaccadeInputPlanHeader) +
                                 static_cast<size_t>(maximum_commands) * sizeof(SaccadeInputCommand) +
                                 maximum_payload_bytes;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

@interface SaccadeWorkflowView : NSView {
  @public
    uint32_t moves_;
    uint32_t drags_;
    uint32_t left_down_;
    uint32_t left_up_;
    uint32_t right_down_;
    uint32_t right_up_;
    uint32_t middle_down_;
    uint32_t middle_up_;
    uint32_t vertical_scroll_;
    uint32_t horizontal_scroll_;
    uint32_t key_down_;
    UniChar text_[maximum_payload_bytes];
    uint32_t text_count_;
}
@end

@implementation SaccadeWorkflowView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)mouseMoved:(NSEvent*)event {
    (void)event;
    ++moves_;
}

- (void)mouseDown:(NSEvent*)event {
    (void)event;
    ++left_down_;
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    ++left_up_;
}

- (void)rightMouseDown:(NSEvent*)event {
    (void)event;
    ++right_down_;
}

- (void)rightMouseUp:(NSEvent*)event {
    (void)event;
    ++right_up_;
}

- (void)otherMouseDown:(NSEvent*)event {
    (void)event;
    ++middle_down_;
}

- (void)otherMouseUp:(NSEvent*)event {
    (void)event;
    ++middle_up_;
}

- (void)mouseDragged:(NSEvent*)event {
    (void)event;
    ++drags_;
}

- (void)scrollWheel:(NSEvent*)event {
    vertical_scroll_ += std::abs(event.scrollingDeltaY) > 0.0 ? 1U : 0U;
    horizontal_scroll_ += std::abs(event.scrollingDeltaX) > 0.0 ? 1U : 0U;
}

- (void)keyDown:(NSEvent*)event {
    ++key_down_;
    NSString* characters = event.characters;
    for (NSUInteger index = 0; index < characters.length && text_count_ != maximum_payload_bytes; ++index) {
        text_[text_count_++] = [characters characterAtIndex:index];
    }
}

@end

namespace {

void pump_events(double seconds) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    do {
        @autoreleasepool {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event != nil) [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    } while (CFAbsoluteTimeGetCurrent() < deadline);
}

bool window_bounds(uint64_t window_id, CGRect* output) noexcept {
    CFArrayRef windows =
        CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>(window_id));
    if (windows == nullptr || CFArrayGetCount(windows) == 0) {
        if (windows != nullptr) CFRelease(windows);
        return false;
    }
    const auto description = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
    const auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(description, kCGWindowBounds));
    const bool valid = bounds != nullptr && CGRectMakeWithDictionaryRepresentation(bounds, output);
    CFRelease(windows);
    return valid;
}

bool desktop_bounds(saccade::platform::macos::Desktop* output) noexcept {
    std::array<CGDirectDisplayID, 32> displays{};
    uint32_t count = 0;
    if (CGGetActiveDisplayList(static_cast<uint32_t>(displays.size()), displays.data(), &count) != kCGErrorSuccess ||
        count == 0) {
        return false;
    }
    CGRect bounds = CGDisplayBounds(displays[0]);
    for (uint32_t index = 1; index < count; ++index)
        bounds = CGRectUnion(bounds, CGDisplayBounds(displays[index]));
    if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0 || bounds.size.width > UINT32_MAX ||
        bounds.size.height > UINT32_MAX) {
        return false;
    }
    *output = {static_cast<int32_t>(std::llround(bounds.origin.x)), static_cast<int32_t>(std::llround(bounds.origin.y)),
               static_cast<uint32_t>(std::llround(bounds.size.width)),
               static_cast<uint32_t>(std::llround(bounds.size.height)), topology_epoch};
    return true;
}

int32_t q8(double value) noexcept {
    return static_cast<int32_t>(std::llround(value * 256.0));
}

struct alignas(8) PlanStorage {
    std::array<uint8_t, plan_capacity> bytes{};
};

SaccadeSpanU8 make_plan(PlanStorage* storage, uint64_t plan_id, uint32_t permissions,
                        const SaccadeInputCommand* commands, uint32_t command_count, uint64_t window_id,
                        const uint8_t* payload = nullptr, uint32_t payload_size = 0) noexcept {
    if (storage == nullptr || commands == nullptr || command_count == 0 || command_count > maximum_commands ||
        payload_size > maximum_payload_bytes) {
        return {};
    }
    *storage = {};
    SaccadeInputPlanHeader header{};
    header.struct_size = sizeof(header);
    header.plan_version = SACCADE_INPUT_PLAN_VERSION;
    header.command_count = command_count;
    header.command_stride = sizeof(SaccadeInputCommand);
    header.flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    header.required_permissions = permissions;
    header.plan_id = plan_id;
    header.scene_epoch = 1;
    header.frame_id = 2;
    header.model_epoch = 3;
    header.session_epoch = 4;
    header.transform_epoch = 5;
    header.topology_epoch = topology_epoch;
    header.permission_epoch = permission_epoch;
    header.source_id = 6;
    header.focus_id = window_id;
    header.window_id = window_id;
    header.display_id = 7;
    header.deadline_ns = UINT64_MAX;
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

SaccadeInputCommand pointer_command(uint32_t kind, CGPoint point) noexcept {
    SaccadeInputCommand command{};
    command.kind = kind;
    command.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    command.target_id = 1;
    command.x_q8 = q8(point.x);
    command.y_q8 = q8(point.y);
    return command;
}

bool execute(saccade::platform::macos::InputExecutor* executor, SaccadeSpanU8 plan, uint32_t permissions) noexcept {
    saccade::platform::macos::InputExecutionResult execution{};
    return executor->execute(plan, permissions, 1, &execution) == SACCADE_OK && execution.flags == 0;
}

class LiveContext final {
  public:
    LiveContext() = default;

    ~LiveContext() {
        if (executor_initialized_) {
            (void)executor_.release_all();
            (void)executor_.shutdown();
        }
        CGWarpMouseCursorPosition(original_pointer_);
        if (window_ != nil) {
            [window_ orderOut:nil];
            [window_ close];
        }
    }

    LiveContext(const LiveContext&) = delete;
    LiveContext& operator=(const LiveContext&) = delete;

    bool open_window() {
        (void)[NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSScreen* screen = NSScreen.mainScreen;
        if (screen == nil) return false;
        const NSRect visible = screen.visibleFrame;
        const NSRect frame =
            NSMakeRect(visible.origin.x + 120.0, visible.origin.y + visible.size.height - 420.0, 640.0, 320.0);
        window_ = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        if (window_ == nil) return false;
        view_ = [[SaccadeWorkflowView alloc] initWithFrame:window_.contentView.bounds];
        if (view_ == nil) return false;
        view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        window_.title = @"Saccade Input Qualification";
        window_.contentView = view_;
        window_.acceptsMouseMovedEvents = YES;
        [window_ makeKeyAndOrderFront:nil];
        [window_ orderFrontRegardless];
        [NSApp activate];
        [window_ makeFirstResponder:view_];
        pump_events(0.1);
        return window_.windowNumber > 0;
    }

    [[nodiscard]] uint64_t window_id() const noexcept { return static_cast<uint64_t>(window_.windowNumber); }

    __strong NSWindow* window_ = nil;
    __strong SaccadeWorkflowView* view_ = nil;
    CGPoint original_pointer_{};
    saccade::platform::macos::InputExecutor executor_{};
    bool executor_initialized_ = false;
};

int run_test() {
    if (!saccade::platform::macos::input_permission_granted()) return test_skipped;

    LiveContext context;
    CGEventRef pointer_event = CGEventCreate(nullptr);
    if (pointer_event == nullptr) return result(TestResult::pointer_query_failed);
    context.original_pointer_ = CGEventGetLocation(pointer_event);
    CFRelease(pointer_event);

    saccade::platform::macos::Desktop desktop{};
    if (!desktop_bounds(&desktop)) return result(TestResult::display_query_failed);
    if (!context.open_window()) return result(TestResult::window_creation_failed);
    if (!NSApp.active || !context.window_.keyWindow || context.window_.firstResponder != context.view_)
        return result(TestResult::window_focus_failed);

    CGRect frame{};
    if (!window_bounds(context.window_id(), &frame) || frame.size.width < 300.0 || frame.size.height < 180.0)
        return result(TestResult::window_geometry_failed);
    const CGPoint first{frame.origin.x + 100.0, frame.origin.y + frame.size.height * 0.6};
    const CGPoint second{frame.origin.x + frame.size.width - 100.0, first.y};

    const saccade::platform::macos::InputSink sink{nullptr, saccade::platform::macos::post_with_cg_event, nullptr};
    if (context.executor_.initialize(desktop, sink, permission_epoch, q8(context.original_pointer_.x),
                                     q8(context.original_pointer_.y)) != SACCADE_OK) {
        return result(TestResult::executor_initialization_failed);
    }
    context.executor_initialized_ = true;
    static PlanStorage storage;

    std::array<SaccadeInputCommand, 2> left{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_CLICK, first)};
    left[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    left[1].data1 = 1;
    if (!execute(&context.executor_,
                 make_plan(&storage, 1, SACCADE_INPUT_PERMISSION_POINTER, left.data(),
                           static_cast<uint32_t>(left.size()), context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::left_click_failed);
    }
    pump_events(0.08);
    if (context.view_->moves_ == 0 || context.view_->left_down_ != 1 || context.view_->left_up_ != 1)
        return result(TestResult::left_click_failed);

    std::array<SaccadeInputCommand, 2> alternate{pointer_command(SACCADE_INPUT_COMMAND_CLICK, first),
                                                 pointer_command(SACCADE_INPUT_COMMAND_CLICK, first)};
    alternate[0].data0 = SACCADE_INPUT_BUTTON_RIGHT;
    alternate[0].data1 = 2;
    alternate[1].data0 = SACCADE_INPUT_BUTTON_MIDDLE;
    alternate[1].data1 = 1;
    if (!execute(&context.executor_,
                 make_plan(&storage, 2, SACCADE_INPUT_PERMISSION_POINTER, alternate.data(),
                           static_cast<uint32_t>(alternate.size()), context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::alternate_click_failed);
    }
    pump_events(0.08);
    if (context.view_->right_down_ != 2 || context.view_->right_up_ != 2 || context.view_->middle_down_ != 1 ||
        context.view_->middle_up_ != 1) {
        return result(TestResult::alternate_click_failed);
    }

    std::array<SaccadeInputCommand, 4> drag{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, second),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_UP, second)};
    drag[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    drag[3].data0 = SACCADE_INPUT_BUTTON_LEFT;
    const uint32_t drag_down_before = context.view_->left_down_;
    const uint32_t drag_up_before = context.view_->left_up_;
    if (!execute(&context.executor_,
                 make_plan(&storage, 3, SACCADE_INPUT_PERMISSION_POINTER, drag.data(),
                           static_cast<uint32_t>(drag.size()), context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::drag_failed);
    }
    pump_events(0.08);
    if (context.view_->left_down_ != drag_down_before + 1U || context.view_->left_up_ != drag_up_before + 1U ||
        context.view_->drags_ == 0) {
        return result(TestResult::drag_failed);
    }

    SaccadeInputCommand scroll = pointer_command(SACCADE_INPUT_COMMAND_SCROLL, second);
    scroll.delta_x_q8 = 256;
    scroll.delta_y_q8 = -512;
    if (!execute(&context.executor_,
                 make_plan(&storage, 4, SACCADE_INPUT_PERMISSION_POINTER, &scroll, 1, context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::scroll_failed);
    }
    pump_events(0.08);
    if (context.view_->vertical_scroll_ == 0 || context.view_->horizontal_scroll_ == 0)
        return result(TestResult::scroll_failed);

    constexpr std::array<uint8_t, expected_text_length> text{'S', 'a', 'c', 'c', 'a', 'd', 'e'};
    constexpr std::array<UniChar, expected_text_length> expected_text{'S', 'a', 'c', 'c', 'a', 'd', 'e'};
    SaccadeInputCommand text_command{};
    text_command.kind = SACCADE_INPUT_COMMAND_TEXT;
    text_command.target_id = 1;
    text_command.payload_offset = sizeof(SaccadeInputPlanHeader) + sizeof(SaccadeInputCommand);
    text_command.payload_size = static_cast<uint32_t>(text.size());
    [context.window_ makeFirstResponder:context.view_];
    if (!execute(&context.executor_,
                 make_plan(&storage, 5, SACCADE_INPUT_PERMISSION_TEXT, &text_command, 1, context.window_id(),
                           text.data(), static_cast<uint32_t>(text.size())),
                 SACCADE_INPUT_PERMISSION_TEXT)) {
        return result(TestResult::text_failed);
    }
    pump_events(0.08);
    if (context.view_->text_count_ != expected_text.size() ||
        std::memcmp(context.view_->text_, expected_text.data(), expected_text.size() * sizeof(UniChar)) != 0) {
        return result(TestResult::text_failed);
    }

    std::array<SaccadeInputCommand, 2> hold{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, second),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, second)};
    hold[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    const uint32_t release_before = context.view_->left_up_;
    if (!execute(&context.executor_,
                 make_plan(&storage, 6, SACCADE_INPUT_PERMISSION_POINTER, hold.data(),
                           static_cast<uint32_t>(hold.size()), context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER) ||
        !context.executor_.synthetic_input_active() ||
        context.executor_.physical_override(q8(context.original_pointer_.x), q8(context.original_pointer_.y)) !=
            SACCADE_OK ||
        context.executor_.synthetic_input_active()) {
        return result(TestResult::physical_override_failed);
    }
    pump_events(0.08);
    if (context.view_->left_up_ != release_before + 1U) return result(TestResult::physical_override_failed);

    SaccadeInputCommand stale = pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first);
    SaccadeSpanU8 stale_plan = make_plan(&storage, 7, SACCADE_INPUT_PERMISSION_POINTER, &stale, 1, context.window_id());
    reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data())->topology_epoch = topology_epoch + 1U;
    saccade::platform::macos::InputExecutionResult execution{};
    if (context.executor_.execute(stale_plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &execution) !=
        SACCADE_ERROR_STALE_HANDLE) {
        return result(TestResult::stale_plan_failed);
    }

    SaccadeInputCommand restore = pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, context.original_pointer_);
    if (!execute(&context.executor_,
                 make_plan(&storage, 8, SACCADE_INPUT_PERMISSION_POINTER, &restore, 1, context.window_id()),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::pointer_restore_failed);
    }
    pump_events(0.08);
    CGEventRef restored_event = CGEventCreate(nullptr);
    if (restored_event == nullptr) return result(TestResult::pointer_restore_failed);
    const CGPoint restored = CGEventGetLocation(restored_event);
    CFRelease(restored_event);
    if (std::abs(restored.x - context.original_pointer_.x) > 1.0 ||
        std::abs(restored.y - context.original_pointer_.y) > 1.0) {
        return result(TestResult::pointer_restore_failed);
    }

    if (context.executor_.shutdown() != SACCADE_OK) return result(TestResult::shutdown_failed);
    context.executor_initialized_ = false;
    return result(TestResult::success);
}

} // namespace

int main() {
    const char* category = std::getenv("SACCADE_ALLOW_LIVE_TESTS");
    if (category == nullptr || std::strcmp(category, live_test_category) != 0) return test_skipped;

    @autoreleasepool {
        return run_test();
    }
}

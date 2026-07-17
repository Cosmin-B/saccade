#include "core/stack_string_builder.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <unistd.h>

namespace {

using saccade::core::StackStringBuilder;

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    display_failure = 2,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t default_duration_seconds = 65;
constexpr uint32_t maximum_duration_seconds = 300;
constexpr uint32_t maximum_displays = 16;
constexpr CGFloat tile_size = 64.0;
constexpr CGFloat tile_inset = 12.0;
constexpr NSTimeInterval stimulus_period_seconds = 1.0 / 120.0;

bool parse_duration(const char* text, uint32_t* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const char* end = text;
    while (*end != '\0')
        ++end;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > maximum_duration_seconds) return false;
    *output = value;
    return true;
}

void emit(std::string_view text) noexcept {
    (void)write(STDOUT_FILENO, text.data(), text.size());
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc > 2) return to_process_exit_code(ExitCode::invalid_arguments);
        uint32_t duration_seconds = default_duration_seconds;
        if (argc == 2 && !parse_duration(argv[1], &duration_seconds))
            return to_process_exit_code(ExitCode::invalid_arguments);

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        std::array<__strong NSPanel*, maximum_displays> windows{};
        uint32_t window_count = 0;
        for (NSScreen* screen in NSScreen.screens) {
            if (window_count == windows.size()) return to_process_exit_code(ExitCode::display_failure);
            const NSRect visible = screen.visibleFrame;
            const NSRect frame =
                NSMakeRect(NSMinX(visible) + tile_inset, NSMinY(visible) + tile_inset, tile_size, tile_size);
            NSPanel* window =
                [[NSPanel alloc] initWithContentRect:frame
                                           styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                             backing:NSBackingStoreBuffered
                                               defer:NO
                                              screen:screen];
            window.opaque = YES;
            window.backgroundColor = NSColor.blackColor;
            window.level = NSFloatingWindowLevel;
            window.ignoresMouseEvents = YES;
            window.hidesOnDeactivate = NO;
            window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                        NSWindowCollectionBehaviorFullScreenAuxiliary |
                                        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
            window.contentView.wantsLayer = YES;
            window.contentView.layer.backgroundColor = NSColor.blackColor.CGColor;
            [window orderFrontRegardless];
            windows[window_count++] = window;
        }
        if (window_count == 0) return to_process_exit_code(ExitCode::display_failure);

        const CGColorRef dark = CGColorCreateGenericGray(0.12, 1.0);
        const CGColorRef light = CGColorCreateGenericGray(0.20, 1.0);
        __block uint64_t ticks = 0;
        NSTimer* stimulus = [NSTimer timerWithTimeInterval:stimulus_period_seconds
                                                   repeats:YES
                                                     block:^(NSTimer*) {
                                                       const CGColorRef color = (ticks++ & 1U) == 0 ? dark : light;
                                                       [CATransaction begin];
                                                       [CATransaction setDisableActions:YES];
                                                       for (uint32_t index = 0; index < window_count; ++index) {
                                                           windows[index].contentView.layer.backgroundColor = color;
                                                       }
                                                       [CATransaction commit];
                                                     }];
        [[NSRunLoop mainRunLoop] addTimer:stimulus forMode:NSRunLoopCommonModes];

        [NSTimer scheduledTimerWithTimeInterval:duration_seconds
                                        repeats:NO
                                          block:^(NSTimer*) {
                                            [NSApp stop:nil];
                                            NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                                               location:NSZeroPoint
                                                                          modifierFlags:0
                                                                              timestamp:0
                                                                           windowNumber:0
                                                                                context:nil
                                                                                subtype:0
                                                                                  data1:0
                                                                                  data2:0];
                                            [NSApp postEvent:wake atStart:NO];
                                          }];
        [NSApp run];

        [stimulus invalidate];
        for (uint32_t index = 0; index < window_count; ++index) {
            [windows[index] orderOut:nil];
        }
        CGColorRelease(light);
        CGColorRelease(dark);

        StackStringBuilder<128> output;
        (void)output.append("macos_display_stimulus displays=");
        (void)output.append_unsigned(window_count);
        (void)output.append(" ticks=");
        (void)output.append_unsigned(ticks);
        (void)output.append('\n');
        emit(output.view());
        return to_process_exit_code(ExitCode::success);
    }
}

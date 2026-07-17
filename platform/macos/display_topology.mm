#include "platform/macos/display_topology.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace saccade::platform::macos {
namespace {

bool q8_value(CGFloat value, int32_t* output) noexcept {
    if (output == nullptr || !std::isfinite(value)) {
        return false;
    }
    const double scaled = static_cast<double>(value) * static_cast<double>(geometry::coordinate_one);
    if (scaled < static_cast<double>(INT32_MIN) || scaled > static_cast<double>(INT32_MAX)) {
        return false;
    }
    *output = static_cast<int32_t>(std::llround(scaled));
    return true;
}

bool q8_rect(CGFloat x, CGFloat y, CGFloat width, CGFloat height, geometry::RectQ8* output) noexcept {
    geometry::RectQ8 result{};
    if (output == nullptr || !q8_value(x, &result.x) || !q8_value(y, &result.y) || !q8_value(width, &result.width) ||
        !q8_value(height, &result.height) || !geometry::rect_valid(result)) {
        return false;
    }
    *output = result;
    return true;
}

bool positive_u32(CGFloat value, uint32_t* output) noexcept {
    if (output == nullptr || !std::isfinite(value) || value <= 0) {
        return false;
    }
    const double rounded = std::round(static_cast<double>(value));
    if (rounded <= 0 || rounded > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *output = static_cast<uint32_t>(rounded);
    return true;
}

bool quarter_turn(double degrees, geometry::QuarterTurn* output) noexcept {
    if (output == nullptr || !std::isfinite(degrees)) {
        return false;
    }
    double normalized = std::fmod(degrees, 360.0);
    if (normalized < 0) {
        normalized += 360.0;
    }
    const long long quarter = std::llround(normalized / 90.0);
    const double snapped = static_cast<double>(quarter) * 90.0;
    if (std::abs(normalized - snapped) > 0.01) {
        return false;
    }
    *output = static_cast<geometry::QuarterTurn>(static_cast<uint32_t>(quarter) & UINT32_C(3));
    return true;
}

bool display_id(NSScreen* screen, CGDirectDisplayID* output) noexcept {
    if (screen == nil || output == nullptr) {
        return false;
    }
    id value = screen.deviceDescription[@"NSScreenNumber"];
    if (![value isKindOfClass:NSNumber.class]) {
        return false;
    }
    const uint32_t identifier = static_cast<NSNumber*>(value).unsignedIntValue;
    if (identifier == kCGNullDirectDisplay) {
        return false;
    }
    *output = static_cast<CGDirectDisplayID>(identifier);
    return true;
}

bool collect_display(NSScreen* screen, geometry::DisplaySurface* output) noexcept {
    CGDirectDisplayID identifier = kCGNullDirectDisplay;
    if (output == nullptr || !display_id(screen, &identifier)) {
        return false;
    }

    geometry::DisplaySurface result{};
    result.display_id = identifier;
    const CGRect quartz_bounds = CGDisplayBounds(identifier);
    if (!q8_rect(quartz_bounds.origin.x, quartz_bounds.origin.y, quartz_bounds.size.width, quartz_bounds.size.height,
                 &result.desktop_bounds)) {
        return false;
    }

    const NSRect appkit_bounds = screen.frame;
    const NSRect visible = screen.visibleFrame;
    const CGFloat work_x = quartz_bounds.origin.x + (visible.origin.x - appkit_bounds.origin.x);
    const CGFloat work_y = quartz_bounds.origin.y + (NSMaxY(appkit_bounds) - NSMaxY(visible));
    if (!q8_rect(work_x, work_y, visible.size.width, visible.size.height, &result.work_bounds)) {
        return false;
    }

    const NSEdgeInsets safe = screen.safeAreaInsets;
    if (!q8_value(safe.top, &result.safe_insets.top) || !q8_value(safe.left, &result.safe_insets.left) ||
        !q8_value(safe.bottom, &result.safe_insets.bottom) || !q8_value(safe.right, &result.safe_insets.right)) {
        return false;
    }

    const NSRect local_bounds = NSMakeRect(0, 0, appkit_bounds.size.width, appkit_bounds.size.height);
    const NSRect backing_bounds = [screen convertRectToBacking:local_bounds];
    if (!positive_u32(backing_bounds.size.width, &result.backing_width) ||
        !positive_u32(backing_bounds.size.height, &result.backing_height)) {
        return false;
    }
    const NSInteger maximum_fps = screen.maximumFramesPerSecond;
    if (maximum_fps <= 0 || static_cast<uint64_t>(maximum_fps) > UINT32_MAX) {
        return false;
    }
    result.maximum_fps = static_cast<uint32_t>(maximum_fps);
    if (!quarter_turn(CGDisplayRotation(identifier), &result.rotation)) {
        return false;
    }

    result.flags |= CGDisplayIsMain(identifier) != 0 ? geometry::display_surface_main : 0U;
    result.flags |= CGDisplayIsBuiltin(identifier) != 0 ? geometry::display_surface_builtin : 0U;
    result.flags |= CGDisplayIsActive(identifier) != 0 ? geometry::display_surface_active : 0U;
    result.flags |= CGDisplayIsAsleep(identifier) != 0 ? geometry::display_surface_asleep : 0U;
    result.flags |= CGDisplayIsInMirrorSet(identifier) != 0 ? geometry::display_surface_mirrored : 0U;
    *output = result;
    return true;
}

} // namespace

SaccadeResult DisplayCollector::refresh(geometry::DisplayCatalog* catalog) noexcept {
    if (![NSThread isMainThread]) {
        return SACCADE_ERROR_STATE;
    }
    ++stats_.refresh_attempts;
    if (catalog == nullptr) {
        ++stats_.failures;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    @autoreleasepool {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        const NSUInteger count = screens.count;
        if (count == 0) {
            ++stats_.failures;
            return SACCADE_ERROR_NOT_FOUND;
        }
        if (count > geometry::display_capacity) {
            ++stats_.failures;
            return SACCADE_ERROR_CAPACITY;
        }

        std::array<geometry::DisplaySurface, geometry::display_capacity> displays{};
        for (NSUInteger index = 0; index < count; ++index) {
            if (!collect_display(screens[index], &displays[index])) {
                ++stats_.failures;
                return SACCADE_ERROR_BACKEND;
            }
        }

        const uint64_t previous_epoch = catalog->snapshot().epoch;
        const uint32_t topology_flags =
            NSScreen.screensHaveSeparateSpaces ? geometry::display_topology_separate_spaces : 0U;
        const SaccadeResult result = catalog->publish(displays.data(), static_cast<uint32_t>(count), topology_flags);
        if (result != SACCADE_OK) {
            ++stats_.failures;
            return result;
        }
        stats_.last_display_count = static_cast<uint32_t>(count);
        stats_.topology_changes += catalog->snapshot().epoch != previous_epoch ? 1U : 0U;
        return SACCADE_OK;
    }
}

SaccadeResult DisplayCollector::read_stats(DisplayCollectorStats* output) const noexcept {
    if (![NSThread isMainThread]) {
        return SACCADE_ERROR_STATE;
    }
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = stats_;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

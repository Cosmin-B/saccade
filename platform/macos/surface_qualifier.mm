#include "platform/macos/surface_qualifier.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CGSession.h>
#import <Carbon/Carbon.h>

#include <mach/mach_time.h>

namespace saccade::platform::macos {
namespace {

AccessibilityRole role_for(NSString* role) noexcept {
    if ([role isEqualToString:(NSString*)kAXWindowRole]) return AccessibilityRole::window;
    if ([role isEqualToString:(NSString*)kAXSheetRole]) return AccessibilityRole::sheet;
    if ([role isEqualToString:(NSString*)kAXTextFieldRole]) return AccessibilityRole::text_field;
    if ([role isEqualToString:(NSString*)kAXButtonRole]) return AccessibilityRole::button;
    return AccessibilityRole::other;
}

AccessibilitySubrole subrole_for(NSString* subrole) noexcept {
    if ([subrole isEqualToString:(NSString*)kAXStandardWindowSubrole]) return AccessibilitySubrole::standard_window;
    if ([subrole isEqualToString:(NSString*)kAXDialogSubrole]) return AccessibilitySubrole::dialog;
    if ([subrole isEqualToString:(NSString*)kAXSystemDialogSubrole]) return AccessibilitySubrole::system_dialog;
    if ([subrole isEqualToString:(NSString*)kAXSystemFloatingWindowSubrole]) {
        return AccessibilitySubrole::system_floating_window;
    }
    if ([subrole isEqualToString:(NSString*)kAXFloatingWindowSubrole]) return AccessibilitySubrole::floating_window;
    if ([subrole isEqualToString:(NSString*)kAXSecureTextFieldSubrole]) return AccessibilitySubrole::secure_text_field;
    return AccessibilitySubrole::other;
}

bool copy_role(AXUIElementRef element, AccessibilityActionPoint* output) noexcept {
    CFTypeRef value = nullptr;
    const AXError error = AXUIElementCopyAttributeValue(element, kAXRoleAttribute, &value);
    if (error != kAXErrorSuccess || value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
        if (value != nullptr) CFRelease(value);
        return false;
    }
    output->role = role_for((__bridge NSString*)value);
    CFRelease(value);
    value = nullptr;
    const AXError subrole_error = AXUIElementCopyAttributeValue(element, kAXSubroleAttribute, &value);
    if (subrole_error == kAXErrorSuccess) {
        if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
            if (value != nullptr) CFRelease(value);
            return false;
        }
        output->subrole = subrole_for((__bridge NSString*)value);
        CFRelease(value);
    } else if (subrole_error == kAXErrorNoValue || subrole_error == kAXErrorAttributeUnsupported) {
        if (value != nullptr) CFRelease(value);
        output->subrole = AccessibilitySubrole::other;
    } else {
        if (value != nullptr) CFRelease(value);
        return false;
    }

    value = nullptr;
    const AXError protected_error = AXUIElementCopyAttributeValue(
        element, (__bridge CFStringRef)NSAccessibilityContainsProtectedContentAttribute, &value);
    if (protected_error == kAXErrorSuccess) {
        if (value == nullptr || CFGetTypeID(value) != CFBooleanGetTypeID()) {
            if (value != nullptr) CFRelease(value);
            return false;
        }
        output->protected_content = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        CFRelease(value);
    } else if (protected_error != kAXErrorNoValue && protected_error != kAXErrorAttributeUnsupported) {
        if (value != nullptr) CFRelease(value);
        return false;
    }
    return true;
}

bool secure_point(const AccessibilityActionPoint& point) noexcept {
    return point.protected_content || point.role == AccessibilityRole::secure_text_field ||
           point.subrole == AccessibilitySubrole::secure_text_field;
}

bool system_surface(const AccessibilityActionPoint& point) noexcept {
    return point.role == AccessibilityRole::system_dialog || point.subrole == AccessibilitySubrole::system_dialog ||
           point.role == AccessibilityRole::system_floating_window ||
           point.subrole == AccessibilitySubrole::system_floating_window;
}

bool native_session(void*, SessionEvidence* output) noexcept {
    if (output == nullptr) return false;
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (session == nullptr) return false;
    const CFTypeRef console = CFDictionaryGetValue(session, kCGSessionOnConsoleKey);
    const CFTypeRef login = CFDictionaryGetValue(session, kCGSessionLoginDoneKey);
    const bool valid = console != nullptr && login != nullptr && CFGetTypeID(console) == CFBooleanGetTypeID() &&
                       CFGetTypeID(login) == CFBooleanGetTypeID();
    output->type_valid = valid;
    if (valid) {
        output->on_console = CFBooleanGetValue(static_cast<CFBooleanRef>(console));
        output->login_done = CFBooleanGetValue(static_cast<CFBooleanRef>(login));
    }
    CFRelease(session);
    return true;
}

bool native_frontmost_pid(void*, uint64_t* output) noexcept {
    if (output == nullptr) return false;
    NSRunningApplication* application = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (application == nil || application.processIdentifier <= 0) return false;
    *output = static_cast<uint64_t>(application.processIdentifier);
    return true;
}

bool native_accessibility(void*, uint64_t frontmost_pid, AccessibilityEvidence* output) noexcept {
    if (output == nullptr) return false;
    if (!AXIsProcessTrusted()) {
        output->status = AccessibilitySampleStatus::untrusted;
        return true;
    }
    AXUIElementRef application = AXUIElementCreateApplication(static_cast<pid_t>(frontmost_pid));
    if (application == nullptr) {
        output->status = AccessibilitySampleStatus::failure;
        return true;
    }
    CFTypeRef focused_value = nullptr;
    const AXError focused_error =
        AXUIElementCopyAttributeValue(application, kAXFocusedUIElementAttribute, &focused_value);
    CFRelease(application);
    if (focused_error != kAXErrorSuccess || focused_value == nullptr ||
        CFGetTypeID(focused_value) != AXUIElementGetTypeID()) {
        if (focused_value != nullptr) CFRelease(focused_value);
        output->status =
            focused_value == nullptr ? AccessibilitySampleStatus::missing : AccessibilitySampleStatus::wrong_type;
        return true;
    }
    output->status = AccessibilitySampleStatus::success;
    output->focus_pid = frontmost_pid;
    AccessibilityActionPoint focus_point{};
    if (!copy_role(static_cast<AXUIElementRef>(focused_value), &focus_point)) {
        CFRelease(focused_value);
        output->status = AccessibilitySampleStatus::wrong_type;
        return true;
    }
    output->focus_role = focus_point.role;
    output->focus_subrole = focus_point.subrole;
    output->secure_text_present = secure_point(focus_point);

    AXUIElementRef current = static_cast<AXUIElementRef>(focused_value);
    CFRetain(current);
    for (;;) {
        CFTypeRef parent_value = nullptr;
        const AXError parent_error = AXUIElementCopyAttributeValue(current, kAXParentAttribute, &parent_value);
        if (parent_error == kAXErrorNoValue || parent_error == kAXErrorAttributeUnsupported) break;
        if (parent_error != kAXErrorSuccess || parent_value == nullptr ||
            CFGetTypeID(parent_value) != AXUIElementGetTypeID()) {
            if (parent_value != nullptr) CFRelease(parent_value);
            output->status =
                parent_value == nullptr ? AccessibilitySampleStatus::missing : AccessibilitySampleStatus::wrong_type;
            break;
        }
        if (output->ancestor_count == 16) {
            output->ancestor_limit_reached = true;
            CFRelease(parent_value);
            break;
        }
        if (!copy_role(static_cast<AXUIElementRef>(parent_value), &output->ancestors[output->ancestor_count])) {
            output->status = AccessibilitySampleStatus::wrong_type;
            CFRelease(parent_value);
            break;
        }
        output->secure_text_present |= secure_point(output->ancestors[output->ancestor_count]);
        ++output->ancestor_count;
        CFRelease(current);
        current = static_cast<AXUIElementRef>(parent_value);
    }
    CFRelease(current);

    CFTypeRef children_value = nullptr;
    const AXError children_error = AXUIElementCopyAttributeValue(static_cast<AXUIElementRef>(focused_value),
                                                                 kAXChildrenAttribute, &children_value);
    if (children_error == kAXErrorSuccess && children_value != nullptr) {
        if (CFGetTypeID(children_value) != CFArrayGetTypeID()) {
            output->status = AccessibilitySampleStatus::wrong_type;
        } else {
            const CFIndex count = CFArrayGetCount(static_cast<CFArrayRef>(children_value));
            const CFIndex limit = count > 64 ? 64 : count;
            output->action_point_limit_reached = count > 64;
            for (CFIndex index = 0; index < limit; ++index) {
                const CFTypeRef child = CFArrayGetValueAtIndex(static_cast<CFArrayRef>(children_value), index);
                if (child == nullptr || CFGetTypeID(child) != AXUIElementGetTypeID() ||
                    !copy_role(static_cast<AXUIElementRef>(const_cast<CFTypeRef>(child)),
                               &output->action_points[output->action_point_count])) {
                    output->status = AccessibilitySampleStatus::wrong_type;
                    break;
                }
                const auto& point = output->action_points[output->action_point_count++];
                output->secure_text_present |= secure_point(point);
            }
        }
        CFRelease(children_value);
    } else if (children_error != kAXErrorNoValue && children_error != kAXErrorAttributeUnsupported) {
        if (children_value != nullptr) CFRelease(children_value);
        output->status = AccessibilitySampleStatus::failure;
    }
    CFRelease(focused_value);
    return true;
}

bool native_secure_input(void*, bool* output) noexcept {
    if (output == nullptr) return false;
    *output = IsSecureEventInputEnabled();
    return true;
}

bool native_time(void*, uint64_t* output) noexcept {
    if (output == nullptr) return false;
    *output = static_cast<uint64_t>(mach_absolute_time());
    return true;
}

uint32_t classify(const SessionEvidence& session, uint64_t frontmost_pid, const AccessibilityEvidence& accessibility,
                  bool secure_input) noexcept {
    uint32_t reasons = surface_reason_none;
    if (!session.type_valid) reasons |= surface_reason_wrong_type;
    if (!session.on_console || !session.login_done) reasons |= surface_reason_session_missing;
    if (frontmost_pid == 0) reasons |= surface_reason_frontmost_missing;
    switch (accessibility.status) {
    case AccessibilitySampleStatus::untrusted:
        reasons |= surface_reason_ax_untrusted;
        break;
    case AccessibilitySampleStatus::failure:
        reasons |= surface_reason_ax_failure;
        break;
    case AccessibilitySampleStatus::missing:
        reasons |= surface_reason_missing_evidence;
        break;
    case AccessibilitySampleStatus::wrong_type:
        reasons |= surface_reason_wrong_type;
        break;
    case AccessibilitySampleStatus::success:
        break;
    }
    if (accessibility.status == AccessibilitySampleStatus::success) {
        if (accessibility.focus_pid == 0 || accessibility.focus_role == AccessibilityRole::unknown ||
            accessibility.focus_subrole == AccessibilitySubrole::unknown) {
            reasons |= surface_reason_missing_evidence;
        }
        if (accessibility.focus_pid != frontmost_pid) reasons |= surface_reason_pid_mismatch;
        if (accessibility.ancestor_limit_reached || accessibility.action_point_limit_reached) {
            reasons |= surface_reason_preflight_limit;
        }
        for (uint32_t i = 0; i < accessibility.ancestor_count; ++i) {
            const auto& point = accessibility.ancestors[i];
            if (point.role == AccessibilityRole::system_dialog ||
                point.subrole == AccessibilitySubrole::system_dialog) {
                reasons |= surface_reason_system_dialog;
            }
            if (point.role == AccessibilityRole::system_floating_window ||
                point.subrole == AccessibilitySubrole::system_floating_window) {
                reasons |= surface_reason_system_floating_window;
            }
        }
        if (accessibility.focus_role == AccessibilityRole::system_dialog ||
            accessibility.focus_subrole == AccessibilitySubrole::system_dialog) {
            reasons |= surface_reason_system_dialog;
        }
        if (accessibility.focus_role == AccessibilityRole::system_floating_window ||
            accessibility.focus_subrole == AccessibilitySubrole::system_floating_window) {
            reasons |= surface_reason_system_floating_window;
        }
        for (uint32_t i = 0; i < accessibility.action_point_count; ++i) {
            if (secure_point(accessibility.action_points[i])) reasons |= surface_reason_secure_input;
        }
        for (uint32_t i = 0; i < accessibility.ancestor_count; ++i) {
            if (secure_point(accessibility.ancestors[i])) reasons |= surface_reason_secure_input;
        }
        if (accessibility.secure_text_present) reasons |= surface_reason_secure_input;
    }
    if (secure_input) reasons |= surface_reason_secure_input;
    return reasons;
}

} // namespace

ActionPointDisposition qualify_action_point(int32_t x_q8, int32_t y_q8) noexcept {
    if (!AXIsProcessTrusted()) return ActionPointDisposition::unavailable;
    AXUIElementRef system = AXUIElementCreateSystemWide();
    if (system == nullptr) return ActionPointDisposition::unavailable;
    AXUIElementRef element = nullptr;
    const AXError hit = AXUIElementCopyElementAtPosition(system, static_cast<float>(x_q8) / 256.0F,
                                                         static_cast<float>(y_q8) / 256.0F, &element);
    CFRelease(system);
    if (hit != kAXErrorSuccess || element == nullptr) {
        if (element != nullptr) CFRelease(element);
        return ActionPointDisposition::unavailable;
    }

    constexpr uint32_t ancestor_limit = 16;
    for (uint32_t depth = 0; depth < ancestor_limit; ++depth) {
        AccessibilityActionPoint point{};
        if (!copy_role(element, &point)) {
            CFRelease(element);
            return ActionPointDisposition::unavailable;
        }
        if (secure_point(point) || system_surface(point)) {
            CFRelease(element);
            return ActionPointDisposition::secure;
        }
        CFTypeRef parent = nullptr;
        const AXError parent_error = AXUIElementCopyAttributeValue(element, kAXParentAttribute, &parent);
        CFRelease(element);
        if (parent_error == kAXErrorNoValue || parent_error == kAXErrorAttributeUnsupported) {
            if (parent != nullptr) CFRelease(parent);
            return ActionPointDisposition::qualified;
        }
        if (parent_error != kAXErrorSuccess || parent == nullptr || CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent != nullptr) CFRelease(parent);
            return ActionPointDisposition::unavailable;
        }
        element = static_cast<AXUIElementRef>(parent);
    }
    CFRelease(element);
    return ActionPointDisposition::unavailable;
}

SurfaceQualifier::SurfaceQualifier(const SurfaceQualifierProbe& probe) noexcept
    : probe_(probe), owner_thread_(current_thread()) {}

pthread_t SurfaceQualifier::current_thread() noexcept {
    return pthread_self();
}

bool SurfaceQualifier::check_owner() const noexcept {
    return pthread_equal(owner_thread_, current_thread()) != 0;
}

bool SurfaceQualifier::on_owner_thread() const noexcept {
    return check_owner();
}

const SurfaceQualifierSnapshot& SurfaceQualifier::cached() const noexcept {
    return snapshot_;
}

uint32_t SurfaceQualifier::classify_preflight(const AccessibilityEvidence& evidence) noexcept {
    SessionEvidence session{};
    session.type_valid = true;
    session.on_console = true;
    session.login_done = true;
    return classify(session, evidence.focus_pid, evidence, false);
}

bool SurfaceQualifier::refresh() noexcept {
    if (!check_owner()) {
        snapshot_.disposition = SurfaceDisposition::blocked;
        snapshot_.reason_bits = surface_reason_owner_thread;
        return false;
    }
    SessionEvidence session{};
    uint64_t frontmost_pid = 0;
    AccessibilityEvidence accessibility{};
    bool secure_input = false;
    uint64_t sampled_time = 0;
    const bool sampled =
        (probe_.sample_session == nullptr ? native_session(nullptr, &session)
                                          : probe_.sample_session(probe_.context, &session)) &&
        (probe_.sample_frontmost_pid == nullptr ? native_frontmost_pid(nullptr, &frontmost_pid)
                                                : probe_.sample_frontmost_pid(probe_.context, &frontmost_pid)) &&
        (probe_.sample_accessibility == nullptr
             ? native_accessibility(nullptr, frontmost_pid, &accessibility)
             : probe_.sample_accessibility(probe_.context, frontmost_pid, &accessibility)) &&
        (probe_.sample_secure_input == nullptr ? native_secure_input(nullptr, &secure_input)
                                               : probe_.sample_secure_input(probe_.context, &secure_input));
    const bool sampled_clock = probe_.sample_time == nullptr ? native_time(nullptr, &sampled_time)
                                                             : probe_.sample_time(probe_.context, &sampled_time);
    ++snapshot_.epoch;
    snapshot_.sampled_time = sampled_time;
    snapshot_.focus_pid = accessibility.focus_pid;
    if (!sampled || !sampled_clock) {
        snapshot_.disposition = SurfaceDisposition::blocked;
        snapshot_.reason_bits = surface_reason_probe_failure;
        return false;
    }
    snapshot_.reason_bits = classify(session, frontmost_pid, accessibility, secure_input);
    snapshot_.disposition =
        snapshot_.reason_bits == surface_reason_none ? SurfaceDisposition::qualified : SurfaceDisposition::blocked;
    return snapshot_.disposition == SurfaceDisposition::qualified;
}

void SurfaceQualifier::invalidate() noexcept {
    if (!check_owner()) return;
    snapshot_.disposition = SurfaceDisposition::unknown;
    snapshot_.reason_bits = surface_reason_none;
}

void SurfaceQualifier::notification_invalidated() noexcept {
    invalidate();
}

} // namespace saccade::platform::macos

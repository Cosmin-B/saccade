#include "platform/macos/accessibility_provider.hpp"

#include "core/cache_line.hpp"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace saccade::platform::macos {
namespace {

constexpr uint32_t maximum_windows = 256;
constexpr uint32_t maximum_pending_elements = SACCADE_TARGET_PACKET_MAX_TARGETS;
constexpr uint32_t maximum_visited_elements = 100000;
constexpr uint32_t maximum_depth = 128;
constexpr size_t window_title_capacity = 256;
constexpr size_t identifier_capacity = 256;
constexpr double coordinate_scale = 256.0;
constexpr float ax_messaging_timeout_seconds = 0.5F;
constexpr uint64_t snapshot_handle_bit = UINT64_C(1) << 63;
constexpr uint64_t ticket_handle_limit = snapshot_handle_bit;
constexpr size_t packet_cache_alignment = 64;
constexpr uint64_t provider_id = UINT64_C(0x4D41434158000001);
constexpr char provider_name[] = "macOS Accessibility";

enum class WorkState : uint32_t { idle, queued, running, complete, cancelled, failed, stopping };

enum AttributeIndex : CFIndex {
    role_attribute,
    subrole_attribute,
    position_attribute,
    size_attribute,
    enabled_attribute,
    hidden_attribute,
    identifier_attribute,
    title_attribute,
    description_attribute,
    attribute_count
};

struct WindowEntry {
    uint64_t id = 0;
    int32_t process_id = 0;
    SaccadeRectI32 bounds{};
    std::array<uint8_t, window_title_capacity> title{};
    uint32_t title_size = 0;
};

struct TraversalEntry {
    AXUIElementRef element = nullptr;
    uint64_t parent_id = 0;
    uint32_t depth = 0;
    uint32_t reserved = 0;
};

constexpr size_t target_bytes = static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
constexpr size_t traversal_bytes = static_cast<size_t>(maximum_pending_elements) * sizeof(TraversalEntry);
constexpr size_t arena_bytes = target_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES + traversal_bytes;

template <typename Structure> SaccadeResult write_structure(Structure* destination, Structure value) noexcept {
    if (destination == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t size = 0;
    uint32_t version = 0;
    std::memcpy(&size, destination, sizeof(size));
    std::memcpy(&version, reinterpret_cast<const uint8_t*>(destination) + offsetof(Structure, api_version),
                sizeof(version));
    if (size < offsetof(Structure, reserved)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if ((version >> 16U) != (SACCADE_API_VERSION >> 16U)) return SACCADE_ERROR_VERSION;
    const size_t copy_size = std::min<size_t>(size, sizeof(value));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(destination, &value, copy_size);
    return SACCADE_OK;
}

bool number(CFDictionaryRef dictionary, CFStringRef key, int64_t* output) noexcept {
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(value, kCFNumberSInt64Type, output);
}

bool dictionary_window(CFDictionaryRef dictionary, WindowEntry* output) noexcept {
    int64_t identifier = 0;
    int64_t process = 0;
    int64_t layer = 0;
    const auto bounds_value = static_cast<CFDictionaryRef>(CFDictionaryGetValue(dictionary, kCGWindowBounds));
    CGRect bounds{};
    if (output == nullptr || !number(dictionary, kCGWindowNumber, &identifier) ||
        !number(dictionary, kCGWindowOwnerPID, &process) || !number(dictionary, kCGWindowLayer, &layer) ||
        identifier <= 0 || process <= 0 || layer != 0 || process == getpid() || bounds_value == nullptr ||
        CFGetTypeID(bounds_value) != CFDictionaryGetTypeID() ||
        !CGRectMakeWithDictionaryRepresentation(bounds_value, &bounds) || !std::isfinite(bounds.origin.x) ||
        !std::isfinite(bounds.origin.y) || !std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width <= 0 || bounds.size.height <= 0 || bounds.origin.x < INT32_MIN ||
        bounds.origin.x > INT32_MAX || bounds.origin.y < INT32_MIN || bounds.origin.y > INT32_MAX ||
        bounds.size.width > INT32_MAX || bounds.size.height > INT32_MAX) {
        return false;
    }
    WindowEntry entry{};
    entry.id = static_cast<uint64_t>(identifier);
    entry.process_id = static_cast<int32_t>(process);
    entry.bounds = {
        static_cast<int32_t>(std::llround(bounds.origin.x)), static_cast<int32_t>(std::llround(bounds.origin.y)),
        static_cast<int32_t>(std::llround(bounds.size.width)), static_cast<int32_t>(std::llround(bounds.size.height))};
    const auto title = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, kCGWindowName));
    if (title != nullptr && CFGetTypeID(title) == CFStringGetTypeID() &&
        CFStringGetCString(title, reinterpret_cast<char*>(entry.title.data()), static_cast<CFIndex>(entry.title.size()),
                           kCFStringEncodingUTF8)) {
        entry.title_size = static_cast<uint32_t>(std::strlen(reinterpret_cast<const char*>(entry.title.data())));
    }
    *output = entry;
    return true;
}

bool ax_point(CFTypeRef value, CGPoint* output) noexcept {
    return value != nullptr && CFGetTypeID(value) == AXValueGetTypeID() &&
           AXValueGetType(static_cast<AXValueRef>(value)) == kAXValueTypeCGPoint &&
           AXValueGetValue(static_cast<AXValueRef>(value), kAXValueTypeCGPoint, output);
}

bool ax_size(CFTypeRef value, CGSize* output) noexcept {
    return value != nullptr && CFGetTypeID(value) == AXValueGetTypeID() &&
           AXValueGetType(static_cast<AXValueRef>(value)) == kAXValueTypeCGSize &&
           AXValueGetValue(static_cast<AXValueRef>(value), kAXValueTypeCGSize, output);
}

bool ax_bool(CFTypeRef value, bool fallback) noexcept {
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID()
               ? CFBooleanGetValue(static_cast<CFBooleanRef>(value))
               : fallback;
}

bool ax_geometry(AXUIElementRef element, CGRect* output) noexcept {
    CFTypeRef position_value = nullptr;
    CFTypeRef size_value = nullptr;
    const AXError position_error = AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value);
    const AXError size_error = AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value);
    CGPoint position{};
    CGSize size{};
    const bool valid = position_error == kAXErrorSuccess && size_error == kAXErrorSuccess &&
                       ax_point(position_value, &position) && ax_size(size_value, &size) && std::isfinite(position.x) &&
                       std::isfinite(position.y) && std::isfinite(size.width) && std::isfinite(size.height) &&
                       size.width > 0 && size.height > 0;
    if (position_value != nullptr) CFRelease(position_value);
    if (size_value != nullptr) CFRelease(size_value);
    if (valid) *output = {position, size};
    return valid;
}

uint32_t ax_title(AXUIElementRef element, std::array<uint8_t, window_title_capacity>* output) noexcept {
    CFTypeRef value = nullptr;
    const AXError error = AXUIElementCopyAttributeValue(element, kAXTitleAttribute, &value);
    if (error != kAXErrorSuccess || value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
        if (value != nullptr) CFRelease(value);
        return 0;
    }

    const bool encoded = CFStringGetCString(static_cast<CFStringRef>(value), reinterpret_cast<char*>(output->data()),
                                            static_cast<CFIndex>(output->size()), kCFStringEncodingUTF8);
    CFRelease(value);
    return encoded ? static_cast<uint32_t>(std::strlen(reinterpret_cast<const char*>(output->data()))) : 0;
}

SaccadeTargetRole role_for(CFStringRef role) noexcept {
    if (role == nullptr) return SACCADE_TARGET_ROLE_UNKNOWN;
    if (CFEqual(role, kAXButtonRole) || CFEqual(role, kAXPopUpButtonRole) || CFEqual(role, kAXMenuButtonRole))
        return SACCADE_TARGET_ROLE_BUTTON;
    if (CFEqual(role, CFSTR("AXLink"))) return SACCADE_TARGET_ROLE_LINK;
    if (CFEqual(role, kAXStaticTextRole)) return SACCADE_TARGET_ROLE_TEXT;
    if (CFEqual(role, kAXTextFieldRole) || CFEqual(role, kAXTextAreaRole)) return SACCADE_TARGET_ROLE_TEXT_FIELD;
    if (CFEqual(role, kAXCheckBoxRole)) return SACCADE_TARGET_ROLE_CHECKBOX;
    if (CFEqual(role, kAXRadioButtonRole)) return SACCADE_TARGET_ROLE_RADIO;
    if (CFEqual(role, kAXMenuItemRole)) return SACCADE_TARGET_ROLE_MENU_ITEM;
    if (CFEqual(role, kAXSliderRole)) return SACCADE_TARGET_ROLE_SLIDER;
    if (CFEqual(role, kAXImageRole)) return SACCADE_TARGET_ROLE_IMAGE;
    if (CFEqual(role, kAXWindowRole)) return SACCADE_TARGET_ROLE_WINDOW;
    return SACCADE_TARGET_ROLE_UNKNOWN;
}

bool role_scrolls(CFStringRef role) noexcept {
    return role != nullptr && CFGetTypeID(role) == CFStringGetTypeID() &&
           (CFEqual(role, kAXScrollAreaRole) || CFEqual(role, kAXListRole) || CFEqual(role, kAXTableRole) ||
            CFEqual(role, kAXOutlineRole));
}

uint64_t mix(uint64_t hash, uint64_t value) noexcept {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

uint64_t element_id(AXUIElementRef element, CFStringRef identifier, uint64_t window_id) noexcept {
    uint64_t hash = mix(UINT64_C(1469598103934665603), window_id);
    hash = mix(hash, static_cast<uint64_t>(CFHash(element)));
    if (identifier != nullptr) {
        std::array<char, identifier_capacity> text{};
        if (CFStringGetCString(identifier, text.data(), static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8)) {
            for (const char character : text) {
                const auto value = static_cast<unsigned char>(character);
                if (value == 0) break;
                hash = mix(hash, value);
            }
        }
    }
    return hash == 0 ? 1 : hash;
}

int32_t q8(CGFloat value) noexcept {
    constexpr double minimum = static_cast<double>(INT32_MIN) / coordinate_scale;
    constexpr double maximum = static_cast<double>(INT32_MAX) / coordinate_scale;
    return static_cast<int32_t>(
        std::llround(std::clamp(static_cast<double>(value), minimum, maximum) * coordinate_scale));
}

bool intersects(const SaccadeRectI32& scope, const CGRect& rect) noexcept {
    const double right = static_cast<double>(scope.x) + scope.width;
    const double bottom = static_cast<double>(scope.y) + scope.height;
    return CGRectGetMaxX(rect) > scope.x && CGRectGetMaxY(rect) > scope.y && CGRectGetMinX(rect) < right &&
           CGRectGetMinY(rect) < bottom;
}

uint64_t display_at(CGPoint point) noexcept {
    std::array<CGDirectDisplayID, 1> displays{};
    uint32_t count = 0;
    return CGGetDisplaysWithPoint(point, static_cast<uint32_t>(displays.size()), displays.data(), &count) ==
                       kCGErrorSuccess &&
                   count != 0
               ? displays[0]
               : 0;
}

uint32_t action_capabilities(AXUIElementRef element, CFStringRef role, SaccadeTargetRole mapped_role) noexcept {
    uint32_t bits = 0;
    if (mapped_role != SACCADE_TARGET_ROLE_UNKNOWN || role_scrolls(role)) {
        bits |= SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    }
    CFArrayRef actions = nullptr;
    if (AXUIElementCopyActionNames(element, &actions) == kAXErrorSuccess && actions != nullptr) {
        const CFIndex count = CFArrayGetCount(actions);
        for (CFIndex index = 0; index < count; ++index) {
            const auto action = static_cast<CFStringRef>(CFArrayGetValueAtIndex(actions, index));
            if (CFEqual(action, kAXPressAction) || CFEqual(action, kAXShowMenuAction)) {
                bits |= SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_INVOKE |
                        SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
            }
        }
        CFRelease(actions);
    }
    Boolean settable = false;
    if ((mapped_role == SACCADE_TARGET_ROLE_TEXT_FIELD || mapped_role == SACCADE_TARGET_ROLE_SLIDER) &&
        AXUIElementIsAttributeSettable(element, kAXValueAttribute, &settable) == kAXErrorSuccess && settable) {
        bits |= SACCADE_TARGET_CAPABILITY_TEXT;
    }
    if (mapped_role == SACCADE_TARGET_ROLE_TEXT || mapped_role == SACCADE_TARGET_ROLE_TEXT_FIELD) {
        bits |= SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    }
    if (role_scrolls(role)) bits |= SACCADE_TARGET_CAPABILITY_SCROLL;
    if (mapped_role == SACCADE_TARGET_ROLE_WINDOW) bits |= SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
    return bits;
}

mach_timespec_t timeout_from_ns(uint64_t timeout_ns) noexcept {
    mach_timespec_t timeout{};
    timeout.tv_sec = static_cast<unsigned int>(std::min<uint64_t>(timeout_ns / UINT64_C(1000000000), UINT_MAX));
    timeout.tv_nsec = static_cast<clock_res_t>(timeout_ns % UINT64_C(1000000000));
    return timeout;
}

} // namespace

struct AccessibilityProvider::Impl {
    alignas(core::destructive_interference_size) std::atomic<WorkState> state_{WorkState::idle};
    alignas(core::destructive_interference_size) std::atomic<bool> cancel_requested_{false};
    alignas(core::destructive_interference_size) std::atomic<bool> stopping_{false};
    semaphore_t request_semaphore_ = SEMAPHORE_NULL;
    semaphore_t done_semaphore_ = SEMAPHORE_NULL;
    pthread_t worker_{};
    bool worker_active_ = false;
    void* arena_ = MAP_FAILED;
    SaccadeTargetRecord* targets_ = nullptr;
    uint8_t* text_ = nullptr;
    TraversalEntry* traversal_ = nullptr;
    SaccadeAccessibilityQueryDesc query_{};
    WindowEntry query_window_{};
    SaccadeTicketHandle ticket_ = 0;
    SaccadeSnapshotHandle snapshot_ = 0;
    uint64_t next_ticket_ = 1;
    SaccadeResult result_ = SACCADE_OK;
    uint32_t target_count_ = 0;
    uint32_t text_size_ = 0;
    uint32_t packet_size_ = 0;
    uint32_t packet_flags_ = 0;
    int32_t native_error_ = 0;
    AccessibilityProviderStats stats_{};
    std::array<WindowEntry, maximum_windows> windows_{};
    uint32_t window_count_ = 0;
    alignas(packet_cache_alignment) SaccadeTargetPacketHeader header_{};

    void append_text(CFStringRef value, bool secure, SaccadeTargetRecord* target) noexcept {
        if (secure) {
            target->flags |= SACCADE_TARGET_TEXT_REDACTED;
            return;
        }
        if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) return;
        const CFIndex length = CFStringGetLength(value);
        if (length == 0) return;
        CFIndex required = 0;
        const CFIndex converted =
            CFStringGetBytes(value, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false, nullptr, 0, &required);
        if (converted != length || required <= 0) return;
        if (static_cast<uint64_t>(required) > SACCADE_TARGET_PACKET_MAX_TEXT_BYTES - text_size_) {
            target->flags |= SACCADE_TARGET_TEXT_TRUNCATED;
            packet_flags_ |= SACCADE_TARGET_PACKET_TEXT_TRUNCATED;
            return;
        }
        CFIndex written = 0;
        const CFIndex encoded = CFStringGetBytes(value, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false,
                                                 text_ + text_size_, required, &written);
        bool contains_nul = false;
        for (CFIndex index = 0; index < written; ++index)
            contains_nul |= text_[text_size_ + index] == 0;
        if (encoded != length || written != required || contains_nul) return;
        target->text = {static_cast<uint16_t>(text_size_), static_cast<uint16_t>(written)};
        text_size_ += static_cast<uint32_t>(written);
    }

    static void* worker_entry(void* context) noexcept {
        static_cast<Impl*>(context)->worker_loop();
        return nullptr;
    }

    void worker_loop() noexcept {
        const void* attribute_values[] = {kAXRoleAttribute,       kAXSubroleAttribute, kAXPositionAttribute,
                                          kAXSizeAttribute,       kAXEnabledAttribute, kAXHiddenAttribute,
                                          kAXIdentifierAttribute, kAXTitleAttribute,   kAXDescriptionAttribute};
        static_assert(std::size(attribute_values) == attribute_count);
        CFArrayRef attributes =
            CFArrayCreate(kCFAllocatorDefault, attribute_values, static_cast<CFIndex>(std::size(attribute_values)),
                          &kCFTypeArrayCallBacks);
        for (;;) {
            if (semaphore_wait(request_semaphore_) != KERN_SUCCESS) continue;
            if (stopping_.load(std::memory_order_acquire)) break;
            if (state_.load(std::memory_order_acquire) != WorkState::queued) continue;
            state_.store(WorkState::running, std::memory_order_release);
            if (attributes == nullptr) {
                result_ = SACCADE_ERROR_BACKEND;
                ++stats_.failed;
                state_.store(WorkState::failed, std::memory_order_release);
            } else {
                run_query(attributes);
            }
            semaphore_signal(done_semaphore_);
        }
        if (attributes != nullptr) CFRelease(attributes);
    }

    AXUIElementRef find_window() noexcept {
        AXUIElementRef application = AXUIElementCreateApplication(query_window_.process_id);
        if (application == nullptr) return nullptr;
        AXUIElementSetMessagingTimeout(application, ax_messaging_timeout_seconds);
        CFTypeRef windows_value = nullptr;
        const AXError error = AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &windows_value);
        if (error != kAXErrorSuccess || windows_value == nullptr || CFGetTypeID(windows_value) != CFArrayGetTypeID()) {
            native_error_ = error;
            if (windows_value != nullptr) CFRelease(windows_value);
            CFRelease(application);
            return nullptr;
        }
        const auto windows = static_cast<CFArrayRef>(windows_value);
        AXUIElementRef best = nullptr;
        double best_distance = std::numeric_limits<double>::max();
        const CFIndex count = CFArrayGetCount(windows);
        for (CFIndex index = 0; index < count; ++index) {
            const auto candidate =
                static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(windows, index)));
            CGRect bounds{};
            if (!ax_geometry(candidate, &bounds)) continue;
            const double distance = std::abs(bounds.origin.x - query_window_.bounds.x) +
                                    std::abs(bounds.origin.y - query_window_.bounds.y) +
                                    std::abs(bounds.size.width - query_window_.bounds.width) +
                                    std::abs(bounds.size.height - query_window_.bounds.height);
            if (distance < best_distance) {
                best_distance = distance;
                best = candidate;
            }
        }
        if (best != nullptr) CFRetain(best);
        CFRelease(windows);
        CFRelease(application);
        return best;
    }

    bool append_target(AXUIElementRef element, uint64_t parent_id, CFArrayRef attributes) noexcept {
        CFArrayRef values = nullptr;
        const AXError error = AXUIElementCopyMultipleAttributeValues(
            element, attributes, static_cast<AXCopyMultipleAttributeOptions>(0), &values);
        if (error != kAXErrorSuccess || values == nullptr || CFArrayGetCount(values) < attribute_count) {
            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
            native_error_ = error;
            if (values != nullptr) CFRelease(values);
            return false;
        }
        const auto role = static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, role_attribute));
        const auto subrole = static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, subrole_attribute));
        const CFTypeRef position_value = CFArrayGetValueAtIndex(values, position_attribute);
        const CFTypeRef size_value = CFArrayGetValueAtIndex(values, size_attribute);
        const CFTypeRef enabled_value = CFArrayGetValueAtIndex(values, enabled_attribute);
        const CFTypeRef hidden_value = CFArrayGetValueAtIndex(values, hidden_attribute);
        const auto identifier = static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, identifier_attribute));
        const auto title = static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, title_attribute));
        const auto description = static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, description_attribute));
        CGPoint position{};
        CGSize size{};
        const bool hidden = ax_bool(hidden_value, false);
        const SaccadeTargetRole mapped_role =
            role != nullptr && CFGetTypeID(role) == CFStringGetTypeID() ? role_for(role) : SACCADE_TARGET_ROLE_UNKNOWN;
        if (hidden || !ax_point(position_value, &position) || !ax_size(size_value, &size) || size.width <= 0 ||
            size.height <= 0) {
            CFRelease(values);
            return false;
        }
        const CGRect bounds{position, size};
        if (!intersects(query_.scope, bounds)) {
            CFRelease(values);
            return false;
        }
        uint32_t capabilities = action_capabilities(element, role, mapped_role);
        const bool enabled = ax_bool(enabled_value, true);
        const bool secure = subrole != nullptr && CFGetTypeID(subrole) == CFStringGetTypeID() &&
                            CFEqual(subrole, kAXSecureTextFieldSubrole);
        if (capabilities == 0 && !secure && enabled) {
            CFRelease(values);
            return false;
        }
        SaccadeTargetRecord& target = targets_[target_count_];
        target = {};
        target.target_id = element_id(
            element, identifier != nullptr && CFGetTypeID(identifier) == CFStringGetTypeID() ? identifier : nullptr,
            query_.window_id);
        target.parent_id = parent_id;
        target.window_id = query_.window_id;
        target.display_id = display_at({CGRectGetMidX(bounds), CGRectGetMidY(bounds)});
        target.x_q8 = q8(bounds.origin.x);
        target.y_q8 = q8(bounds.origin.y);
        target.width_q8 = q8(bounds.size.width);
        target.height_q8 = q8(bounds.size.height);
        if (target.width_q8 <= 0 || target.height_q8 <= 0) {
            CFRelease(values);
            return false;
        }
        const int64_t safe_x = static_cast<int64_t>(target.x_q8) + target.width_q8 / 2;
        const int64_t safe_y = static_cast<int64_t>(target.y_q8) + target.height_q8 / 2;
        if (safe_x < INT32_MIN || safe_x > INT32_MAX || safe_y < INT32_MIN || safe_y > INT32_MAX) {
            CFRelease(values);
            return false;
        }
        target.safe_x_q8 = static_cast<int32_t>(safe_x);
        target.safe_y_q8 = static_cast<int32_t>(safe_y);
        target.confidence_q16 = UINT16_MAX;
        target.role = mapped_role;
        target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
        target.capability_bits = capabilities;
        target.flags = SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_APPROXIMATE;
        if (!enabled || secure) {
            target.capability_bits = 0;
            target.flags &= ~static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE);
            target.flags |= !enabled ? SACCADE_TARGET_DISABLED : 0;
            target.flags |= secure ? SACCADE_TARGET_SECURE : 0;
        }
        target.order = target_count_;
        const CFStringRef text =
            title != nullptr && CFGetTypeID(title) == CFStringGetTypeID() && CFStringGetLength(title) != 0
                ? title
                : description;
        append_text(text, secure, &target);
        ++target_count_;
        CFRelease(values);
        return true;
    }

    void clear_traversal(uint32_t count) noexcept {
        for (uint32_t index = 0; index < count; ++index) {
            if (traversal_[index].element != nullptr) {
                CFRelease(traversal_[index].element);
                traversal_[index].element = nullptr;
            }
        }
    }

    void run_query(CFArrayRef attributes) noexcept {
        target_count_ = 0;
        text_size_ = 0;
        packet_size_ = 0;
        packet_flags_ = 0;
        native_error_ = 0;
        result_ = SACCADE_OK;
        if (!AXIsProcessTrusted()) {
            result_ = SACCADE_ERROR_PERMISSION;
            native_error_ = kAXErrorAPIDisabled;
            ++stats_.failed;
            state_.store(WorkState::failed, std::memory_order_release);
            return;
        }
        if (cancel_requested_.load(std::memory_order_acquire)) {
            result_ = SACCADE_ERROR_CANCELLED;
            ++stats_.cancelled;
            state_.store(WorkState::cancelled, std::memory_order_release);
            return;
        }
        AXUIElementRef root = find_window();
        if (root == nullptr) {
            result_ = native_error_ == kAXErrorAPIDisabled ? SACCADE_ERROR_PERMISSION : SACCADE_ERROR_NOT_FOUND;
            ++stats_.failed;
            state_.store(WorkState::failed, std::memory_order_release);
            return;
        }

        uint32_t stack_count = 1;
        uint32_t visited = 0;
        traversal_[0] = {root, query_.window_id, 0, 0};
        const uint32_t capacity = std::min(query_.target_capacity, SACCADE_TARGET_PACKET_MAX_TARGETS);
        while (stack_count != 0 && target_count_ < capacity && visited < maximum_visited_elements) {
            if (cancel_requested_.load(std::memory_order_acquire)) {
                clear_traversal(stack_count);
                result_ = SACCADE_ERROR_CANCELLED;
                ++stats_.cancelled;
                state_.store(WorkState::cancelled, std::memory_order_release);
                return;
            }
            TraversalEntry entry = traversal_[--stack_count];
            traversal_[stack_count].element = nullptr;
            ++visited;
            const uint64_t current_id = element_id(entry.element, nullptr, query_.window_id);
            (void)append_target(entry.element, entry.parent_id, attributes);

            if (entry.depth < maximum_depth) {
                CFTypeRef children_value = nullptr;
                const AXError children_error =
                    AXUIElementCopyAttributeValue(entry.element, kAXChildrenAttribute, &children_value);
                if (children_error == kAXErrorSuccess && children_value != nullptr &&
                    CFGetTypeID(children_value) == CFArrayGetTypeID()) {
                    const auto children = static_cast<CFArrayRef>(children_value);
                    const CFIndex child_count = CFArrayGetCount(children);
                    for (CFIndex index = child_count; index > 0; --index) {
                        if (stack_count == maximum_pending_elements) {
                            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                            break;
                        }
                        const auto child =
                            static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(children, index - 1)));
                        if (child == nullptr || CFGetTypeID(child) != AXUIElementGetTypeID()) {
                            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                            continue;
                        }
                        CFRetain(child);
                        traversal_[stack_count++] = {child, current_id, entry.depth + 1U, 0};
                    }
                } else if (children_error != kAXErrorNoValue && children_error != kAXErrorAttributeUnsupported) {
                    packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                    native_error_ = children_error;
                }
                if (children_value != nullptr) CFRelease(children_value);
            } else {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
            }
            CFRelease(entry.element);
        }
        stats_.elements += visited;
        if (stack_count != 0 || visited == maximum_visited_elements || target_count_ == capacity) {
            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
        }
        clear_traversal(stack_count);

        header_ = {};
        header_.struct_size = sizeof(header_);
        header_.packet_version = SACCADE_TARGET_PACKET_VERSION;
        header_.target_count = target_count_;
        header_.target_stride = sizeof(SaccadeTargetRecord);
        header_.flags = packet_flags_;
        header_.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
        header_.scene_epoch = ticket_;
        header_.frame_id = query_.frame_id;
        header_.model_epoch = provider_id;
        header_.session_epoch = query_.session_epoch;
        header_.transform_epoch = query_.transform_epoch;
        header_.topology_epoch = query_.topology_epoch;
        header_.source_id = query_.window_id;
        header_.targets_offset = sizeof(header_);
        packet_size_ = static_cast<uint32_t>(
            sizeof(header_) + static_cast<size_t>(target_count_) * sizeof(SaccadeTargetRecord) + text_size_);
        header_.total_size = packet_size_;
        snapshot_ = ticket_ | snapshot_handle_bit;
        ++stats_.completed;
        stats_.targets += target_count_;
        stats_.incomplete += packet_flags_ != 0 ? 1U : 0U;
        state_.store(WorkState::complete, std::memory_order_release);
    }

    void append_minimized_windows(int32_t process_id, CFArrayRef descriptions) noexcept {
        AXUIElementRef application = AXUIElementCreateApplication(process_id);
        if (application == nullptr) return;

        AXUIElementSetMessagingTimeout(application, ax_messaging_timeout_seconds);
        CFTypeRef values = nullptr;
        const AXError copied = AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &values);
        if (copied == kAXErrorSuccess && values != nullptr && CFGetTypeID(values) == CFArrayGetTypeID()) {
            const auto ax_windows = static_cast<CFArrayRef>(values);
            const CFIndex description_count = CFArrayGetCount(descriptions);
            for (CFIndex ax_index = 0; ax_index < CFArrayGetCount(ax_windows) && window_count_ < maximum_windows;
                 ++ax_index) {
                const auto ax_window =
                    static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(ax_windows, ax_index)));
                CFTypeRef minimized_value = nullptr;
                const AXError minimized_error =
                    AXUIElementCopyAttributeValue(ax_window, kAXMinimizedAttribute, &minimized_value);
                const bool minimized = minimized_error == kAXErrorSuccess && ax_bool(minimized_value, false);
                if (minimized_value != nullptr) CFRelease(minimized_value);

                CGRect ax_bounds{};
                if (!minimized || !ax_geometry(ax_window, &ax_bounds)) continue;

                std::array<uint8_t, window_title_capacity> title{};
                const uint32_t title_size = ax_title(ax_window, &title);
                WindowEntry match{};
                uint32_t match_count = 0;
                for (CFIndex index = 0; index < description_count; ++index) {
                    const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(descriptions, index));
                    WindowEntry candidate{};
                    if (!dictionary_window(dictionary, &candidate) || candidate.process_id != process_id ||
                        find_window_entry(candidate.id) != nullptr) {
                        continue;
                    }

                    const bool title_matches = title_size == 0 || candidate.title_size == 0 ||
                                               (title_size == candidate.title_size &&
                                                std::memcmp(title.data(), candidate.title.data(), title_size) == 0);
                    const double distance = std::abs(ax_bounds.origin.x - candidate.bounds.x) +
                                            std::abs(ax_bounds.origin.y - candidate.bounds.y) +
                                            std::abs(ax_bounds.size.width - candidate.bounds.width) +
                                            std::abs(ax_bounds.size.height - candidate.bounds.height);
                    if (!title_matches || distance >= 4.0) continue;
                    match = candidate;
                    ++match_count;
                }
                if (match_count == 1) windows_[window_count_++] = match;
            }
        }
        if (values != nullptr) CFRelease(values);
        CFRelease(application);
    }

    SaccadeResult refresh_windows() noexcept {
        CFArrayRef descriptions = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (descriptions == nullptr) return SACCADE_ERROR_BACKEND;
        window_count_ = 0;
        const CFIndex count = CFArrayGetCount(descriptions);
        for (CFIndex index = 0; index < count && window_count_ < maximum_windows; ++index) {
            const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(descriptions, index));
            WindowEntry entry{};
            if (dictionary_window(dictionary, &entry)) windows_[window_count_++] = entry;
        }
        CFRelease(descriptions);

        const uint32_t visible_count = window_count_;
        // Public APIs expose current-Space normal windows but no Space identifier for minimized windows. Use the
        // all-window list only to identify AX windows that explicitly report themselves minimized.
        CFArrayRef all_descriptions =
            CGWindowListCopyWindowInfo(kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (all_descriptions != nullptr) {
            std::array<int32_t, maximum_windows> processes{};
            uint32_t process_count = 0;
            for (uint32_t visible = 0; visible < visible_count; ++visible) {
                const int32_t process_id = windows_[visible].process_id;
                bool unique = true;
                for (uint32_t previous = 0; previous < process_count; ++previous)
                    unique &= processes[previous] != process_id;
                if (unique) processes[process_count++] = process_id;
            }

            const CFIndex description_count = CFArrayGetCount(all_descriptions);
            for (CFIndex index = 0; index < description_count && process_count < maximum_windows; ++index) {
                const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(all_descriptions, index));
                WindowEntry candidate{};
                if (!dictionary_window(dictionary, &candidate)) continue;

                bool unique = true;
                for (uint32_t previous = 0; previous < process_count; ++previous)
                    unique &= processes[previous] != candidate.process_id;
                if (unique) processes[process_count++] = candidate.process_id;
            }

            for (uint32_t index = 0; index < process_count && window_count_ < maximum_windows; ++index)
                append_minimized_windows(processes[index], all_descriptions);
            CFRelease(all_descriptions);
        }
        ++stats_.window_refreshes;
        return window_count_ == 0 ? SACCADE_ERROR_NOT_FOUND : SACCADE_OK;
    }

    WindowEntry* find_window_entry(uint64_t id) noexcept {
        for (uint32_t index = 0; index < window_count_; ++index) {
            if (windows_[index].id == id) return &windows_[index];
        }
        return nullptr;
    }

    SaccadeAccessibilityStatus status() const noexcept {
        SaccadeAccessibilityStatus output{};
        output.ticket = ticket_;
        output.session_epoch = query_.session_epoch;
        output.transform_epoch = query_.transform_epoch;
        output.topology_epoch = query_.topology_epoch;
        output.frame_id = query_.frame_id;
        output.target_count = target_count_;
        output.required_bytes = packet_size_;
        switch (state_.load(std::memory_order_acquire)) {
        case WorkState::queued:
            output.state = SACCADE_TICKET_QUEUED;
            break;
        case WorkState::running:
            output.state = SACCADE_TICKET_RUNNING;
            break;
        case WorkState::complete:
            output.state = SACCADE_TICKET_COMPLETE;
            output.snapshot = snapshot_;
            break;
        case WorkState::cancelled:
            output.state = SACCADE_TICKET_CANCELLED;
            output.result = SACCADE_ERROR_CANCELLED;
            break;
        case WorkState::failed:
            output.state = SACCADE_TICKET_FAILED;
            output.result = result_;
            break;
        default:
            output.state = SACCADE_TICKET_FAILED;
            output.result = SACCADE_ERROR_STATE;
            break;
        }
        return output;
    }

    static Impl* from(void* context) noexcept { return static_cast<Impl*>(context); }

    static SaccadeResult SACCADE_CALL enumerate(void* context, uint32_t index, SaccadeWindowInfo* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (index == 0) {
            const SaccadeResult refreshed = state->refresh_windows();
            if (refreshed != SACCADE_OK) return refreshed;
        }
        if (index >= state->window_count_) return SACCADE_ERROR_NOT_FOUND;
        const WindowEntry& entry = state->windows_[index];
        SaccadeWindowInfo value{};
        value.stable_id = entry.id;
        value.process_id = static_cast<uint64_t>(entry.process_id);
        value.desktop_bounds = entry.bounds;
        value.title = {entry.title.data(), entry.title_size};
        return write_structure(output, value);
    }

    static SaccadeResult SACCADE_CALL request(void* context, const SaccadeAccessibilityQueryDesc* query,
                                              SaccadeTicketHandle* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || query == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;

        *output = 0;
        if (!state->worker_active_ || state->targets_ == nullptr || state->traversal_ == nullptr)
            return SACCADE_ERROR_STATE;

        if (query->struct_size < sizeof(*query) || query->api_version != SACCADE_API_VERSION || query->window_id == 0 ||
            query->scope.width <= 0 || query->scope.height <= 0 || query->target_capacity == 0 ||
            query->target_capacity > SACCADE_TARGET_PACKET_MAX_TARGETS || query->flags != 0 ||
            query->session_epoch == 0 || query->transform_epoch == 0 || query->topology_epoch == 0 ||
            query->frame_id == 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (state->state_.load(std::memory_order_acquire) != WorkState::idle) return SACCADE_ERROR_BUSY;
        WindowEntry* window = state->find_window_entry(query->window_id);
        if (window == nullptr) {
            const SaccadeResult refreshed = state->refresh_windows();
            if (refreshed != SACCADE_OK) return refreshed;
            window = state->find_window_entry(query->window_id);
        }
        if (window == nullptr) return SACCADE_ERROR_NOT_FOUND;
        while (semaphore_timedwait(state->done_semaphore_, {0, 0}) == KERN_SUCCESS) {}
        state->query_ = *query;
        state->query_window_ = *window;
        state->ticket_ = state->next_ticket_++;
        if (state->next_ticket_ == 0 || state->next_ticket_ >= ticket_handle_limit) state->next_ticket_ = 1;
        state->snapshot_ = 0;
        state->target_count_ = 0;
        state->packet_size_ = 0;
        state->cancel_requested_.store(false, std::memory_order_relaxed);
        state->state_.store(WorkState::queued, std::memory_order_release);
        ++state->stats_.requests;
        *output = state->ticket_;
        semaphore_signal(state->request_semaphore_);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL poll(void* context, SaccadeTicketHandle ticket,
                                           SaccadeAccessibilityStatus* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (ticket == 0 || ticket != state->ticket_ || current == WorkState::idle) return SACCADE_ERROR_STALE_HANDLE;
        const SaccadeResult written = write_structure(output, state->status());
        if (written == SACCADE_OK && (current == WorkState::cancelled || current == WorkState::failed)) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
        }
        return written;
    }

    static SaccadeResult SACCADE_CALL wait(void* context, SaccadeTicketHandle ticket, uint64_t timeout_ns,
                                           SaccadeAccessibilityStatus* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (ticket == 0 || ticket != state->ticket_) return SACCADE_ERROR_STALE_HANDLE;
        ++state->stats_.waits;
        WorkState current = state->state_.load(std::memory_order_acquire);
        if (current == WorkState::queued || current == WorkState::running) {
            kern_return_t waited = timeout_ns == UINT64_MAX
                                       ? semaphore_wait(state->done_semaphore_)
                                       : semaphore_timedwait(state->done_semaphore_, timeout_from_ns(timeout_ns));
            if (waited == KERN_OPERATION_TIMED_OUT) {
                const SaccadeResult written = write_structure(output, state->status());
                return written == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : written;
            }
            if (waited != KERN_SUCCESS) return SACCADE_ERROR_BACKEND;
        }
        current = state->state_.load(std::memory_order_acquire);
        const SaccadeResult written = write_structure(output, state->status());
        if (written == SACCADE_OK && (current == WorkState::cancelled || current == WorkState::failed)) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
        }
        return written;
    }

    static SaccadeResult SACCADE_CALL collect(void* context, SaccadeSnapshotHandle snapshot,
                                              SaccadeMutableSpanU8 output, size_t* required) noexcept {
        Impl* state = from(context);
        if (state == nullptr || required == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 ||
            snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        *required = state->packet_size_;
        if (output.data == nullptr || output.size < state->packet_size_) return SACCADE_ERROR_CAPACITY;
        std::memcpy(output.data, &state->header_, sizeof(state->header_));
        std::memcpy(output.data + sizeof(state->header_), state->targets_,
                    static_cast<size_t>(state->target_count_) * sizeof(SaccadeTargetRecord));
        std::memcpy(output.data + sizeof(state->header_) +
                        static_cast<size_t>(state->target_count_) * sizeof(SaccadeTargetRecord),
                    state->text_, state->text_size_);
        state->stats_.copied_bytes += state->packet_size_;
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL cancel(void* context, SaccadeTicketHandle ticket) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (ticket == 0 || ticket != state->ticket_) return SACCADE_ERROR_STALE_HANDLE;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running) return SACCADE_ERROR_STATE;
        state->cancel_requested_.store(true, std::memory_order_release);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL release(void* context, SaccadeSnapshotHandle snapshot) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 ||
            snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        state->snapshot_ = 0;
        state->ticket_ = 0;
        state->state_.store(WorkState::idle, std::memory_order_release);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL synchronize(void* context, uint64_t timeout_ns) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running) return SACCADE_OK;
        const kern_return_t waited = timeout_ns == UINT64_MAX
                                         ? semaphore_wait(state->done_semaphore_)
                                         : semaphore_timedwait(state->done_semaphore_, timeout_from_ns(timeout_ns));
        if (waited == KERN_OPERATION_TIMED_OUT) return SACCADE_ERROR_TIMEOUT;
        return waited == KERN_SUCCESS ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    }

    static SaccadeResult SACCADE_CALL memory(void* context, SaccadeMemoryStats* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        SaccadeMemoryStats value{};
        value.host_committed = sizeof(Impl) + arena_bytes;
        value.host_reserved = sizeof(Impl) + arena_bytes;
        value.copied_bytes = state->stats_.copied_bytes;
        value.high_water_bytes = arena_bytes;
        return write_structure(output, value);
    }
};

static_assert(sizeof(AccessibilityProvider::Impl) <= AccessibilityProvider::storage_size);

AccessibilityProvider::AccessibilityProvider() noexcept {
    new (storage_.data()) Impl{};
}

AccessibilityProvider::~AccessibilityProvider() {
    shutdown();
    impl().~Impl();
}

AccessibilityProvider::Impl& AccessibilityProvider::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const AccessibilityProvider::Impl& AccessibilityProvider::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult AccessibilityProvider::initialize() noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    Impl& state = impl();
    state.stopping_.store(false, std::memory_order_relaxed);
    state.arena_ = mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (state.arena_ == MAP_FAILED) return SACCADE_ERROR_BACKEND;
    state.targets_ = static_cast<SaccadeTargetRecord*>(state.arena_);
    state.text_ = static_cast<uint8_t*>(state.arena_) + target_bytes;
    state.traversal_ = reinterpret_cast<TraversalEntry*>(state.text_ + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES);
    if (semaphore_create(mach_task_self(), &state.request_semaphore_, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS ||
        semaphore_create(mach_task_self(), &state.done_semaphore_, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    if (pthread_create(&state.worker_, nullptr, &Impl::worker_entry, &state) != 0) {
        shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    state.worker_active_ = true;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::shutdown() noexcept {
    Impl& state = impl();
    if (state.worker_active_) {
        state.cancel_requested_.store(true, std::memory_order_release);
        state.stopping_.store(true, std::memory_order_release);
        state.state_.store(WorkState::stopping, std::memory_order_release);
        semaphore_signal(state.request_semaphore_);
        pthread_join(state.worker_, nullptr);
        state.worker_active_ = false;
    }
    if (state.done_semaphore_ != SEMAPHORE_NULL) {
        semaphore_destroy(mach_task_self(), state.done_semaphore_);
        state.done_semaphore_ = SEMAPHORE_NULL;
    }
    if (state.request_semaphore_ != SEMAPHORE_NULL) {
        semaphore_destroy(mach_task_self(), state.request_semaphore_);
        state.request_semaphore_ = SEMAPHORE_NULL;
    }
    if (state.arena_ != MAP_FAILED) {
        munmap(state.arena_, arena_bytes);
        state.arena_ = MAP_FAILED;
        state.targets_ = nullptr;
        state.text_ = nullptr;
        state.traversal_ = nullptr;
    }
    state.state_.store(WorkState::idle, std::memory_order_relaxed);
    initialized_ = false;
    return SACCADE_OK;
}

SaccadeAccessibilityProviderDesc AccessibilityProvider::descriptor() noexcept {
    SaccadeAccessibilityOps ops{};
    ops.struct_size = sizeof(ops);
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_windows = &Impl::enumerate;
    ops.request = &Impl::request;
    ops.poll = &Impl::poll;
    ops.wait = &Impl::wait;
    ops.collect = &Impl::collect;
    ops.cancel = &Impl::cancel;
    ops.release = &Impl::release;
    ops.synchronize = &Impl::synchronize;
    ops.memory_stats = &Impl::memory;
    SaccadeAccessibilityProviderDesc output{};
    output.struct_size = sizeof(output);
    output.api_version = SACCADE_API_VERSION;
    output.info.struct_size = sizeof(output.info);
    output.info.api_version = SACCADE_API_VERSION;
    output.info.family = SACCADE_PROVIDER_FAMILY_ACCESSIBILITY;
    output.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    output.info.stable_id = provider_id;
    output.info.name = {reinterpret_cast<const uint8_t*>(provider_name), sizeof(provider_name) - 1};
    output.context = &impl();
    output.ops = ops;
    return output;
}

SaccadeResult AccessibilityProvider::read_stats(AccessibilityProviderStats* output) const noexcept {
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running) return SACCADE_ERROR_BUSY;
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::read_last_native_error(int32_t* output) const noexcept {
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running) return SACCADE_ERROR_BUSY;
    *output = impl().native_error_;
    return SACCADE_OK;
}

bool AccessibilityProvider::permission_granted() const noexcept {
    return AXIsProcessTrusted();
}

} // namespace saccade::platform::macos

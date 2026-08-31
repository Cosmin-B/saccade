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
constexpr uint32_t locator_generation_capacity = 2;
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

enum class LocatorGenerationState : uint32_t { free, filling, candidate, published };

struct LocatorEntry {
    AXUIElementRef element = nullptr;
    uint64_t target_id = 0;
    SaccadeRectI32 bounds_q8{};
    uint32_t target_flags = 0;
    bool supports_press = false;
    uint8_t reserved[3]{};
};

struct LocatorGeneration {
    LocatorGenerationState state = LocatorGenerationState::free;
    AccessibilityGenerationKey key{};
    AXUIElementRef window = nullptr;
    SaccadeRectI32 window_bounds{};
    uint32_t count = 0;
    uint32_t reserved = 0;
};

constexpr size_t target_bytes = static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
constexpr size_t traversal_bytes = static_cast<size_t>(maximum_pending_elements) * sizeof(TraversalEntry);
constexpr size_t locator_bytes =
    static_cast<size_t>(locator_generation_capacity) * SACCADE_TARGET_PACKET_MAX_TARGETS * sizeof(LocatorEntry);
constexpr size_t arena_bytes = target_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES + traversal_bytes + locator_bytes;

bool same_generation(const AccessibilityGenerationKey& left, const AccessibilityGenerationKey& right) noexcept {
    return left.session_epoch == right.session_epoch && left.frame_id == right.frame_id && left.transform_epoch == right.transform_epoch &&
           left.topology_epoch == right.topology_epoch && left.process_id == right.process_id && left.window_id == right.window_id;
}

bool same_rect(const SaccadeRectI32& left, const SaccadeRectI32& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

AXError native_perform_press(void*, AXUIElementRef element) noexcept {
    return AXUIElementPerformAction(element, kAXPressAction);
}

template <typename Structure> SaccadeResult write_structure(Structure* destination, Structure value) noexcept {
    if (destination == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t size = 0;
    uint32_t version = 0;
    std::memcpy(&size, destination, sizeof(size));
    std::memcpy(&version, reinterpret_cast<const uint8_t*>(destination) + offsetof(Structure, api_version), sizeof(version));
    if (size < offsetof(Structure, reserved))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if ((version >> 16U) != (SACCADE_API_VERSION >> 16U))
        return SACCADE_ERROR_VERSION;
    const size_t copy_size = std::min<size_t>(size, sizeof(value));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(destination, &value, copy_size);
    return SACCADE_OK;
}

bool number(CFDictionaryRef dictionary, CFStringRef key, int64_t* output) noexcept {
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberSInt64Type, output);
}

bool dictionary_window(CFDictionaryRef dictionary, WindowEntry* output) noexcept {
    int64_t identifier = 0;
    int64_t process = 0;
    int64_t layer = 0;
    const auto bounds_value = static_cast<CFDictionaryRef>(CFDictionaryGetValue(dictionary, kCGWindowBounds));
    CGRect bounds{};
    if (output == nullptr || !number(dictionary, kCGWindowNumber, &identifier) || !number(dictionary, kCGWindowOwnerPID, &process) ||
        !number(dictionary, kCGWindowLayer, &layer) || identifier <= 0 || process <= 0 || layer != 0 || process == getpid() ||
        bounds_value == nullptr || CFGetTypeID(bounds_value) != CFDictionaryGetTypeID() ||
        !CGRectMakeWithDictionaryRepresentation(bounds_value, &bounds) || !std::isfinite(bounds.origin.x) ||
        !std::isfinite(bounds.origin.y) || !std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width <= 0 || bounds.size.height <= 0 || bounds.origin.x < INT32_MIN || bounds.origin.x > INT32_MAX ||
        bounds.origin.y < INT32_MIN || bounds.origin.y > INT32_MAX || bounds.size.width > INT32_MAX || bounds.size.height > INT32_MAX) {
        return false;
    }
    WindowEntry entry{};
    entry.id = static_cast<uint64_t>(identifier);
    entry.process_id = static_cast<int32_t>(process);
    entry.bounds = {static_cast<int32_t>(std::llround(bounds.origin.x)), static_cast<int32_t>(std::llround(bounds.origin.y)),
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
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() ? CFBooleanGetValue(static_cast<CFBooleanRef>(value)) : fallback;
}

bool ax_geometry(AXUIElementRef element, CGRect* output) noexcept {
    CFTypeRef position_value = nullptr;
    CFTypeRef size_value = nullptr;
    const AXError position_error = AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value);
    const AXError size_error = AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value);
    CGPoint position{};
    CGSize size{};
    const bool valid = position_error == kAXErrorSuccess && size_error == kAXErrorSuccess && ax_point(position_value, &position) &&
                       ax_size(size_value, &size) && std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(size.width) &&
                       std::isfinite(size.height) && size.width > 0 && size.height > 0;
    if (position_value != nullptr)
        CFRelease(position_value);
    if (size_value != nullptr)
        CFRelease(size_value);
    if (valid)
        *output = {position, size};
    return valid;
}

uint32_t ax_title(AXUIElementRef element, std::array<uint8_t, window_title_capacity>* output) noexcept {
    CFTypeRef value = nullptr;
    const AXError error = AXUIElementCopyAttributeValue(element, kAXTitleAttribute, &value);
    if (error != kAXErrorSuccess || value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
        if (value != nullptr)
            CFRelease(value);
        return 0;
    }

    const bool encoded = CFStringGetCString(static_cast<CFStringRef>(value), reinterpret_cast<char*>(output->data()),
                                            static_cast<CFIndex>(output->size()), kCFStringEncodingUTF8);
    CFRelease(value);
    return encoded ? static_cast<uint32_t>(std::strlen(reinterpret_cast<const char*>(output->data()))) : 0;
}

SaccadeTargetRole role_for(CFStringRef role) noexcept {
    if (role == nullptr)
        return SACCADE_TARGET_ROLE_UNKNOWN;
    if (CFEqual(role, kAXButtonRole) || CFEqual(role, kAXPopUpButtonRole) || CFEqual(role, kAXMenuButtonRole))
        return SACCADE_TARGET_ROLE_BUTTON;
    if (CFEqual(role, CFSTR("AXLink")))
        return SACCADE_TARGET_ROLE_LINK;
    if (CFEqual(role, kAXStaticTextRole))
        return SACCADE_TARGET_ROLE_TEXT;
    if (CFEqual(role, kAXTextFieldRole) || CFEqual(role, kAXTextAreaRole))
        return SACCADE_TARGET_ROLE_TEXT_FIELD;
    if (CFEqual(role, kAXCheckBoxRole))
        return SACCADE_TARGET_ROLE_CHECKBOX;
    if (CFEqual(role, kAXRadioButtonRole))
        return SACCADE_TARGET_ROLE_RADIO;
    if (CFEqual(role, kAXMenuItemRole))
        return SACCADE_TARGET_ROLE_MENU_ITEM;
    if (CFEqual(role, kAXSliderRole))
        return SACCADE_TARGET_ROLE_SLIDER;
    if (CFEqual(role, kAXImageRole))
        return SACCADE_TARGET_ROLE_IMAGE;
    if (CFEqual(role, kAXWindowRole))
        return SACCADE_TARGET_ROLE_WINDOW;
    return SACCADE_TARGET_ROLE_UNKNOWN;
}

bool role_scrolls(CFStringRef role) noexcept {
    return role != nullptr && CFGetTypeID(role) == CFStringGetTypeID() &&
           (CFEqual(role, kAXScrollAreaRole) || CFEqual(role, kAXListRole) || CFEqual(role, kAXTableRole) || CFEqual(role, kAXOutlineRole));
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
                if (value == 0)
                    break;
                hash = mix(hash, value);
            }
        }
    }
    return hash == 0 ? 1 : hash;
}

int32_t q8(CGFloat value) noexcept {
    constexpr double minimum = static_cast<double>(INT32_MIN) / coordinate_scale;
    constexpr double maximum = static_cast<double>(INT32_MAX) / coordinate_scale;
    return static_cast<int32_t>(std::llround(std::clamp(static_cast<double>(value), minimum, maximum) * coordinate_scale));
}

bool intersects(const SaccadeRectI32& scope, const CGRect& rect) noexcept {
    const double right = static_cast<double>(scope.x) + scope.width;
    const double bottom = static_cast<double>(scope.y) + scope.height;
    return CGRectGetMaxX(rect) > scope.x && CGRectGetMaxY(rect) > scope.y && CGRectGetMinX(rect) < right && CGRectGetMinY(rect) < bottom;
}

uint64_t display_at(CGPoint point) noexcept {
    std::array<CGDirectDisplayID, 1> displays{};
    uint32_t count = 0;
    return CGGetDisplaysWithPoint(point, static_cast<uint32_t>(displays.size()), displays.data(), &count) == kCGErrorSuccess && count != 0
               ? displays[0]
               : 0;
}

uint32_t action_capabilities(AXUIElementRef element, CFStringRef role, SaccadeTargetRole mapped_role, bool* supports_press) noexcept {
    uint32_t bits = 0;
    *supports_press = false;
    if (mapped_role != SACCADE_TARGET_ROLE_UNKNOWN || role_scrolls(role)) {
        bits |= SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    }
    CFArrayRef actions = nullptr;
    if (AXUIElementCopyActionNames(element, &actions) == kAXErrorSuccess && actions != nullptr) {
        const CFIndex count = CFArrayGetCount(actions);
        for (CFIndex index = 0; index < count; ++index) {
            const auto action = static_cast<CFStringRef>(CFArrayGetValueAtIndex(actions, index));
            if (CFEqual(action, kAXPressAction) || CFEqual(action, kAXShowMenuAction)) {
                bits |= SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_INVOKE | SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
            }
            *supports_press |= CFEqual(action, kAXPressAction);
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
    if (role_scrolls(role))
        bits |= SACCADE_TARGET_CAPABILITY_SCROLL;
    if (mapped_role == SACCADE_TARGET_ROLE_WINDOW)
        bits |= SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
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
    alignas(core::destructive_interference_size) std::atomic<WorkState> action_state_{WorkState::idle};
    alignas(core::destructive_interference_size) std::atomic<bool> cancel_requested_{false};
    alignas(core::destructive_interference_size) std::atomic<bool> action_cancel_requested_{false};
    alignas(core::destructive_interference_size) std::atomic<bool> stopping_{false};
    semaphore_t request_semaphore_ = SEMAPHORE_NULL;
    semaphore_t done_semaphore_ = SEMAPHORE_NULL;
    pthread_t worker_{};
    bool worker_active_ = false;
    void* arena_ = MAP_FAILED;
    SaccadeTargetRecord* targets_ = nullptr;
    uint8_t* text_ = nullptr;
    TraversalEntry* traversal_ = nullptr;
    LocatorEntry* locators_ = nullptr;
    std::array<LocatorGeneration, locator_generation_capacity> locator_generations_{};
    uint32_t query_locator_generation_ = locator_generation_capacity;
    SaccadeAccessibilityQueryDesc query_{};
    WindowEntry query_window_{};
    SaccadeTicketHandle ticket_ = 0;
    SaccadeSnapshotHandle snapshot_ = 0;
    uint64_t next_ticket_ = 1;
    SaccadeTicketHandle action_ticket_ = 0;
    uint64_t next_action_ticket_ = 1;
    AccessibilityPressRequest action_request_{};
    AccessibilityPressStatus action_status_{};
    AccessibilityActionHooks action_hooks_{nullptr, &native_perform_press, nullptr};
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

    LocatorEntry* locator_entries(uint32_t generation_index) noexcept {
        return locators_ + static_cast<size_t>(generation_index) * SACCADE_TARGET_PACKET_MAX_TARGETS;
    }

    void clear_locator_generation(uint32_t generation_index) noexcept {
        LocatorGeneration& generation = locator_generations_[generation_index];
        LocatorEntry* entries = locator_entries(generation_index);
        for (uint32_t index = 0; index < generation.count; ++index) {
            if (entries[index].element != nullptr)
                CFRelease(entries[index].element);
            entries[index] = {};
        }
        if (generation.window != nullptr)
            CFRelease(generation.window);
        generation = {};
    }

    uint32_t find_locator_generation(const AccessibilityGenerationKey& key) const noexcept {
        for (uint32_t index = 0; index < locator_generation_capacity; ++index) {
            const LocatorGeneration& generation = locator_generations_[index];
            if (generation.state != LocatorGenerationState::free && same_generation(generation.key, key))
                return index;
        }
        return locator_generation_capacity;
    }

    uint32_t free_locator_generation() const noexcept {
        for (uint32_t index = 0; index < locator_generation_capacity; ++index) {
            if (locator_generations_[index].state == LocatorGenerationState::free)
                return index;
        }
        return locator_generation_capacity;
    }

    void append_text(CFStringRef value, bool secure, SaccadeTargetRecord* target) noexcept {
        if (secure) {
            target->flags |= SACCADE_TARGET_TEXT_REDACTED;
            return;
        }
        if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
            return;
        const CFIndex length = CFStringGetLength(value);
        if (length == 0)
            return;
        CFIndex required = 0;
        const CFIndex converted = CFStringGetBytes(value, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false, nullptr, 0, &required);
        if (converted != length || required <= 0)
            return;
        if (static_cast<uint64_t>(required) > SACCADE_TARGET_PACKET_MAX_TEXT_BYTES - text_size_) {
            target->flags |= SACCADE_TARGET_TEXT_TRUNCATED;
            packet_flags_ |= SACCADE_TARGET_PACKET_TEXT_TRUNCATED;
            return;
        }
        CFIndex written = 0;
        const CFIndex encoded =
            CFStringGetBytes(value, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false, text_ + text_size_, required, &written);
        bool contains_nul = false;
        for (CFIndex index = 0; index < written; ++index)
            contains_nul |= text_[text_size_ + index] == 0;
        if (encoded != length || written != required || contains_nul)
            return;
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
            CFArrayCreate(kCFAllocatorDefault, attribute_values, static_cast<CFIndex>(std::size(attribute_values)), &kCFTypeArrayCallBacks);
        for (;;) {
            if (semaphore_wait(request_semaphore_) != KERN_SUCCESS)
                continue;
            if (stopping_.load(std::memory_order_acquire))
                break;
            if (action_state_.load(std::memory_order_acquire) == WorkState::queued) {
                action_state_.store(WorkState::running, std::memory_order_release);
                run_press();
                semaphore_signal(done_semaphore_);
                continue;
            }
            if (state_.load(std::memory_order_acquire) == WorkState::queued) {
                state_.store(WorkState::running, std::memory_order_release);
                if (attributes == nullptr) {
                    result_ = SACCADE_ERROR_BACKEND;
                    ++stats_.failed;
                    abandon_query_locators();
                    state_.store(WorkState::failed, std::memory_order_release);
                } else {
                    run_query(attributes);
                }
                semaphore_signal(done_semaphore_);
            }
        }
        if (attributes != nullptr)
            CFRelease(attributes);
    }

    AXUIElementRef find_window() noexcept {
        AXUIElementRef application = AXUIElementCreateApplication(query_window_.process_id);
        if (application == nullptr)
            return nullptr;
        AXUIElementSetMessagingTimeout(application, ax_messaging_timeout_seconds);
        CFTypeRef windows_value = nullptr;
        const AXError error = AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &windows_value);
        if (error != kAXErrorSuccess || windows_value == nullptr || CFGetTypeID(windows_value) != CFArrayGetTypeID()) {
            native_error_ = error;
            if (windows_value != nullptr)
                CFRelease(windows_value);
            CFRelease(application);
            return nullptr;
        }
        const auto windows = static_cast<CFArrayRef>(windows_value);
        AXUIElementRef best = nullptr;
        double best_distance = std::numeric_limits<double>::max();
        uint32_t best_count = 0;
        const CFIndex count = CFArrayGetCount(windows);
        for (CFIndex index = 0; index < count; ++index) {
            const auto candidate = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(windows, index)));
            CGRect bounds{};
            if (!ax_geometry(candidate, &bounds))
                continue;
            std::array<uint8_t, window_title_capacity> title{};
            const uint32_t title_size = ax_title(candidate, &title);
            if (query_window_.title_size != 0 && (title_size != query_window_.title_size ||
                                                  std::memcmp(title.data(), query_window_.title.data(), query_window_.title_size) != 0)) {
                continue;
            }
            const double distance =
                std::abs(bounds.origin.x - query_window_.bounds.x) + std::abs(bounds.origin.y - query_window_.bounds.y) +
                std::abs(bounds.size.width - query_window_.bounds.width) + std::abs(bounds.size.height - query_window_.bounds.height);
            if (distance + 0.001 < best_distance) {
                best_distance = distance;
                best = candidate;
                best_count = 1;
            } else if (std::abs(distance - best_distance) <= 0.001) {
                ++best_count;
            }
        }
        if (best != nullptr && best_count == 1 && best_distance < 4.0)
            CFRetain(best);
        else
            best = nullptr;
        CFRelease(windows);
        CFRelease(application);
        return best;
    }

    bool append_target(AXUIElementRef element, uint64_t parent_id, CFArrayRef attributes) noexcept {
        CFArrayRef values = nullptr;
        const AXError error =
            AXUIElementCopyMultipleAttributeValues(element, attributes, static_cast<AXCopyMultipleAttributeOptions>(0), &values);
        if (error != kAXErrorSuccess || values == nullptr || CFArrayGetCount(values) < attribute_count) {
            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
            native_error_ = error;
            if (values != nullptr)
                CFRelease(values);
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
        if (hidden || !ax_point(position_value, &position) || !ax_size(size_value, &size) || size.width <= 0 || size.height <= 0) {
            CFRelease(values);
            return false;
        }
        const CGRect bounds{position, size};
        if (!intersects(query_.scope, bounds)) {
            CFRelease(values);
            return false;
        }
        bool supports_press = false;
        uint32_t capabilities = action_capabilities(element, role, mapped_role, &supports_press);
        const bool enabled = ax_bool(enabled_value, true);
        const bool secure =
            subrole != nullptr && CFGetTypeID(subrole) == CFStringGetTypeID() && CFEqual(subrole, kAXSecureTextFieldSubrole);
        if (capabilities == 0 && !secure && enabled) {
            CFRelease(values);
            return false;
        }
        SaccadeTargetRecord& target = targets_[target_count_];
        target = {};
        target.target_id =
            element_id(element, identifier != nullptr && CFGetTypeID(identifier) == CFStringGetTypeID() ? identifier : nullptr,
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
            title != nullptr && CFGetTypeID(title) == CFStringGetTypeID() && CFStringGetLength(title) != 0 ? title : description;
        append_text(text, secure, &target);
        if (query_locator_generation_ < locator_generation_capacity) {
            LocatorGeneration& generation = locator_generations_[query_locator_generation_];
            LocatorEntry& locator = locator_entries(query_locator_generation_)[generation.count++];
            locator.element = static_cast<AXUIElementRef>(CFRetain(element));
            locator.target_id = target.target_id;
            locator.bounds_q8 = {target.x_q8, target.y_q8, target.width_q8, target.height_q8};
            locator.target_flags = target.flags;
            locator.supports_press = supports_press;
        }
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

    void abandon_query_locators() noexcept {
        if (query_locator_generation_ < locator_generation_capacity)
            clear_locator_generation(query_locator_generation_);
        query_locator_generation_ = locator_generation_capacity;
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
            abandon_query_locators();
            state_.store(WorkState::failed, std::memory_order_release);
            return;
        }
        if (cancel_requested_.load(std::memory_order_acquire)) {
            result_ = SACCADE_ERROR_CANCELLED;
            ++stats_.cancelled;
            abandon_query_locators();
            state_.store(WorkState::cancelled, std::memory_order_release);
            return;
        }
        AXUIElementRef root = find_window();
        if (root == nullptr) {
            result_ = native_error_ == kAXErrorAPIDisabled ? SACCADE_ERROR_PERMISSION : SACCADE_ERROR_NOT_FOUND;
            ++stats_.failed;
            abandon_query_locators();
            state_.store(WorkState::failed, std::memory_order_release);
            return;
        }
        LocatorGeneration& locator_generation = locator_generations_[query_locator_generation_];
        locator_generation.window = static_cast<AXUIElementRef>(CFRetain(root));

        uint32_t stack_count = 1;
        uint32_t visited = 0;
        traversal_[0] = {root, query_.window_id, 0, 0};
        const uint32_t capacity = std::min(query_.target_capacity, SACCADE_TARGET_PACKET_MAX_TARGETS);
        while (stack_count != 0 && target_count_ < capacity && visited < maximum_visited_elements) {
            if (cancel_requested_.load(std::memory_order_acquire)) {
                clear_traversal(stack_count);
                result_ = SACCADE_ERROR_CANCELLED;
                ++stats_.cancelled;
                abandon_query_locators();
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
                const AXError children_error = AXUIElementCopyAttributeValue(entry.element, kAXChildrenAttribute, &children_value);
                if (children_error == kAXErrorSuccess && children_value != nullptr && CFGetTypeID(children_value) == CFArrayGetTypeID()) {
                    const auto children = static_cast<CFArrayRef>(children_value);
                    const CFIndex child_count = CFArrayGetCount(children);
                    for (CFIndex index = child_count; index > 0; --index) {
                        if (stack_count == maximum_pending_elements) {
                            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                            break;
                        }
                        const auto child = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(children, index - 1)));
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
                if (children_value != nullptr)
                    CFRelease(children_value);
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
        packet_size_ =
            static_cast<uint32_t>(sizeof(header_) + static_cast<size_t>(target_count_) * sizeof(SaccadeTargetRecord) + text_size_);
        header_.total_size = packet_size_;
        snapshot_ = ticket_ | snapshot_handle_bit;
        ++stats_.completed;
        stats_.targets += target_count_;
        stats_.incomplete += packet_flags_ != 0 ? 1U : 0U;
        locator_generation.state = LocatorGenerationState::candidate;
        query_locator_generation_ = locator_generation_capacity;
        state_.store(WorkState::complete, std::memory_order_release);
    }

    void complete_press(SaccadeAgentResult result, int32_t native_error, uint32_t attempt_count) noexcept {
        action_status_.result = result;
        action_status_.native_error = native_error;
        action_status_.attempt_count = attempt_count;
        action_state_.store(WorkState::complete, std::memory_order_release);
    }

    bool current_window_matches(const LocatorGeneration& generation) noexcept {
        if (generation.key.window_id > UINT32_MAX || generation.key.process_id > static_cast<uint64_t>(INT32_MAX))
            return false;
        CFArrayRef descriptions =
            CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>(generation.key.window_id));
        if (descriptions == nullptr)
            return false;
        bool matches = false;
        if (CFArrayGetCount(descriptions) == 1) {
            WindowEntry current{};
            const auto description = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(descriptions, 0));
            const CFTypeRef on_screen_value = CFDictionaryGetValue(description, kCGWindowIsOnscreen);
            bool on_screen = false;
            if (on_screen_value != nullptr && CFGetTypeID(on_screen_value) == CFBooleanGetTypeID()) {
                on_screen = CFBooleanGetValue(static_cast<CFBooleanRef>(on_screen_value));
            } else if (on_screen_value != nullptr && CFGetTypeID(on_screen_value) == CFNumberGetTypeID()) {
                int32_t numeric_on_screen = 0;
                on_screen = CFNumberGetValue(static_cast<CFNumberRef>(on_screen_value), kCFNumberSInt32Type, &numeric_on_screen) &&
                            numeric_on_screen != 0;
            }
            matches = on_screen && dictionary_window(description, &current) && current.id == generation.key.window_id &&
                      current.process_id == static_cast<int32_t>(generation.key.process_id) &&
                      same_rect(current.bounds, generation.window_bounds);
        }
        CFRelease(descriptions);
        return matches;
    }

    bool window_contains_visible_element(AXUIElementRef window, AXUIElementRef target, AXError* native_error) noexcept {
        uint32_t stack_count = 1;
        uint32_t visited = 0;
        traversal_[0] = {static_cast<AXUIElementRef>(CFRetain(window)), 0, 0, 0};
        while (stack_count != 0 && visited < maximum_visited_elements) {
            TraversalEntry entry = traversal_[--stack_count];
            traversal_[stack_count].element = nullptr;
            ++visited;
            if (CFEqual(entry.element, target)) {
                CFRelease(entry.element);
                clear_traversal(stack_count);
                return true;
            }

            CFTypeRef children_value = nullptr;
            AXError error = AXUIElementCopyAttributeValue(entry.element, kAXVisibleChildrenAttribute, &children_value);
            if (error == kAXErrorAttributeUnsupported) {
                if (children_value != nullptr) {
                    CFRelease(children_value);
                    children_value = nullptr;
                }
                error = AXUIElementCopyAttributeValue(entry.element, kAXChildrenAttribute, &children_value);
            }
            if (error == kAXErrorSuccess && children_value != nullptr && CFGetTypeID(children_value) == CFArrayGetTypeID()) {
                const auto children = static_cast<CFArrayRef>(children_value);
                const CFIndex child_count = CFArrayGetCount(children);
                for (CFIndex index = child_count; index > 0; --index) {
                    if (stack_count == maximum_pending_elements) {
                        if (children_value != nullptr)
                            CFRelease(children_value);
                        CFRelease(entry.element);
                        clear_traversal(stack_count);
                        *native_error = kAXErrorFailure;
                        return false;
                    }
                    const auto child = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(children, index - 1)));
                    if (child == nullptr || CFGetTypeID(child) != AXUIElementGetTypeID())
                        continue;
                    traversal_[stack_count++] = {static_cast<AXUIElementRef>(CFRetain(child)), 0, 0, 0};
                }
            } else if (error != kAXErrorNoValue && error != kAXErrorAttributeUnsupported) {
                if (children_value != nullptr)
                    CFRelease(children_value);
                CFRelease(entry.element);
                clear_traversal(stack_count);
                *native_error = error;
                return false;
            }
            if (children_value != nullptr)
                CFRelease(children_value);
            CFRelease(entry.element);
        }
        clear_traversal(stack_count);
        *native_error = visited == maximum_visited_elements ? kAXErrorFailure : kAXErrorSuccess;
        return false;
    }

    void run_press() noexcept {
        if (action_cancel_requested_.load(std::memory_order_acquire)) {
            complete_press(SACCADE_AGENT_ERROR_CANCELLED, 0, 0);
            return;
        }
        if (!AXIsProcessTrusted()) {
            complete_press(SACCADE_AGENT_ERROR_PERMISSION_DENIED, kAXErrorAPIDisabled, 0);
            return;
        }
        const uint32_t generation_index = find_locator_generation(action_request_.generation);
        if (generation_index == locator_generation_capacity ||
            locator_generations_[generation_index].state != LocatorGenerationState::published) {
            complete_press(SACCADE_AGENT_ERROR_STALE_GENERATION, 0, 0);
            return;
        }

        const LocatorGeneration& generation = locator_generations_[generation_index];
        LocatorEntry* entries = locator_entries(generation_index);
        LocatorEntry* locator = nullptr;
        uint32_t matches = 0;
        for (uint32_t index = 0; index < generation.count; ++index) {
            if (entries[index].target_id == action_request_.target_id) {
                locator = &entries[index];
                ++matches;
            }
        }
        if (matches == 0) {
            complete_press(SACCADE_AGENT_ERROR_TARGET_NOT_FOUND, 0, 0);
            return;
        }
        if (matches != 1 || locator == nullptr) {
            complete_press(SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, 0, 0);
            return;
        }
        if (!same_rect(locator->bounds_q8, action_request_.bounds_q8)) {
            complete_press(SACCADE_AGENT_ERROR_STALE_GENERATION, 0, 0);
            return;
        }
        if ((locator->target_flags & SACCADE_TARGET_SECURE) != 0) {
            complete_press(SACCADE_AGENT_ERROR_SECURE_SURFACE, 0, 0);
            return;
        }
        if ((locator->target_flags & (SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_DISABLED)) != SACCADE_TARGET_ACTIONABLE) {
            complete_press(SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, 0, 0);
            return;
        }
        if (!locator->supports_press) {
            complete_press(SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, 0, 0);
            return;
        }
        if (!current_window_matches(generation)) {
            complete_press(SACCADE_AGENT_ERROR_STALE_GENERATION, 0, 0);
            return;
        }

        pid_t process_id = 0;
        AXError error = AXUIElementGetPid(locator->element, &process_id);
        if (error != kAXErrorSuccess || process_id <= 0 || static_cast<uint64_t>(process_id) != generation.key.process_id) {
            complete_press(error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED : SACCADE_AGENT_ERROR_STALE_GENERATION,
                           error, 0);
            return;
        }
        CFTypeRef window_value = nullptr;
        error = AXUIElementCopyAttributeValue(locator->element, kAXWindowAttribute, &window_value);
        const bool same_window = error == kAXErrorSuccess && window_value != nullptr && CFEqual(window_value, generation.window);
        if (window_value != nullptr)
            CFRelease(window_value);
        if (!same_window) {
            complete_press(error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED : SACCADE_AGENT_ERROR_STALE_GENERATION,
                           error, 0);
            return;
        }
        AXError visibility_error = kAXErrorSuccess;
        if (!window_contains_visible_element(generation.window, locator->element, &visibility_error)) {
            complete_press(visibility_error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED
                                                                   : SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE,
                           visibility_error, 0);
            return;
        }

        CGRect bounds{};
        if (!ax_geometry(locator->element, &bounds) ||
            !same_rect(locator->bounds_q8, {q8(bounds.origin.x), q8(bounds.origin.y), q8(bounds.size.width), q8(bounds.size.height)})) {
            complete_press(SACCADE_AGENT_ERROR_STALE_GENERATION, 0, 0);
            return;
        }
        CFTypeRef enabled_value = nullptr;
        error = AXUIElementCopyAttributeValue(locator->element, kAXEnabledAttribute, &enabled_value);
        const bool enabled = error == kAXErrorSuccess && ax_bool(enabled_value, false);
        if (enabled_value != nullptr)
            CFRelease(enabled_value);
        if (!enabled) {
            complete_press(error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED : SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE,
                           error, 0);
            return;
        }
        CFTypeRef hidden_value = nullptr;
        error = AXUIElementCopyAttributeValue(locator->element, kAXHiddenAttribute, &hidden_value);
        const bool hidden = error == kAXErrorSuccess && ax_bool(hidden_value, false);
        if (hidden_value != nullptr)
            CFRelease(hidden_value);
        if ((error != kAXErrorSuccess && error != kAXErrorAttributeUnsupported && error != kAXErrorNoValue) || hidden) {
            complete_press(error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED : SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE,
                           error, 0);
            return;
        }
        CFTypeRef subrole_value = nullptr;
        error = AXUIElementCopyAttributeValue(locator->element, kAXSubroleAttribute, &subrole_value);
        const bool secure = error == kAXErrorSuccess && subrole_value != nullptr && CFGetTypeID(subrole_value) == CFStringGetTypeID() &&
                            CFEqual(subrole_value, kAXSecureTextFieldSubrole);
        if (subrole_value != nullptr)
            CFRelease(subrole_value);
        if (secure) {
            complete_press(SACCADE_AGENT_ERROR_SECURE_SURFACE, 0, 0);
            return;
        }

        CFArrayRef actions = nullptr;
        error = AXUIElementCopyActionNames(locator->element, &actions);
        bool supports_press = false;
        if (error == kAXErrorSuccess && actions != nullptr) {
            for (CFIndex index = 0; index < CFArrayGetCount(actions); ++index) {
                const auto action = static_cast<CFStringRef>(CFArrayGetValueAtIndex(actions, index));
                supports_press |= CFEqual(action, kAXPressAction);
            }
        }
        if (actions != nullptr)
            CFRelease(actions);
        if (!supports_press) {
            complete_press(error == kAXErrorAPIDisabled ? SACCADE_AGENT_ERROR_PERMISSION_DENIED : SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED,
                           error, 0);
            return;
        }

        if (action_hooks_.before_perform_press != nullptr)
            action_hooks_.before_perform_press(action_hooks_.context);
        // This is the last cancellable point before the externally visible
        // AX action. A cancellation racing after this check is reported as an
        // attempted action with an unconfirmed outcome and is never retried.
        if (action_cancel_requested_.load(std::memory_order_acquire)) {
            complete_press(SACCADE_AGENT_ERROR_CANCELLED, 0, 0);
            return;
        }
        error = action_hooks_.perform_press(action_hooks_.context, locator->element);
        if (action_cancel_requested_.load(std::memory_order_acquire)) {
            complete_press(SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED, error, 1);
        } else if (error == kAXErrorSuccess) {
            complete_press(SACCADE_AGENT_OK, 0, 1);
        } else if (error == kAXErrorCannotComplete) {
            complete_press(SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED, error, 1);
        } else if (error == kAXErrorAPIDisabled) {
            complete_press(SACCADE_AGENT_ERROR_PERMISSION_DENIED, error, 1);
        } else if (error == kAXErrorActionUnsupported) {
            complete_press(SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, error, 1);
        } else {
            complete_press(SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, error, 1);
        }
    }

    void append_minimized_windows(int32_t process_id, CFArrayRef descriptions) noexcept {
        AXUIElementRef application = AXUIElementCreateApplication(process_id);
        if (application == nullptr)
            return;

        AXUIElementSetMessagingTimeout(application, ax_messaging_timeout_seconds);
        CFTypeRef values = nullptr;
        const AXError copied = AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &values);
        if (copied == kAXErrorSuccess && values != nullptr && CFGetTypeID(values) == CFArrayGetTypeID()) {
            const auto ax_windows = static_cast<CFArrayRef>(values);
            const CFIndex description_count = CFArrayGetCount(descriptions);
            for (CFIndex ax_index = 0; ax_index < CFArrayGetCount(ax_windows) && window_count_ < maximum_windows; ++ax_index) {
                const auto ax_window = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(ax_windows, ax_index)));
                CFTypeRef minimized_value = nullptr;
                const AXError minimized_error = AXUIElementCopyAttributeValue(ax_window, kAXMinimizedAttribute, &minimized_value);
                const bool minimized = minimized_error == kAXErrorSuccess && ax_bool(minimized_value, false);
                if (minimized_value != nullptr)
                    CFRelease(minimized_value);

                CGRect ax_bounds{};
                if (!minimized || !ax_geometry(ax_window, &ax_bounds))
                    continue;

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

                    const bool title_matches =
                        title_size == 0 || candidate.title_size == 0 ||
                        (title_size == candidate.title_size && std::memcmp(title.data(), candidate.title.data(), title_size) == 0);
                    const double distance =
                        std::abs(ax_bounds.origin.x - candidate.bounds.x) + std::abs(ax_bounds.origin.y - candidate.bounds.y) +
                        std::abs(ax_bounds.size.width - candidate.bounds.width) + std::abs(ax_bounds.size.height - candidate.bounds.height);
                    if (!title_matches || distance >= 4.0)
                        continue;
                    match = candidate;
                    ++match_count;
                }
                if (match_count == 1)
                    windows_[window_count_++] = match;
            }
        }
        if (values != nullptr)
            CFRelease(values);
        CFRelease(application);
    }

    SaccadeResult refresh_windows() noexcept {
        CFArrayRef descriptions =
            CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (descriptions == nullptr)
            return SACCADE_ERROR_BACKEND;
        window_count_ = 0;
        const CFIndex count = CFArrayGetCount(descriptions);
        for (CFIndex index = 0; index < count && window_count_ < maximum_windows; ++index) {
            const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(descriptions, index));
            WindowEntry entry{};
            if (dictionary_window(dictionary, &entry))
                windows_[window_count_++] = entry;
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
                if (unique)
                    processes[process_count++] = process_id;
            }

            const CFIndex description_count = CFArrayGetCount(all_descriptions);
            for (CFIndex index = 0; index < description_count && process_count < maximum_windows; ++index) {
                const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(all_descriptions, index));
                WindowEntry candidate{};
                if (!dictionary_window(dictionary, &candidate))
                    continue;

                bool unique = true;
                for (uint32_t previous = 0; previous < process_count; ++previous)
                    unique &= processes[previous] != candidate.process_id;
                if (unique)
                    processes[process_count++] = candidate.process_id;
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
            if (windows_[index].id == id)
                return &windows_[index];
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
        if (state == nullptr || output == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (index == 0) {
            const SaccadeResult refreshed = state->refresh_windows();
            if (refreshed != SACCADE_OK)
                return refreshed;
        }
        if (index >= state->window_count_)
            return SACCADE_ERROR_NOT_FOUND;
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
        if (state == nullptr || query == nullptr || output == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;

        *output = 0;
        if (!state->worker_active_ || state->targets_ == nullptr || state->traversal_ == nullptr || state->locators_ == nullptr)
            return SACCADE_ERROR_STATE;

        if (query->struct_size < sizeof(*query) || query->api_version != SACCADE_API_VERSION || query->window_id == 0 ||
            query->scope.width <= 0 || query->scope.height <= 0 || query->target_capacity == 0 ||
            query->target_capacity > SACCADE_TARGET_PACKET_MAX_TARGETS || query->flags != 0 || query->session_epoch == 0 ||
            query->transform_epoch == 0 || query->topology_epoch == 0 || query->frame_id == 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (state->state_.load(std::memory_order_acquire) != WorkState::idle)
            return SACCADE_ERROR_BUSY;
        if (state->action_state_.load(std::memory_order_acquire) != WorkState::idle)
            return SACCADE_ERROR_BUSY;
        WindowEntry* window = state->find_window_entry(query->window_id);
        if (window == nullptr) {
            const SaccadeResult refreshed = state->refresh_windows();
            if (refreshed != SACCADE_OK)
                return refreshed;
            window = state->find_window_entry(query->window_id);
        }
        if (window == nullptr)
            return SACCADE_ERROR_NOT_FOUND;
        const AccessibilityGenerationKey generation{query->session_epoch,
                                                    query->frame_id,
                                                    query->transform_epoch,
                                                    query->topology_epoch,
                                                    static_cast<uint64_t>(window->process_id),
                                                    query->window_id};
        if (state->find_locator_generation(generation) != locator_generation_capacity)
            return SACCADE_ERROR_ALREADY_EXISTS;
        const uint32_t locator_generation = state->free_locator_generation();
        if (locator_generation == locator_generation_capacity)
            return SACCADE_ERROR_CAPACITY;
        while (semaphore_timedwait(state->done_semaphore_, {0, 0}) == KERN_SUCCESS) {}
        LocatorGeneration& locators = state->locator_generations_[locator_generation];
        locators.state = LocatorGenerationState::filling;
        locators.key = generation;
        locators.window_bounds = window->bounds;
        locators.count = 0;
        state->query_locator_generation_ = locator_generation;
        state->query_ = *query;
        state->query_window_ = *window;
        state->ticket_ = state->next_ticket_++;
        if (state->next_ticket_ == 0 || state->next_ticket_ >= ticket_handle_limit)
            state->next_ticket_ = 1;
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

    static SaccadeResult SACCADE_CALL poll(void* context, SaccadeTicketHandle ticket, SaccadeAccessibilityStatus* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (ticket == 0 || ticket != state->ticket_ || current == WorkState::idle)
            return SACCADE_ERROR_STALE_HANDLE;
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
        if (state == nullptr || output == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (ticket == 0 || ticket != state->ticket_)
            return SACCADE_ERROR_STALE_HANDLE;
        ++state->stats_.waits;
        WorkState current = state->state_.load(std::memory_order_acquire);
        if (current == WorkState::queued || current == WorkState::running) {
            kern_return_t waited = timeout_ns == UINT64_MAX ? semaphore_wait(state->done_semaphore_)
                                                            : semaphore_timedwait(state->done_semaphore_, timeout_from_ns(timeout_ns));
            if (waited == KERN_OPERATION_TIMED_OUT) {
                const SaccadeResult written = write_structure(output, state->status());
                return written == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : written;
            }
            if (waited != KERN_SUCCESS)
                return SACCADE_ERROR_BACKEND;
        }
        current = state->state_.load(std::memory_order_acquire);
        const SaccadeResult written = write_structure(output, state->status());
        if (written == SACCADE_OK && (current == WorkState::cancelled || current == WorkState::failed)) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
        }
        return written;
    }

    static SaccadeResult SACCADE_CALL collect(void* context, SaccadeSnapshotHandle snapshot, SaccadeMutableSpanU8 output,
                                              size_t* required) noexcept {
        Impl* state = from(context);
        if (state == nullptr || required == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 || snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        *required = state->packet_size_;
        if (output.data == nullptr || output.size < state->packet_size_)
            return SACCADE_ERROR_CAPACITY;
        std::memcpy(output.data, &state->header_, sizeof(state->header_));
        std::memcpy(output.data + sizeof(state->header_), state->targets_,
                    static_cast<size_t>(state->target_count_) * sizeof(SaccadeTargetRecord));
        std::memcpy(output.data + sizeof(state->header_) + static_cast<size_t>(state->target_count_) * sizeof(SaccadeTargetRecord),
                    state->text_, state->text_size_);
        state->stats_.copied_bytes += state->packet_size_;
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL cancel(void* context, SaccadeTicketHandle ticket) noexcept {
        Impl* state = from(context);
        if (state == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (ticket == 0 || ticket != state->ticket_)
            return SACCADE_ERROR_STALE_HANDLE;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running)
            return SACCADE_ERROR_STATE;
        state->cancel_requested_.store(true, std::memory_order_release);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL release(void* context, SaccadeSnapshotHandle snapshot) noexcept {
        Impl* state = from(context);
        if (state == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 || snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        state->snapshot_ = 0;
        state->ticket_ = 0;
        state->state_.store(WorkState::idle, std::memory_order_release);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL synchronize(void* context, uint64_t timeout_ns) noexcept {
        Impl* state = from(context);
        if (state == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running)
            return SACCADE_OK;
        const kern_return_t waited = timeout_ns == UINT64_MAX ? semaphore_wait(state->done_semaphore_)
                                                              : semaphore_timedwait(state->done_semaphore_, timeout_from_ns(timeout_ns));
        if (waited == KERN_OPERATION_TIMED_OUT)
            return SACCADE_ERROR_TIMEOUT;
        return waited == KERN_SUCCESS ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    }

    static SaccadeResult SACCADE_CALL memory(void* context, SaccadeMemoryStats* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
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

SaccadeResult AccessibilityProvider::initialize(const AccessibilityActionHooks* action_hooks) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (action_hooks != nullptr && action_hooks->perform_press == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    state.action_hooks_ =
        action_hooks != nullptr ? *action_hooks : AccessibilityActionHooks{nullptr, &native_perform_press, nullptr};
    state.stopping_.store(false, std::memory_order_relaxed);
    state.arena_ = mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (state.arena_ == MAP_FAILED)
        return SACCADE_ERROR_BACKEND;
    state.targets_ = static_cast<SaccadeTargetRecord*>(state.arena_);
    state.text_ = static_cast<uint8_t*>(state.arena_) + target_bytes;
    state.traversal_ = reinterpret_cast<TraversalEntry*>(state.text_ + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES);
    state.locators_ = reinterpret_cast<LocatorEntry*>(reinterpret_cast<uint8_t*>(state.traversal_) + traversal_bytes);
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
        state.action_cancel_requested_.store(true, std::memory_order_release);
        state.stopping_.store(true, std::memory_order_release);
        state.state_.store(WorkState::stopping, std::memory_order_release);
        state.action_state_.store(WorkState::stopping, std::memory_order_release);
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
        for (uint32_t index = 0; index < locator_generation_capacity; ++index)
            state.clear_locator_generation(index);
        munmap(state.arena_, arena_bytes);
        state.arena_ = MAP_FAILED;
        state.targets_ = nullptr;
        state.text_ = nullptr;
        state.traversal_ = nullptr;
        state.locators_ = nullptr;
    }
    state.state_.store(WorkState::idle, std::memory_order_relaxed);
    state.action_state_.store(WorkState::idle, std::memory_order_relaxed);
    state.query_locator_generation_ = locator_generation_capacity;
    state.action_ticket_ = 0;
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
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running)
        return SACCADE_ERROR_BUSY;
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::read_last_native_error(int32_t* output) const noexcept {
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running)
        return SACCADE_ERROR_BUSY;
    *output = impl().native_error_;
    return SACCADE_OK;
}

bool AccessibilityProvider::permission_granted() const noexcept {
    return AXIsProcessTrusted();
}

SaccadeResult AccessibilityProvider::promote_action_generation(const AccessibilityGenerationKey& key) noexcept {
    Impl& state = impl();
    if (!initialized_ || key.session_epoch == 0 || key.frame_id == 0 || key.transform_epoch == 0 || key.topology_epoch == 0 ||
        key.process_id == 0 || key.window_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const WorkState query_state = state.state_.load(std::memory_order_acquire);
    const WorkState action_state = state.action_state_.load(std::memory_order_acquire);
    if (query_state == WorkState::queued || query_state == WorkState::running || action_state == WorkState::queued ||
        action_state == WorkState::running) {
        return SACCADE_ERROR_BUSY;
    }
    const uint32_t index = state.find_locator_generation(key);
    if (index == locator_generation_capacity)
        return SACCADE_ERROR_STALE_HANDLE;
    LocatorGeneration& generation = state.locator_generations_[index];
    if (generation.state != LocatorGenerationState::candidate && generation.state != LocatorGenerationState::published)
        return SACCADE_ERROR_STATE;
    generation.state = LocatorGenerationState::published;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::retire_action_generation(const AccessibilityGenerationKey& key) noexcept {
    Impl& state = impl();
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    const WorkState query_state = state.state_.load(std::memory_order_acquire);
    const WorkState action_state = state.action_state_.load(std::memory_order_acquire);
    if (query_state == WorkState::queued || query_state == WorkState::running || action_state == WorkState::queued ||
        action_state == WorkState::running) {
        return SACCADE_ERROR_BUSY;
    }
    const uint32_t index = state.find_locator_generation(key);
    if (index == locator_generation_capacity)
        return SACCADE_ERROR_STALE_HANDLE;
    state.clear_locator_generation(index);
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::retire_action_session(uint64_t session_epoch) noexcept {
    Impl& state = impl();
    if (!initialized_ || session_epoch == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState query_state = state.state_.load(std::memory_order_acquire);
    const WorkState action_state = state.action_state_.load(std::memory_order_acquire);
    if (query_state == WorkState::queued || query_state == WorkState::running || action_state == WorkState::queued ||
        action_state == WorkState::running) {
        return SACCADE_ERROR_BUSY;
    }
    bool retired = false;
    for (uint32_t index = 0; index < locator_generation_capacity; ++index) {
        if (state.locator_generations_[index].state != LocatorGenerationState::free &&
            state.locator_generations_[index].key.session_epoch == session_epoch) {
            state.clear_locator_generation(index);
            retired = true;
        }
    }
    return retired ? SACCADE_OK : SACCADE_ERROR_STALE_HANDLE;
}

SaccadeResult AccessibilityProvider::request_press(const AccessibilityPressRequest& request, SaccadeTicketHandle* output) noexcept {
    Impl& state = impl();
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = 0;
    if (!initialized_ || !state.worker_active_)
        return SACCADE_ERROR_STATE;
    const AccessibilityGenerationKey& key = request.generation;
    if (key.session_epoch == 0 || key.frame_id == 0 || key.transform_epoch == 0 || key.topology_epoch == 0 || key.process_id == 0 ||
        key.window_id == 0 || request.target_id == 0 || request.bounds_q8.width <= 0 || request.bounds_q8.height <= 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state.state_.load(std::memory_order_acquire) != WorkState::idle ||
        state.action_state_.load(std::memory_order_acquire) != WorkState::idle) {
        return SACCADE_ERROR_BUSY;
    }
    while (semaphore_timedwait(state.done_semaphore_, {0, 0}) == KERN_SUCCESS) {}
    state.action_request_ = request;
    state.action_ticket_ = state.next_action_ticket_++;
    if (state.next_action_ticket_ == 0 || state.next_action_ticket_ >= ticket_handle_limit)
        state.next_action_ticket_ = 1;
    state.action_status_ = {};
    state.action_status_.ticket = state.action_ticket_;
    state.action_cancel_requested_.store(false, std::memory_order_release);
    state.action_state_.store(WorkState::queued, std::memory_order_release);
    *output = state.action_ticket_;
    semaphore_signal(state.request_semaphore_);
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::cancel_press(SaccadeTicketHandle ticket) noexcept {
    Impl& state = impl();
    if (!initialized_ || ticket == 0 || ticket != state.action_ticket_)
        return SACCADE_ERROR_STALE_HANDLE;
    const WorkState current = state.action_state_.load(std::memory_order_acquire);
    if (current != WorkState::queued && current != WorkState::running)
        return SACCADE_ERROR_STATE;
    state.action_cancel_requested_.store(true, std::memory_order_release);
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::wait_press(SaccadeTicketHandle ticket, uint64_t timeout_ns,
                                                AccessibilityPressStatus* output) noexcept {
    Impl& state = impl();
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    WorkState current = state.action_state_.load(std::memory_order_acquire);
    if (!initialized_ || ticket == 0 || ticket != state.action_ticket_ || current == WorkState::idle)
        return SACCADE_ERROR_STALE_HANDLE;
    if (current == WorkState::queued || current == WorkState::running) {
        const kern_return_t waited = timeout_ns == UINT64_MAX ? semaphore_wait(state.done_semaphore_)
                                                              : semaphore_timedwait(state.done_semaphore_, timeout_from_ns(timeout_ns));
        if (waited == KERN_OPERATION_TIMED_OUT)
            return SACCADE_ERROR_TIMEOUT;
        if (waited != KERN_SUCCESS)
            return SACCADE_ERROR_BACKEND;
    }
    return poll_press(ticket, output);
}

SaccadeResult AccessibilityProvider::poll_press(SaccadeTicketHandle ticket, AccessibilityPressStatus* output) noexcept {
    Impl& state = impl();
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = state.action_state_.load(std::memory_order_acquire);
    if (ticket == 0 || ticket != state.action_ticket_ || current == WorkState::idle)
        return SACCADE_ERROR_STALE_HANDLE;
    *output = state.action_status_;
    if (current == WorkState::queued)
        output->state = SACCADE_TICKET_QUEUED;
    else if (current == WorkState::running)
        output->state = SACCADE_TICKET_RUNNING;
    else
        output->state = SACCADE_TICKET_COMPLETE;
    if (current == WorkState::complete) {
        state.action_ticket_ = 0;
        state.action_state_.store(WorkState::idle, std::memory_order_release);
    }
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

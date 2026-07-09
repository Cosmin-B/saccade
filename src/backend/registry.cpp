#include "backend/registry.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>

namespace saccade::backend {
namespace {

std::atomic<uint32_t> next_registry_domain{1};

uint32_t allocate_registry_domain() noexcept {
    uint32_t current = next_registry_domain.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<uint32_t>::max()) {
        if (next_registry_domain.compare_exchange_weak(
                current,
                current + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

SaccadeResult validate_prefix(
    uint32_t struct_size,
    uint32_t api_version,
    size_t minimum_size) noexcept {
    if (static_cast<size_t>(struct_size) < minimum_size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (api_major(api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    return SACCADE_OK;
}

bool reserved_is_zero(
    const void* object,
    uint32_t struct_size,
    size_t reserved_offset,
    size_t current_size) noexcept {
    const size_t available = std::min(static_cast<size_t>(struct_size), current_size);
    if (available <= reserved_offset) {
        return true;
    }
    const auto* bytes = static_cast<const uint8_t*>(object);
    for (size_t index = reserved_offset; index < available; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

SaccadeResult validate_info(
    const SaccadeProviderInfo& info,
    uint32_t family) noexcept {
    if (static_cast<size_t>(info.struct_size) <
            offsetof(SaccadeProviderInfo, reserved) ||
        static_cast<size_t>(info.struct_size) > sizeof(info)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult prefix = validate_prefix(
        info.struct_size, info.api_version, offsetof(SaccadeProviderInfo, reserved));
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (info.family != family || info.stable_id == 0 ||
        (info.name.size != 0 && info.name.data == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (info.name.size >= 64) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!reserved_is_zero(
            &info, info.struct_size, offsetof(SaccadeProviderInfo, reserved), sizeof(info))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Operations>
SaccadeResult validate_operations_prefix(const Operations& ops) noexcept {
    if (static_cast<size_t>(ops.struct_size) < offsetof(Operations, reserved) ||
        static_cast<size_t>(ops.struct_size) > sizeof(ops)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult prefix = validate_prefix(
        ops.struct_size, ops.api_version, offsetof(Operations, reserved));
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (!reserved_is_zero(
            &ops, ops.struct_size, offsetof(Operations, reserved), sizeof(ops))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult validate_operations(const SaccadeInferenceOps& ops) noexcept {
    const SaccadeResult prefix = validate_operations_prefix(ops);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (ops.enumerate_devices == nullptr || ops.query_model == nullptr ||
        ops.create_model == nullptr || ops.destroy_model == nullptr ||
        ops.create_context == nullptr || ops.destroy_context == nullptr ||
        ops.submit == nullptr || ops.poll == nullptr || ops.wait == nullptr ||
        ops.collect == nullptr || ops.cancel == nullptr || ops.reset == nullptr ||
        ops.synchronize == nullptr || ops.memory_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult validate_operations(const SaccadeCaptureOps& ops) noexcept {
    const SaccadeResult prefix = validate_operations_prefix(ops);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (ops.enumerate_sources == nullptr || ops.create == nullptr ||
        ops.destroy == nullptr || ops.start == nullptr || ops.stop == nullptr ||
        ops.acquire == nullptr || ops.copy_damage == nullptr || ops.release == nullptr ||
        ops.synchronize == nullptr || ops.memory_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult validate_operations(const SaccadeOverlayOps& ops) noexcept {
    const SaccadeResult prefix = validate_operations_prefix(ops);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (ops.create == nullptr || ops.destroy == nullptr || ops.submit == nullptr ||
        ops.set_visible == nullptr || ops.synchronize == nullptr ||
        ops.memory_stats == nullptr || ops.reset == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult validate_operations(const SaccadeAccessibilityOps& ops) noexcept {
    const SaccadeResult prefix = validate_operations_prefix(ops);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (ops.enumerate_windows == nullptr || ops.request == nullptr ||
        ops.poll == nullptr || ops.wait == nullptr || ops.collect == nullptr ||
        ops.cancel == nullptr || ops.release == nullptr || ops.synchronize == nullptr ||
        ops.memory_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult validate_operations(const SaccadeInputOps& ops) noexcept {
    const SaccadeResult prefix = validate_operations_prefix(ops);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (ops.execute == nullptr || ops.poll == nullptr || ops.wait == nullptr ||
        ops.cancel == nullptr || ops.release_all == nullptr ||
        ops.synchronize == nullptr || ops.reset == nullptr ||
        ops.memory_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Descriptor>
SaccadeResult copy_descriptor_prefix(
    const Descriptor* desc,
    Descriptor* out_desc) noexcept {
    if (desc == nullptr || out_desc == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t struct_size = 0;
    std::memcpy(&struct_size, static_cast<const void*>(desc), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Descriptor, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *out_desc = {};
    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(*out_desc));
    std::memcpy(out_desc, static_cast<const void*>(desc), copy_size);
    const SaccadeResult prefix = validate_prefix(
        struct_size, out_desc->api_version, offsetof(Descriptor, reserved));
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    if (!reserved_is_zero(
            out_desc, struct_size, offsetof(Descriptor, reserved), sizeof(*out_desc))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeProviderInfo normalized_info(const SaccadeProviderInfo& source) noexcept {
    SaccadeProviderInfo result{};
    const size_t size = std::min(static_cast<size_t>(source.struct_size), sizeof(result));
    std::memcpy(&result, &source, size);
    result.struct_size = static_cast<uint32_t>(sizeof(result));
    result.api_version = SACCADE_API_VERSION;
    return result;
}

template <typename Descriptor>
SaccadeResult copy_and_validate_descriptor(
    const Descriptor* desc,
    uint32_t family,
    Descriptor* out_desc) noexcept {
    const SaccadeResult prefix = copy_descriptor_prefix(desc, out_desc);
    if (prefix != SACCADE_OK) {
        return prefix;
    }
    const SaccadeResult info = validate_info(out_desc->info, family);
    if (info != SACCADE_OK) {
        return info;
    }
    return validate_operations(out_desc->ops);
}

template <typename Operations>
Operations normalized_operations(const Operations& source) noexcept {
    Operations result{};
    const size_t size = std::min(static_cast<size_t>(source.struct_size), sizeof(result));
    std::memcpy(&result, &source, size);
    result.struct_size = static_cast<uint32_t>(sizeof(result));
    result.api_version = SACCADE_API_VERSION;
    return result;
}

uint32_t count_bits(uint32_t value) noexcept {
    uint32_t count = 0;
    while (value != 0) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

SaccadeProviderHandle make_handle(
    uint32_t domain,
    uint32_t family,
    size_t slot) noexcept {
    return (static_cast<uint64_t>(domain) << 32U) |
           (static_cast<uint64_t>(family) << 16U) |
           (static_cast<uint64_t>(slot) + UINT64_C(1));
}

struct DecodedHandle {
    size_t slot = 0;
    uint32_t family = 0;
    uint32_t domain = 0;
    bool valid = false;
};

DecodedHandle decode_handle(SaccadeProviderHandle handle) noexcept {
    const uint32_t slot = static_cast<uint32_t>(handle & UINT64_C(0xFFFF));
    const uint32_t family = static_cast<uint32_t>((handle >> 16U) & UINT64_C(0xFF));
    const uint32_t reserved = static_cast<uint32_t>((handle >> 24U) & UINT64_C(0xFF));
    const uint32_t domain = static_cast<uint32_t>(handle >> 32U);
    if (slot == 0 || reserved != 0 || domain == 0 ||
        static_cast<size_t>(slot) > ProviderRegistry::capacity_per_family) {
        return {};
    }
    return {static_cast<size_t>(slot - 1U), family, domain, true};
}

}  // namespace

ProviderRegistry::ProviderRegistry() noexcept
    : domain_(allocate_registry_domain()) {}

template <typename Operations>
SaccadeResult ProviderRegistry::insert(
    FamilyStore<Operations>& store,
    uint32_t family,
    const SaccadeProviderInfo& info,
    void* context,
    const Operations& ops,
    SaccadeProviderHandle* out_handle) noexcept {
    if (out_handle != nullptr) {
        *out_handle = 0;
    }
    if (domain_ == 0) {
        return SACCADE_ERROR_CAPACITY;
    }
    for (const Slot<Operations>& slot : store.slots) {
        if (slot.occupied && slot.record.info.stable_id == info.stable_id) {
            return SACCADE_ERROR_ALREADY_EXISTS;
        }
    }
    if (store.size == capacity_per_family) {
        return SACCADE_ERROR_CAPACITY;
    }

    for (size_t index = 0; index < capacity_per_family; ++index) {
        Slot<Operations>& slot = store.slots[index];
        if (slot.occupied) {
            continue;
        }

        slot.record = {};
        slot.record.handle = make_handle(domain_, family, index);
        slot.record.registration_order = next_registration_order_++;
        slot.record.context = context;
        slot.record.info = normalized_info(info);
        if (info.name.size != 0) {
            std::memcpy(slot.record.name.data(), info.name.data, info.name.size);
        }
        slot.record.name[info.name.size] = 0;
        slot.record.info.name = {slot.record.name.data(), info.name.size};
        slot.record.ops = ops;
        slot.occupied = true;
        ++store.size;
        if (out_handle != nullptr) {
            *out_handle = slot.record.handle;
        }
        return SACCADE_OK;
    }
    return SACCADE_ERROR_CAPACITY;
}

template <typename Operations>
SaccadeResult ProviderRegistry::select(
    const FamilyStore<Operations>& store,
    uint32_t family,
    uint32_t required,
    uint32_t preferred,
    ProviderSelection* out_selection) const noexcept {
    if (out_selection == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_selection = {};

    const Slot<Operations>* best = nullptr;
    uint32_t best_score = 0;
    for (const Slot<Operations>& slot : store.slots) {
        if (!slot.occupied) {
            continue;
        }
        const uint32_t capabilities = slot.record.info.capability_bits;
        if ((capabilities & required) != required) {
            continue;
        }
        const uint32_t score = count_bits(capabilities & preferred);
        if (best == nullptr || score > best_score) {
            best = &slot;
            best_score = score;
        }
    }
    if (best == nullptr) {
        return SACCADE_ERROR_NOT_FOUND;
    }

    out_selection->handle = best->record.handle;
    out_selection->stable_id = best->record.info.stable_id;
    out_selection->capability_bits = best->record.info.capability_bits;
    out_selection->reason = best_score == 0
        ? SelectionReason::registration_order
        : SelectionReason::preferred_capability;
    (void)family;
    return SACCADE_OK;
}

template <typename Operations>
SaccadeResult ProviderRegistry::select_by_id(
    const FamilyStore<Operations>& store,
    uint32_t family,
    uint64_t stable_id,
    ProviderSelection* out_selection) const noexcept {
    if (out_selection == nullptr || stable_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_selection = {};
    for (const Slot<Operations>& slot : store.slots) {
        if (slot.occupied && slot.record.info.stable_id == stable_id) {
            out_selection->handle = slot.record.handle;
            out_selection->stable_id = stable_id;
            out_selection->capability_bits = slot.record.info.capability_bits;
            out_selection->reason = SelectionReason::explicit_id;
            (void)family;
            return SACCADE_OK;
        }
    }
    return SACCADE_ERROR_NOT_FOUND;
}

template <typename Operations>
const ProviderRecord<Operations>* ProviderRegistry::lookup(
    const FamilyStore<Operations>& store,
    uint32_t family,
    SaccadeProviderHandle handle) const noexcept {
    const DecodedHandle decoded = decode_handle(handle);
    if (!decoded.valid || decoded.family != family || decoded.domain != domain_) {
        return nullptr;
    }
    const Slot<Operations>& slot = store.slots[decoded.slot];
    if (!slot.occupied || slot.record.handle != handle) {
        return nullptr;
    }
    return &slot.record;
}

#define SACCADE_DEFINE_REGISTRATION(method, member, family_value, descriptor_type) \
    SaccadeResult ProviderRegistry::method(                                    \
        const descriptor_type* desc, SaccadeProviderHandle* out_handle) noexcept { \
        if (frozen_) {                                                         \
            return SACCADE_ERROR_STATE;                                        \
        }                                                                      \
        descriptor_type normalized_desc{};                                     \
        const SaccadeResult validation = copy_and_validate_descriptor(          \
            desc, family_value, &normalized_desc);                              \
        if (validation != SACCADE_OK) {                                        \
            return validation;                                                 \
        }                                                                      \
        return insert(member, family_value, normalized_desc.info,               \
                      normalized_desc.context,                                 \
                      normalized_operations(normalized_desc.ops), out_handle);  \
    }

SACCADE_DEFINE_REGISTRATION(
    register_inference, inference_, SACCADE_PROVIDER_FAMILY_INFERENCE,
    SaccadeInferenceProviderDesc)
SACCADE_DEFINE_REGISTRATION(
    register_capture, capture_, SACCADE_PROVIDER_FAMILY_CAPTURE,
    SaccadeCaptureProviderDesc)
SACCADE_DEFINE_REGISTRATION(
    register_overlay, overlay_, SACCADE_PROVIDER_FAMILY_OVERLAY,
    SaccadeOverlayProviderDesc)
SACCADE_DEFINE_REGISTRATION(
    register_accessibility, accessibility_, SACCADE_PROVIDER_FAMILY_ACCESSIBILITY,
    SaccadeAccessibilityProviderDesc)
SACCADE_DEFINE_REGISTRATION(
    register_input, input_, SACCADE_PROVIDER_FAMILY_INPUT,
    SaccadeInputProviderDesc)

#undef SACCADE_DEFINE_REGISTRATION

#define SACCADE_DEFINE_SELECTION(method, member, family_value)                  \
    SaccadeResult ProviderRegistry::method(                                    \
        uint32_t required, uint32_t preferred,                                 \
        ProviderSelection* out_selection) const noexcept {                     \
        return select(member, family_value, required, preferred, out_selection); \
    }

SACCADE_DEFINE_SELECTION(select_inference, inference_, SACCADE_PROVIDER_FAMILY_INFERENCE)
SACCADE_DEFINE_SELECTION(select_capture, capture_, SACCADE_PROVIDER_FAMILY_CAPTURE)
SACCADE_DEFINE_SELECTION(select_overlay, overlay_, SACCADE_PROVIDER_FAMILY_OVERLAY)
SACCADE_DEFINE_SELECTION(
    select_accessibility, accessibility_, SACCADE_PROVIDER_FAMILY_ACCESSIBILITY)
SACCADE_DEFINE_SELECTION(select_input, input_, SACCADE_PROVIDER_FAMILY_INPUT)

#undef SACCADE_DEFINE_SELECTION

#define SACCADE_DEFINE_ID_SELECTION(method, member, family_value)               \
    SaccadeResult ProviderRegistry::method(                                    \
        uint64_t stable_id, ProviderSelection* out_selection) const noexcept { \
        return select_by_id(member, family_value, stable_id, out_selection);    \
    }

SACCADE_DEFINE_ID_SELECTION(
    select_inference_by_id, inference_, SACCADE_PROVIDER_FAMILY_INFERENCE)
SACCADE_DEFINE_ID_SELECTION(
    select_capture_by_id, capture_, SACCADE_PROVIDER_FAMILY_CAPTURE)
SACCADE_DEFINE_ID_SELECTION(
    select_overlay_by_id, overlay_, SACCADE_PROVIDER_FAMILY_OVERLAY)
SACCADE_DEFINE_ID_SELECTION(
    select_accessibility_by_id, accessibility_, SACCADE_PROVIDER_FAMILY_ACCESSIBILITY)
SACCADE_DEFINE_ID_SELECTION(select_input_by_id, input_, SACCADE_PROVIDER_FAMILY_INPUT)

#undef SACCADE_DEFINE_ID_SELECTION

#define SACCADE_DEFINE_LOOKUP(method, record_type, member, family_value)         \
    const ProviderRegistry::record_type* ProviderRegistry::method(              \
        SaccadeProviderHandle handle) const noexcept {                          \
        return lookup(member, family_value, handle);                            \
    }

SACCADE_DEFINE_LOOKUP(
    inference, InferenceRecord, inference_, SACCADE_PROVIDER_FAMILY_INFERENCE)
SACCADE_DEFINE_LOOKUP(capture, CaptureRecord, capture_, SACCADE_PROVIDER_FAMILY_CAPTURE)
SACCADE_DEFINE_LOOKUP(overlay, OverlayRecord, overlay_, SACCADE_PROVIDER_FAMILY_OVERLAY)
SACCADE_DEFINE_LOOKUP(
    accessibility, AccessibilityRecord, accessibility_,
    SACCADE_PROVIDER_FAMILY_ACCESSIBILITY)
SACCADE_DEFINE_LOOKUP(input, InputRecord, input_, SACCADE_PROVIDER_FAMILY_INPUT)

#undef SACCADE_DEFINE_LOOKUP

void ProviderRegistry::freeze() noexcept {
    frozen_ = true;
}

bool ProviderRegistry::frozen() const noexcept {
    return frozen_;
}

}  // namespace saccade::backend

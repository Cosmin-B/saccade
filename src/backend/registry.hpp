#ifndef SACCADE_BACKEND_REGISTRY_HPP
#define SACCADE_BACKEND_REGISTRY_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend {

enum class SelectionReason : uint32_t { registration_order = 1, preferred_capability = 2, explicit_id = 3 };

struct ProviderSelection {
    SaccadeProviderHandle handle = 0;
    uint64_t stable_id = 0;
    uint32_t capability_bits = 0;
    SelectionReason reason = SelectionReason::registration_order;
};

struct DeviceRequirements {
    uint32_t required_capability_bits = 0;
    uint32_t preferred_capability_bits = 0;
    uint32_t required_format_bits = 0;
    uint32_t preferred_format_bits = 0;
    uint32_t required_precision_bits = 0;
    uint32_t preferred_precision_bits = 0;
    uint32_t required_import_bits = 0;
    uint32_t preferred_import_bits = 0;
    uint32_t minimum_queue_capacity = 0;
    uint32_t minimum_max_in_flight = 0;
};

struct DeviceSelection {
    SaccadeDeviceHandle handle = 0;
    SaccadeProviderHandle provider = 0;
    uint64_t stable_id = 0;
    uint32_t capability_bits = 0;
    SelectionReason reason = SelectionReason::registration_order;
};

struct DeviceRecord {
    SaccadeDeviceHandle handle = 0;
    SaccadeProviderHandle provider = 0;
    uint64_t registration_order = 0;
    SaccadeDeviceInfo info{};
    std::array<uint8_t, 64> name{};
};

template <typename Operations> struct ProviderRecord {
    SaccadeProviderHandle handle = 0;
    uint64_t registration_order = 0;
    void* context = nullptr;
    SaccadeProviderInfo info{};
    Operations ops{};
    std::array<uint8_t, 64> name{};
};

class ProviderRegistry final {
  public:
    static constexpr size_t capacity_per_family = 8;
    static constexpr size_t device_capacity = 32;

    using InferenceRecord = ProviderRecord<SaccadeInferenceOps>;
    using CaptureRecord = ProviderRecord<SaccadeCaptureOps>;
    using OverlayRecord = ProviderRecord<SaccadeOverlayOps>;
    using AccessibilityRecord = ProviderRecord<SaccadeAccessibilityOps>;
    using InputRecord = ProviderRecord<SaccadeInputOps>;

    ProviderRegistry() noexcept;
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;
    ProviderRegistry(ProviderRegistry&&) = delete;
    ProviderRegistry& operator=(ProviderRegistry&&) = delete;

    SaccadeResult register_inference(const SaccadeInferenceProviderDesc*, SaccadeProviderHandle*) noexcept;
    SaccadeResult register_capture(const SaccadeCaptureProviderDesc*, SaccadeProviderHandle*) noexcept;
    SaccadeResult register_overlay(const SaccadeOverlayProviderDesc*, SaccadeProviderHandle*) noexcept;
    SaccadeResult register_accessibility(const SaccadeAccessibilityProviderDesc*, SaccadeProviderHandle*) noexcept;
    SaccadeResult register_input(const SaccadeInputProviderDesc*, SaccadeProviderHandle*) noexcept;
    SaccadeResult register_device(SaccadeProviderHandle, const SaccadeDeviceInfo*, SaccadeDeviceHandle*) noexcept;

    SaccadeResult select_inference(uint32_t, uint32_t, ProviderSelection*) const noexcept;
    SaccadeResult select_capture(uint32_t, uint32_t, ProviderSelection*) const noexcept;
    SaccadeResult select_overlay(uint32_t, uint32_t, ProviderSelection*) const noexcept;
    SaccadeResult select_accessibility(uint32_t, uint32_t, ProviderSelection*) const noexcept;
    SaccadeResult select_input(uint32_t, uint32_t, ProviderSelection*) const noexcept;

    SaccadeResult select_inference_by_id(uint64_t, ProviderSelection*) const noexcept;
    SaccadeResult select_capture_by_id(uint64_t, ProviderSelection*) const noexcept;
    SaccadeResult select_overlay_by_id(uint64_t, ProviderSelection*) const noexcept;
    SaccadeResult select_accessibility_by_id(uint64_t, ProviderSelection*) const noexcept;
    SaccadeResult select_input_by_id(uint64_t, ProviderSelection*) const noexcept;
    SaccadeResult select_device(const DeviceRequirements&, DeviceSelection*) const noexcept;
    SaccadeResult select_device_by_id(SaccadeProviderHandle, uint64_t, DeviceSelection*) const noexcept;

    const InferenceRecord* inference(SaccadeProviderHandle) const noexcept;
    const CaptureRecord* capture(SaccadeProviderHandle) const noexcept;
    const OverlayRecord* overlay(SaccadeProviderHandle) const noexcept;
    const AccessibilityRecord* accessibility(SaccadeProviderHandle) const noexcept;
    const InputRecord* input(SaccadeProviderHandle) const noexcept;
    const DeviceRecord* device(SaccadeDeviceHandle) const noexcept;

    void freeze() noexcept;
    [[nodiscard]] bool frozen() const noexcept;

  private:
    template <typename Operations> struct Slot {
        ProviderRecord<Operations> record{};
        bool occupied = false;
    };

    template <typename Operations> struct FamilyStore {
        std::array<Slot<Operations>, capacity_per_family> slots{};
        size_t size = 0;
    };

    struct DeviceSlot {
        DeviceRecord record{};
        bool occupied = false;
    };

    template <typename Operations>
    SaccadeResult insert(FamilyStore<Operations>&, uint32_t, const SaccadeProviderInfo&, void*, const Operations&,
                         SaccadeProviderHandle*) noexcept;

    template <typename Operations>
    SaccadeResult select(const FamilyStore<Operations>&, uint32_t, uint32_t, uint32_t,
                         ProviderSelection*) const noexcept;

    template <typename Operations>
    SaccadeResult select_by_id(const FamilyStore<Operations>&, uint32_t, uint64_t, ProviderSelection*) const noexcept;

    template <typename Operations>
    const ProviderRecord<Operations>* lookup(const FamilyStore<Operations>&, uint32_t,
                                             SaccadeProviderHandle) const noexcept;

    FamilyStore<SaccadeInferenceOps> inference_{};
    FamilyStore<SaccadeCaptureOps> capture_{};
    FamilyStore<SaccadeOverlayOps> overlay_{};
    FamilyStore<SaccadeAccessibilityOps> accessibility_{};
    FamilyStore<SaccadeInputOps> input_{};
    std::array<DeviceSlot, device_capacity> devices_{};
    size_t device_size_ = 0;
    uint64_t next_registration_order_ = 1;
    uint64_t next_device_registration_order_ = 1;
    uint32_t domain_ = 0;
    bool frozen_ = false;
};

} // namespace saccade::backend

#endif

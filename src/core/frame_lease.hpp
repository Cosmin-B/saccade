#ifndef SACCADE_CORE_FRAME_LEASE_HPP
#define SACCADE_CORE_FRAME_LEASE_HPP

#include "core/frame_validation.hpp"
#include "core/handle_table.hpp"

#include <saccade/saccade.h>

#include <cstddef>
#include <cstdint>

namespace saccade::core {

enum class FrameLeaseOwner : uint8_t {
    caller = UINT8_C(1) << 0U,
    mailbox = UINT8_C(1) << 1U,
    worker = UINT8_C(1) << 2U
};

template <size_t Capacity>
class FrameLeasePool;

class FrameLease final {
public:
    explicit FrameLease(const SaccadeHostFrameDesc& desc) noexcept
        : data_(desc.data.data),
          byte_size_(desc.data.size),
          width_(desc.width),
          height_(desc.height),
          row_stride_bytes_(desc.row_stride_bytes),
          pixel_format_(desc.pixel_format),
          frame_id_(desc.frame_id),
          transform_epoch_(desc.transform_epoch),
          owners_(owner_bit(FrameLeaseOwner::caller)) {}

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&&) = delete;
    FrameLease& operator=(FrameLease&&) = delete;

    [[nodiscard]] const uint8_t* data() const noexcept {
        return data_;
    }

    [[nodiscard]] size_t byte_size() const noexcept {
        return byte_size_;
    }

    [[nodiscard]] uint32_t width() const noexcept {
        return width_;
    }

    [[nodiscard]] uint32_t height() const noexcept {
        return height_;
    }

    [[nodiscard]] uint32_t row_stride_bytes() const noexcept {
        return row_stride_bytes_;
    }

    [[nodiscard]] uint32_t pixel_format() const noexcept {
        return pixel_format_;
    }

    [[nodiscard]] uint64_t frame_id() const noexcept {
        return frame_id_;
    }

    [[nodiscard]] uint64_t transform_epoch() const noexcept {
        return transform_epoch_;
    }

    [[nodiscard]] bool has_owner(FrameLeaseOwner owner) const noexcept {
        return (owners_ & owner_bit(owner)) != 0;
    }

private:
    template <size_t>
    friend class FrameLeasePool;

    static constexpr uint8_t owner_bit(FrameLeaseOwner owner) noexcept {
        return static_cast<uint8_t>(owner);
    }

    static constexpr bool valid_owner(FrameLeaseOwner owner) noexcept {
        return owner == FrameLeaseOwner::caller || owner == FrameLeaseOwner::mailbox ||
               owner == FrameLeaseOwner::worker;
    }

    bool add_owner(FrameLeaseOwner owner) noexcept {
        const uint8_t bit = owner_bit(owner);
        if ((owners_ & bit) != 0) {
            return false;
        }
        owners_ = static_cast<uint8_t>(owners_ | bit);
        return true;
    }

    bool remove_owner(FrameLeaseOwner owner) noexcept {
        const uint8_t bit = owner_bit(owner);
        if ((owners_ & bit) == 0) {
            return false;
        }
        owners_ = static_cast<uint8_t>(owners_ & static_cast<uint8_t>(~bit));
        return true;
    }

    [[nodiscard]] bool has_owners() const noexcept {
        return owners_ != 0;
    }

    const uint8_t* data_ = nullptr;
    size_t byte_size_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t row_stride_bytes_ = 0;
    uint32_t pixel_format_ = 0;
    uint64_t frame_id_ = 0;
    uint64_t transform_epoch_ = 0;
    uint8_t owners_ = 0;
};

template <size_t Capacity>
class FrameLeasePool final {
    static_assert(Capacity > 0);
    static_assert(Capacity <= UINT8_MAX);

public:
    explicit FrameLeasePool(uint32_t domain) noexcept : domain_(domain) {}
    FrameLeasePool(const FrameLeasePool&) = delete;
    FrameLeasePool& operator=(const FrameLeasePool&) = delete;
    FrameLeasePool(FrameLeasePool&&) = delete;
    FrameLeasePool& operator=(FrameLeasePool&&) = delete;

    SaccadeResult import_host(
        const SaccadeHostFrameDesc& desc,
        SaccadeFrameHandle* out_frame) noexcept {
        if (out_frame == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_frame = 0;
        if (domain_ == 0 || domain_ > max_domain_ || !valid_host_frame(desc)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        SaccadeFrameHandle local_frame = 0;
        const SaccadeResult result = leases_.emplace(&local_frame, desc);
        if (result == SACCADE_OK) {
            *out_frame = encode(local_frame);
        }
        return result;
    }

    SaccadeResult add_owner(
        SaccadeFrameHandle frame,
        FrameLeaseOwner owner) noexcept {
        if (!FrameLease::valid_owner(owner)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        FrameLease* lease = leases_.get(decode(frame));
        if (lease == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        if (!lease->add_owner(owner)) {
            return SACCADE_ERROR_ALREADY_EXISTS;
        }
        return SACCADE_OK;
    }

    SaccadeResult release_owner(
        SaccadeFrameHandle frame,
        FrameLeaseOwner owner) noexcept {
        if (!FrameLease::valid_owner(owner)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        const SaccadeFrameHandle local_frame = decode(frame);
        FrameLease* lease = leases_.get(local_frame);
        if (lease == nullptr || !lease->remove_owner(owner)) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        if (lease->has_owners()) {
            return SACCADE_OK;
        }
        return leases_.erase(local_frame);
    }

    [[nodiscard]] FrameLease* get(SaccadeFrameHandle frame) noexcept {
        return leases_.get(decode(frame));
    }

    [[nodiscard]] const FrameLease* get(SaccadeFrameHandle frame) const noexcept {
        return leases_.get(decode(frame));
    }

    void clear() noexcept {
        leases_.clear_reverse([](uint64_t, FrameLease&) noexcept {});
    }

    [[nodiscard]] size_t size() const noexcept {
        return leases_.size();
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    [[nodiscard]] static constexpr uint32_t maximum_domain() noexcept {
        return max_domain_;
    }

private:
    static constexpr uint32_t max_domain_ = UINT32_C(0x00FFFFFF);
    static constexpr uint64_t domain_shift_ = 40;
    static constexpr uint64_t generation_shift_ = 8;
    static constexpr uint64_t slot_mask_ = UINT64_C(0xFF);
    static constexpr uint64_t generation_mask_ = UINT64_C(0xFFFFFFFF);

    [[nodiscard]] SaccadeFrameHandle encode(
        SaccadeFrameHandle local_frame) const noexcept {
        const uint64_t slot = local_frame & UINT64_C(0xFFFFFFFF);
        const uint64_t generation = local_frame >> 32U;
        return (static_cast<uint64_t>(domain_) << domain_shift_) |
               (generation << generation_shift_) | slot;
    }

    [[nodiscard]] SaccadeFrameHandle decode(SaccadeFrameHandle frame) const noexcept {
        const uint32_t domain = static_cast<uint32_t>(frame >> domain_shift_);
        if (domain == 0 || domain != domain_) {
            return 0;
        }
        const uint64_t slot = frame & slot_mask_;
        const uint64_t generation =
            (frame >> generation_shift_) & generation_mask_;
        if (slot == 0 || generation == 0) {
            return 0;
        }
        return (generation << 32U) | slot;
    }

    HandleTable<FrameLease, Capacity> leases_{};
    uint32_t domain_ = 0;
};

}  // namespace saccade::core

#endif

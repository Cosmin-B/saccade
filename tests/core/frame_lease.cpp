#include "core/frame_lease.hpp"
#include "../support/allocation_tracker.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

SaccadeHostFrameDesc host_frame(const uint8_t* data, size_t size, uint64_t frame_id) {
    SaccadeHostFrameDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.data = {data, size};
    desc.width = 2;
    desc.height = 2;
    desc.row_stride_bytes = 8;
    desc.pixel_format = SACCADE_FORMAT_BGRA8;
    desc.frame_id = frame_id;
    desc.transform_epoch = 7;
    return desc;
}

}  // namespace

int main() {
    using saccade::core::FrameLeaseOwner;
    using Pool = saccade::core::FrameLeasePool<2>;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    std::array<uint8_t, 16> first_pixels{};
    std::array<uint8_t, 16> second_pixels{};
    Pool pool{1};
    SaccadeFrameHandle first = 0;
    const SaccadeHostFrameDesc first_desc =
        host_frame(first_pixels.data(), first_pixels.size(), 41);
    Pool invalid_pool{0};
    SaccadeFrameHandle invalid_frame = 99;
    if (invalid_pool.import_host(first_desc, &invalid_frame) !=
            SACCADE_ERROR_INVALID_ARGUMENT ||
        invalid_frame != 0) {
        return 17;
    }
    if (pool.import_host(first_desc, &first) != SACCADE_OK || first == 0 || pool.size() != 1) {
        return 2;
    }

    const saccade::core::FrameLease* lease = pool.get(first);
    if (lease == nullptr || lease->data() != first_pixels.data() ||
        lease->byte_size() != first_pixels.size() || lease->width() != 2 || lease->height() != 2 ||
        lease->row_stride_bytes() != 8 || lease->pixel_format() != SACCADE_FORMAT_BGRA8 ||
        lease->frame_id() != 41 || lease->transform_epoch() != 7 ||
        !lease->has_owner(FrameLeaseOwner::caller)) {
        return 3;
    }

    Pool other_pool{2};
    SaccadeFrameHandle other_domain = 0;
    if (other_pool.import_host(first_desc, &other_domain) != SACCADE_OK ||
        other_domain == 0 || other_domain == first ||
        pool.get(other_domain) != nullptr || other_pool.get(first) != nullptr ||
        other_pool.release_owner(other_domain, FrameLeaseOwner::caller) != SACCADE_OK) {
        return 4;
    }

    if (pool.add_owner(first, FrameLeaseOwner::mailbox) != SACCADE_OK ||
        pool.add_owner(first, FrameLeaseOwner::mailbox) != SACCADE_ERROR_ALREADY_EXISTS ||
        pool.release_owner(first, FrameLeaseOwner::caller) != SACCADE_OK ||
        pool.release_owner(first, FrameLeaseOwner::caller) != SACCADE_ERROR_STALE_HANDLE ||
        pool.get(first) == nullptr || pool.size() != 1) {
        return 5;
    }
    if (pool.release_owner(first, FrameLeaseOwner::mailbox) != SACCADE_OK ||
        pool.get(first) != nullptr || pool.size() != 0 ||
        pool.release_owner(first, FrameLeaseOwner::mailbox) != SACCADE_ERROR_STALE_HANDLE) {
        return 6;
    }

    SaccadeFrameHandle regenerated = 0;
    if (pool.import_host(first_desc, &regenerated) != SACCADE_OK || regenerated == first ||
        pool.get(first) != nullptr) {
        return 7;
    }

    SaccadeFrameHandle second = 0;
    const SaccadeHostFrameDesc second_desc =
        host_frame(second_pixels.data(), second_pixels.size(), 42);
    if (pool.import_host(second_desc, &second) != SACCADE_OK || second == 0) {
        return 8;
    }
    SaccadeFrameHandle overflow = 99;
    if (pool.import_host(second_desc, &overflow) != SACCADE_ERROR_CAPACITY || overflow != 0 ||
        pool.size() != Pool::capacity()) {
        return 9;
    }

    pool.clear();
    if (pool.size() != 0 || pool.get(regenerated) != nullptr || pool.get(second) != nullptr) {
        return 10;
    }

    std::array<uint8_t, 20> padded_pixels{};
    SaccadeHostFrameDesc padded_desc =
        host_frame(padded_pixels.data(), padded_pixels.size(), 43);
    padded_desc.row_stride_bytes = 12;
    SaccadeFrameHandle padded = 0;
    if (pool.import_host(padded_desc, &padded) != SACCADE_OK || padded == 0 ||
        pool.release_owner(padded, FrameLeaseOwner::caller) != SACCADE_OK) {
        return 11;
    }
    padded_desc.data.size = padded_pixels.size() - 1;
    padded = 99;
    if (pool.import_host(padded_desc, &padded) != SACCADE_ERROR_INVALID_ARGUMENT ||
        padded != 0) {
        return 12;
    }

    SaccadeHostFrameDesc malformed = first_desc;
    malformed.row_stride_bytes = 2;
    padded = 99;
    if (pool.import_host(malformed, &padded) != SACCADE_ERROR_INVALID_ARGUMENT ||
        padded != 0) {
        return 13;
    }
    malformed = first_desc;
    malformed.pixel_format = SACCADE_FORMAT_BGRA8 | SACCADE_FORMAT_RGBA8;
    if (pool.import_host(malformed, &padded) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 14;
    }
    malformed.pixel_format = UINT32_C(1) << 31U;
    if (pool.import_host(malformed, &padded) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 15;
    }

    saccade::test::begin_allocation_tracking();
    SaccadeFrameHandle measured = 0;
    const SaccadeResult import_result = pool.import_host(first_desc, &measured);
    const SaccadeResult add_result = pool.add_owner(measured, FrameLeaseOwner::mailbox);
    const SaccadeResult caller_release = pool.release_owner(measured, FrameLeaseOwner::caller);
    const SaccadeResult mailbox_release = pool.release_owner(measured, FrameLeaseOwner::mailbox);
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (import_result != SACCADE_OK || add_result != SACCADE_OK || caller_release != SACCADE_OK ||
        mailbox_release != SACCADE_OK || allocations != 0) {
        return 16;
    }

    return 0;
}

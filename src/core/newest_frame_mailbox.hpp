#ifndef SACCADE_CORE_NEWEST_FRAME_MAILBOX_HPP
#define SACCADE_CORE_NEWEST_FRAME_MAILBOX_HPP

#include "core/cache_line.hpp"

#include <saccade/saccade.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace saccade::core {

struct NewestFrameMailboxStats {
    uint64_t published = 0;
    uint64_t replaced = 0;
    uint64_t consumed = 0;
    uint64_t discarded = 0;
};

// Single producer calls replace, one consumer calls take, and control operations
// run only while both sides are quiescent.
class NewestFrameMailbox final {
    static_assert(std::atomic<SaccadeFrameHandle>::is_always_lock_free);

  public:
    NewestFrameMailbox() = default;
    NewestFrameMailbox(const NewestFrameMailbox&) = delete;
    NewestFrameMailbox& operator=(const NewestFrameMailbox&) = delete;
    NewestFrameMailbox(NewestFrameMailbox&&) = delete;
    NewestFrameMailbox& operator=(NewestFrameMailbox&&) = delete;

    [[nodiscard]] SaccadeFrameHandle replace(SaccadeFrameHandle frame) noexcept {
        if (frame == 0) {
            return 0;
        }
        const SaccadeFrameHandle replaced = pending_.frame.exchange(frame, std::memory_order_acq_rel);
        ++producer_.published;
        if (replaced != 0) {
            ++producer_.replaced;
        }
        return replaced;
    }

    [[nodiscard]] SaccadeFrameHandle take() noexcept {
        const SaccadeFrameHandle frame = pending_.frame.exchange(0, std::memory_order_acq_rel);
        if (frame != 0) {
            ++consumer_.consumed;
        }
        return frame;
    }

    bool remove_quiescent(SaccadeFrameHandle frame) noexcept {
        if (frame == 0) {
            return false;
        }
        // Control-path cancellation must be serialized against replace and take.
        if (pending_.frame.load(std::memory_order_acquire) != frame) {
            return false;
        }
        pending_.frame.store(0, std::memory_order_release);
        ++consumer_.discarded;
        return true;
    }

    [[nodiscard]] SaccadeFrameHandle clear_quiescent() noexcept {
        const SaccadeFrameHandle frame = pending_.frame.exchange(0, std::memory_order_acq_rel);
        if (frame != 0) {
            ++consumer_.discarded;
        }
        return frame;
    }

    [[nodiscard]] NewestFrameMailboxStats stats_quiescent() const noexcept {
        return {producer_.published, producer_.replaced, consumer_.consumed, consumer_.discarded};
    }

  private:
    struct alignas(destructive_interference_size) Pending {
        std::atomic<SaccadeFrameHandle> frame{0};
        std::array<std::byte, destructive_interference_size - sizeof(std::atomic<SaccadeFrameHandle>)> padding{};
    };

    struct alignas(destructive_interference_size) ProducerCounters {
        uint64_t published = 0;
        uint64_t replaced = 0;
        std::array<std::byte, destructive_interference_size - 2U * sizeof(uint64_t)> padding{};
    };

    struct alignas(destructive_interference_size) ConsumerCounters {
        uint64_t consumed = 0;
        uint64_t discarded = 0;
        std::array<std::byte, destructive_interference_size - 2U * sizeof(uint64_t)> padding{};
    };

    Pending pending_{};
    ProducerCounters producer_{};
    ConsumerCounters consumer_{};
};

static_assert(sizeof(NewestFrameMailbox) == 3U * destructive_interference_size);

} // namespace saccade::core

#endif

#ifndef SACCADE_CORE_NEWEST_FRAME_MAILBOX_HPP
#define SACCADE_CORE_NEWEST_FRAME_MAILBOX_HPP

#include <saccade/saccade.h>

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
        const SaccadeFrameHandle replaced =
            pending_.exchange(frame, std::memory_order_acq_rel);
        ++published_;
        if (replaced != 0) {
            ++replaced_;
        }
        return replaced;
    }

    [[nodiscard]] SaccadeFrameHandle take() noexcept {
        const SaccadeFrameHandle frame =
            pending_.exchange(0, std::memory_order_acq_rel);
        if (frame != 0) {
            ++consumed_;
        }
        return frame;
    }

    bool remove_quiescent(SaccadeFrameHandle frame) noexcept {
        if (frame == 0) {
            return false;
        }
        // Control-path cancellation must be serialized against replace and take.
        if (pending_.load(std::memory_order_acquire) != frame) {
            return false;
        }
        pending_.store(0, std::memory_order_release);
        ++discarded_;
        return true;
    }

    [[nodiscard]] SaccadeFrameHandle clear_quiescent() noexcept {
        const SaccadeFrameHandle frame =
            pending_.exchange(0, std::memory_order_acq_rel);
        if (frame != 0) {
            ++discarded_;
        }
        return frame;
    }

    [[nodiscard]] NewestFrameMailboxStats stats_quiescent() const noexcept {
        return {published_, replaced_, consumed_, discarded_};
    }

private:
    static constexpr size_t cache_line_size_ = 64;

    alignas(cache_line_size_) std::atomic<SaccadeFrameHandle> pending_{0};
    alignas(cache_line_size_) uint64_t published_ = 0;
    uint64_t replaced_ = 0;
    alignas(cache_line_size_) uint64_t consumed_ = 0;
    alignas(cache_line_size_) uint64_t discarded_ = 0;
};

}  // namespace saccade::core

#endif

#include "core/newest_frame_mailbox.hpp"
#include "../support/allocation_tracker.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

int main() {
    using saccade::core::NewestFrameMailbox;

    static_assert(!std::is_copy_constructible_v<NewestFrameMailbox>);
    static_assert(!std::is_move_constructible_v<NewestFrameMailbox>);

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    NewestFrameMailbox mailbox;
    if (mailbox.replace(1) != 0 || mailbox.replace(2) != 1 || mailbox.take() != 2 || mailbox.take() != 0) {
        return 2;
    }
    if (mailbox.replace(3) != 0 || mailbox.remove_quiescent(4) || !mailbox.remove_quiescent(3) ||
        mailbox.replace(4) != 0 || mailbox.clear_quiescent() != 4 || mailbox.take() != 0) {
        return 3;
    }

    const saccade::core::NewestFrameMailboxStats stats = mailbox.stats_quiescent();
    if (stats.published != 4 || stats.replaced != 1 || stats.consumed != 1 || stats.discarded != 2) {
        return 4;
    }

    NewestFrameMailbox measured;
    saccade::test::begin_allocation_tracking();
    for (uint64_t value = 1; value <= 10000; ++value) {
        if (measured.replace(value) != 0 || measured.take() != value) {
            saccade::test::end_allocation_tracking();
            return 5;
        }
    }
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (allocations != 0) {
        return 6;
    }

    constexpr size_t stress_count = 20000;
    std::array<std::atomic<uint8_t>, stress_count + 1> claims{};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> invalid_claim{false};
    NewestFrameMailbox concurrent;

    const auto claim = [&](SaccadeFrameHandle handle) noexcept {
        if (handle == 0 || handle > stress_count) {
            invalid_claim.store(true, std::memory_order_relaxed);
            return;
        }
        claims[static_cast<size_t>(handle)].fetch_add(1, std::memory_order_relaxed);
    };

    std::thread producer([&]() {
        for (size_t index = 1; index <= stress_count; ++index) {
            const SaccadeFrameHandle replaced = concurrent.replace(static_cast<SaccadeFrameHandle>(index));
            if (replaced != 0) {
                claim(replaced);
            }
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&]() {
        while (!producer_done.load(std::memory_order_acquire)) {
            const SaccadeFrameHandle frame = concurrent.take();
            if (frame != 0) {
                claim(frame);
            } else {
                std::this_thread::yield();
            }
        }
        for (;;) {
            const SaccadeFrameHandle frame = concurrent.take();
            if (frame == 0) {
                break;
            }
            claim(frame);
        }
    });
    producer.join();
    consumer.join();

    if (invalid_claim.load(std::memory_order_relaxed)) {
        return 7;
    }
    for (size_t index = 1; index <= stress_count; ++index) {
        if (claims[index].load(std::memory_order_relaxed) != 1) {
            return 8;
        }
    }
    const saccade::core::NewestFrameMailboxStats concurrent_stats = concurrent.stats_quiescent();
    if (concurrent_stats.published != stress_count ||
        concurrent_stats.replaced + concurrent_stats.consumed != stress_count || concurrent_stats.discarded != 0) {
        return 9;
    }

    return 0;
}

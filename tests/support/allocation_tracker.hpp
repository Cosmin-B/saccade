#ifndef SACCADE_TESTS_SUPPORT_ALLOCATION_TRACKER_HPP
#define SACCADE_TESTS_SUPPORT_ALLOCATION_TRACKER_HPP

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace saccade::test {

inline std::atomic<bool> allocation_tracking_enabled{false};
inline std::atomic<size_t> tracked_allocation_count{0};

inline void record_allocation() noexcept {
    if (allocation_tracking_enabled.load(std::memory_order_relaxed)) {
        tracked_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void* allocate(size_t size) {
    const size_t requested = size == 0 ? 1 : size;
    if (void* memory = std::malloc(requested)) {
        return memory;
    }
    throw std::bad_alloc();
}

inline void* allocate_aligned(size_t size, size_t alignment) {
    const size_t requested = size == 0 ? 1 : size;
#if defined(_WIN32)
    if (void* memory = _aligned_malloc(requested, alignment)) {
        return memory;
    }
#else
    const size_t remainder = requested % alignment;
    const size_t padding = remainder == 0 ? 0 : alignment - remainder;
    if (requested > std::numeric_limits<size_t>::max() - padding) {
        throw std::bad_alloc();
    }
    if (void* memory = std::aligned_alloc(alignment, requested + padding)) {
        return memory;
    }
#endif
    throw std::bad_alloc();
}

inline void deallocate_aligned(void* memory) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

inline void begin_allocation_tracking() noexcept {
    tracked_allocation_count.store(0, std::memory_order_relaxed);
    allocation_tracking_enabled.store(true, std::memory_order_relaxed);
}

inline size_t end_allocation_tracking() noexcept {
    allocation_tracking_enabled.store(false, std::memory_order_relaxed);
    return tracked_allocation_count.load(std::memory_order_relaxed);
}

inline bool allocation_tracker_self_test() {
    begin_allocation_tracking();
    void* array = ::operator new[](8);
    ::operator delete[](array);
    void* aligned = ::operator new(64, std::align_val_t{64});
    ::operator delete(aligned, std::align_val_t{64});
    const size_t observed = end_allocation_tracking();
    tracked_allocation_count.store(0, std::memory_order_relaxed);
    return observed == 2;
}

} // namespace saccade::test

void* operator new(std::size_t size) {
    saccade::test::record_allocation();
    return saccade::test::allocate(size);
}

void* operator new[](std::size_t size) {
    saccade::test::record_allocation();
    return saccade::test::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    saccade::test::record_allocation();
    return saccade::test::allocate_aligned(size, static_cast<size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    saccade::test::record_allocation();
    return saccade::test::allocate_aligned(size, static_cast<size_t>(alignment));
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    saccade::test::deallocate_aligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    saccade::test::deallocate_aligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    saccade::test::deallocate_aligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    saccade::test::deallocate_aligned(memory);
}

#endif

#include "core/handle_table.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> count_allocations{false};
std::atomic<size_t> allocation_count{0};

struct Item {
    explicit Item(int value) noexcept : id(value) {}
    int id;
};

struct ThrowingDestroy {
    void operator()(uint64_t, Item&) {}
};

struct NoexceptDestroy {
    void operator()(uint64_t, Item&) noexcept {}
};

using ConstraintTable = saccade::core::HandleTable<Item, 1>;
template <typename Destroy>
concept CanClearReverse = requires(ConstraintTable& table, Destroy destroy) {
    table.clear_reverse(destroy);
};

static_assert(!CanClearReverse<ThrowingDestroy>);
static_assert(CanClearReverse<NoexceptDestroy>);

}  // namespace

void* operator new(std::size_t size) {
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    saccade::core::HandleTable<Item, 2> table;
    SaccadeRuntimeHandle first = 0;
    SaccadeRuntimeHandle second = 0;
    SaccadeRuntimeHandle third = 0;

    if (table.get(0) != nullptr ||
        table.emplace(nullptr, 1) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 1;
    }
    if (table.emplace(&first, 1) != SACCADE_OK || first == 0 ||
        table.emplace(&second, 2) != SACCADE_OK || second == 0) {
        return 2;
    }
    if (table.emplace(&third, 3) != SACCADE_ERROR_CAPACITY || third != 0) {
        return 3;
    }

    std::array<int, 2> iteration{};
    size_t iteration_size = 0;
    table.for_each([&](uint64_t handle, Item& item) {
        if ((iteration_size == 0 && handle != first) ||
            (iteration_size == 1 && handle != second)) {
            iteration_size = iteration.size() + 1;
            return;
        }
        iteration[iteration_size++] = item.id;
    });
    if (iteration_size != 2 || iteration[0] != 1 || iteration[1] != 2) {
        return 4;
    }

    if (table.erase(first) != SACCADE_OK || table.get(first) != nullptr ||
        table.erase(first) != SACCADE_ERROR_STALE_HANDLE) {
        return 5;
    }
    if (table.emplace(&third, 3) != SACCADE_OK || third == first ||
        static_cast<uint32_t>(third) != static_cast<uint32_t>(first)) {
        return 6;
    }

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (int index = 0; index < 1000; ++index) {
        if (table.get(second) == nullptr || table.get(third) == nullptr) {
            return 7;
        }
        table.for_each([](uint64_t, Item&) noexcept {});
    }
    count_allocations.store(false, std::memory_order_relaxed);
    if (allocation_count.load(std::memory_order_relaxed) != 0) {
        return 8;
    }

    std::array<int, 2> destruction{};
    size_t destruction_size = 0;
    table.clear_reverse([&](uint64_t, Item& item) noexcept {
        destruction[destruction_size++] = item.id;
    });
    if (destruction_size != 2 || destruction[0] != 3 || destruction[1] != 2 ||
        table.size() != 0 || table.get(second) != nullptr || table.get(third) != nullptr) {
        return 9;
    }

    saccade::core::HandleTable<Item, 1, uint8_t> saturating_table;
    uint64_t first_generation_handle = 0;
    uint64_t current_handle = 0;
    if (saturating_table.emplace(&current_handle, 1) != SACCADE_OK) {
        return 10;
    }
    first_generation_handle = current_handle;
    for (uint32_t generation = 1; generation <= UINT8_MAX; ++generation) {
        if (static_cast<uint32_t>(current_handle >> 32U) != generation ||
            saturating_table.erase(current_handle) != SACCADE_OK) {
            return 11;
        }
        if (generation != UINT8_MAX &&
            saturating_table.emplace(&current_handle, 1) != SACCADE_OK) {
            return 12;
        }
    }
    current_handle = 99;
    if (saturating_table.emplace(&current_handle, 1) != SACCADE_ERROR_CAPACITY ||
        current_handle != 0 || saturating_table.get(first_generation_handle) != nullptr) {
        return 13;
    }

    return 0;
}

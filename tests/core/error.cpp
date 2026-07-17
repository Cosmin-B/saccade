#include "core/abi_guard.hpp"
#include "core/error.hpp"

#include <saccade/saccade.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace {

std::atomic<bool> count_allocations{false};
std::atomic<size_t> allocation_count{0};

bool span_equals(SaccadeSpanU8 span, const char* text) {
    const size_t size = std::strlen(text);
    return span.size == size && std::memcmp(span.data, text, size) == 0;
}

} // namespace

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

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

void operator delete[](void* memory, std::size_t size) noexcept {
    ::operator delete(memory, size);
}

int main() {
    using saccade::core::clear_last_error;
    using saccade::core::ErrorScope;
    using saccade::core::set_last_error;

    clear_last_error();
    {
        ErrorScope outer;
        if (saccade_last_error().size != 0) {
            return 1;
        }
        set_last_error("outer");
        {
            ErrorScope inner;
            if (saccade_last_error().size != 0) {
                return 2;
            }
            set_last_error("inner");
            if (!span_equals(saccade_last_error(), "inner")) {
                return 3;
            }
        }
        if (!span_equals(saccade_last_error(), "outer")) {
            return 4;
        }
    }
    if (!span_equals(saccade_last_error(), "outer")) {
        return 5;
    }

    { ErrorScope successful_call; }
    if (saccade_last_error().size != 0) {
        return 6;
    }

    char long_utf8[513];
    std::memset(long_utf8, 'a', 509);
    long_utf8[509] = static_cast<char>(0xE2);
    long_utf8[510] = static_cast<char>(0x82);
    long_utf8[511] = static_cast<char>(0xAC);
    long_utf8[512] = '\0';
    set_last_error(long_utf8);
    const SaccadeSpanU8 truncated = saccade_last_error();
    if (truncated.size != 509 || truncated.data[508] != 'a') {
        return 7;
    }

    set_last_error("main");
    std::thread worker([] {
        saccade::core::set_last_error("worker");
        if (!span_equals(saccade_last_error(), "worker")) {
            std::abort();
        }
    });
    worker.join();
    if (!span_equals(saccade_last_error(), "main")) {
        return 8;
    }

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    {
        ErrorScope measured;
        set_last_error("bounded");
        const SaccadeSpanU8 value = saccade_last_error();
        if (!span_equals(value, "bounded")) {
            return 9;
        }
    }
    count_allocations.store(false, std::memory_order_relaxed);
    if (allocation_count.load(std::memory_order_relaxed) != 0) {
        return 10;
    }

    const SaccadeResult guarded =
        saccade::core::abi_guard([]() -> SaccadeResult { throw std::runtime_error("test exception"); });
    if (guarded != SACCADE_ERROR_BACKEND || saccade_last_error().size == 0) {
        return 11;
    }

    return 0;
}

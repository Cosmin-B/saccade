#include "backend/registry.hpp"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

int main() {
#if defined(_WIN32)
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const size_t page_size = static_cast<size_t>(system_info.dwPageSize);
    auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, page_size * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (pages == nullptr) {
        return 1;
    }
    DWORD old_protection = 0;
    if (VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old_protection) == 0) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return 2;
    }
#else
    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) {
        return 1;
    }
    const size_t page_size = static_cast<size_t>(queried_page_size);
    auto* pages =
        static_cast<uint8_t*>(mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (pages == MAP_FAILED) {
        return 2;
    }
    if (mprotect(pages + page_size, page_size, PROT_NONE) != 0) {
        munmap(pages, page_size * 2);
        return 3;
    }
#endif

    auto* size = reinterpret_cast<uint32_t*>(pages + page_size - sizeof(uint32_t));
    *size = 0;
    const auto* desc = reinterpret_cast<const SaccadeInferenceProviderDesc*>(size);
    saccade::backend::ProviderRegistry registry;
    const SaccadeResult result = registry.register_inference(desc, nullptr);

#if defined(_WIN32)
    VirtualFree(pages, 0, MEM_RELEASE);
#else
    munmap(pages, page_size * 2);
#endif

    return result == SACCADE_ERROR_INVALID_ARGUMENT ? 0 : 4;
}

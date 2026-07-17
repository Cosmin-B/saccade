#include "platform/windows/operating_system.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace saccade::platform::windows {

uint32_t operating_system_build() noexcept {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);

    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) return 0;
    const auto query = reinterpret_cast<RtlGetVersion>(GetProcAddress(module, "RtlGetVersion"));
    if (query == nullptr) return 0;

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return query(&version) >= 0 ? version.dwBuildNumber : 0;
}

bool operating_system_supported() noexcept {
    return operating_system_build() >= minimum_supported_windows_build;
}

} // namespace saccade::platform::windows

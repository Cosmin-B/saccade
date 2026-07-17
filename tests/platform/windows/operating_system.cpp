#include "platform/windows/operating_system.hpp"

#include <cstdint>

namespace {

enum class ExitCode : int { success, query_failed, support_mismatch };

} // namespace

int main() {
    const uint32_t build = saccade::platform::windows::operating_system_build();
    if (build == 0) return static_cast<int>(ExitCode::query_failed);

    const bool expected = build >= saccade::platform::windows::minimum_supported_windows_build;
    return saccade::platform::windows::operating_system_supported() == expected
               ? static_cast<int>(ExitCode::success)
               : static_cast<int>(ExitCode::support_mismatch);
}

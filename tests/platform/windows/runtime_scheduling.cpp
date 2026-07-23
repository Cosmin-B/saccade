#include "platform/windows/runtime_scheduling.hpp"

namespace {

enum class ExitCode : int {
    success,
    initial_state,
    initialization,
    active_state,
    process_priority,
    duplicate_initialization,
    shutdown,
    final_state,
    process_priority_restore,
    duplicate_shutdown
};

constexpr int exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

} // namespace

int main() {
    const DWORD previous_priority_class = GetPriorityClass(GetCurrentProcess());
    if (previous_priority_class == 0) return exit_code(ExitCode::initial_state);

    saccade::platform::windows::RuntimeScheduling scheduling;
    if (scheduling.initialized()) return exit_code(ExitCode::initial_state);
    if (scheduling.initialize() != SACCADE_OK) return exit_code(ExitCode::initialization);
    if (!scheduling.initialized()) return exit_code(ExitCode::active_state);
    const DWORD active_priority_class = GetPriorityClass(GetCurrentProcess());
    if (active_priority_class != ABOVE_NORMAL_PRIORITY_CLASS && active_priority_class != HIGH_PRIORITY_CLASS &&
        active_priority_class != REALTIME_PRIORITY_CLASS)
        return exit_code(ExitCode::process_priority);
    if (scheduling.initialize() != SACCADE_ERROR_ALREADY_EXISTS) return exit_code(ExitCode::duplicate_initialization);
    if (scheduling.shutdown() != SACCADE_OK) return exit_code(ExitCode::shutdown);
    if (scheduling.initialized()) return exit_code(ExitCode::final_state);
    if (GetPriorityClass(GetCurrentProcess()) != previous_priority_class)
        return exit_code(ExitCode::process_priority_restore);
    if (scheduling.shutdown() != SACCADE_ERROR_STATE) return exit_code(ExitCode::duplicate_shutdown);
    return exit_code(ExitCode::success);
}

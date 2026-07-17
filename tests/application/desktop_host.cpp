#include "application/desktop_host.hpp"

#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    initialize_failed,
    dispatch_failed,
    suspend_failed,
    physical_input_failed,
    operation_failed,
    shutdown_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct Capture {
    uint32_t interactions = 0;
    uint32_t suspension_changes = 0;
    uint32_t neutralizations = 0;
    uint32_t physical_inputs = 0;
    uint32_t settings = 0;
    uint32_t restarts = 0;
    uint32_t quits = 0;
    bool suspended = false;
};

SaccadeResult dispatch(void* context, saccade::application::Command, uint64_t) noexcept {
    ++static_cast<Capture*>(context)->interactions;
    return SACCADE_OK;
}

SaccadeResult set_suspended(void* context, bool value) noexcept {
    auto* capture = static_cast<Capture*>(context);
    capture->suspended = value;
    ++capture->suspension_changes;
    return SACCADE_OK;
}

SaccadeResult neutralize(void* context) noexcept {
    ++static_cast<Capture*>(context)->neutralizations;
    return SACCADE_OK;
}

void observe(void* context, uint64_t) noexcept {
    ++static_cast<Capture*>(context)->physical_inputs;
}

SaccadeResult settings(void* context) noexcept {
    ++static_cast<Capture*>(context)->settings;
    return SACCADE_OK;
}

SaccadeResult restart(void* context) noexcept {
    ++static_cast<Capture*>(context)->restarts;
    return SACCADE_OK;
}

SaccadeResult quit(void* context) noexcept {
    ++static_cast<Capture*>(context)->quits;
    return SACCADE_OK;
}

} // namespace

int main() {
    using saccade::application::Command;
    using saccade::application::CommandEvent;
    Capture capture{};
    saccade::application::DesktopHost host;
    if (host.initialize({&capture, dispatch, set_suspended, neutralize, observe, settings, restart, quit}) !=
        SACCADE_OK) {
        return result(TestResult::initialize_failed);
    }
    if (host.dispatch({1, Command::left_click}) != SACCADE_OK || capture.interactions != 1)
        return result(TestResult::dispatch_failed);
    if (host.dispatch({2, Command::suspend_toggle}) != SACCADE_OK || !host.suspended() || !capture.suspended ||
        capture.neutralizations != 1 || host.dispatch({3, Command::left_click}) != SACCADE_OK ||
        capture.interactions != 1)
        return result(TestResult::suspend_failed);
    host.observe_physical_input(4);
    if (capture.physical_inputs != 1) return result(TestResult::physical_input_failed);
    if (host.dispatch(CommandEvent{5, Command::open_settings}) != SACCADE_OK ||
        host.dispatch(CommandEvent{6, Command::restart}) != SACCADE_OK ||
        host.dispatch(CommandEvent{7, Command::quit}) != SACCADE_OK || capture.settings != 1 || capture.restarts != 1 ||
        capture.quits != 1 || capture.neutralizations != 3)
        return result(TestResult::operation_failed);
    return host.shutdown() == SACCADE_OK && capture.neutralizations == 4 ? result(TestResult::success)
                                                                         : result(TestResult::shutdown_failed);
}

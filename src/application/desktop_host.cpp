#include "application/desktop_host.hpp"

namespace saccade::application {

SaccadeResult DesktopHost::initialize(DesktopHostCallbacks callbacks) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (callbacks.dispatch == nullptr || callbacks.set_suspended == nullptr || callbacks.neutralize_input == nullptr ||
        callbacks.open_settings == nullptr || callbacks.restart == nullptr || callbacks.quit == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    callbacks_ = callbacks;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DesktopHost::fail(SaccadeResult result) noexcept {
    if (result != SACCADE_OK) {
        last_fault_ = result;
        ++stats_.failures;
    }
    return result;
}

SaccadeResult DesktopHost::run(DesktopHostOperationFn operation) noexcept {
    return fail(operation(callbacks_.context));
}

SaccadeResult DesktopHost::set_suspended(bool value) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (suspended_ == value) return SACCADE_OK;
    if (value) {
        const SaccadeResult neutralized = callbacks_.neutralize_input(callbacks_.context);
        if (neutralized != SACCADE_OK) return fail(neutralized);
        ++stats_.neutralizations;
    }
    const SaccadeResult changed = callbacks_.set_suspended(callbacks_.context, value);
    if (changed != SACCADE_OK) return fail(changed);
    suspended_ = value;
    ++stats_.suspension_changes;
    return SACCADE_OK;
}

SaccadeResult DesktopHost::dispatch(const CommandEvent& event) noexcept {
    if (!initialized_ || event.timestamp_ns == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    ++stats_.commands;
    switch (event.command) {
    case Command::suspend_toggle:
        return set_suspended(!suspended_);
    case Command::open_settings: {
        const SaccadeResult result = run(callbacks_.open_settings);
        if (result == SACCADE_OK) ++stats_.settings_opens;
        return result;
    }
    case Command::restart: {
        const SaccadeResult neutralized = callbacks_.neutralize_input(callbacks_.context);
        if (neutralized != SACCADE_OK) return fail(neutralized);
        ++stats_.neutralizations;
        const SaccadeResult result = run(callbacks_.restart);
        if (result == SACCADE_OK) ++stats_.restarts;
        return result;
    }
    case Command::quit: {
        const SaccadeResult neutralized = callbacks_.neutralize_input(callbacks_.context);
        if (neutralized != SACCADE_OK) return fail(neutralized);
        ++stats_.neutralizations;
        const SaccadeResult result = run(callbacks_.quit);
        if (result == SACCADE_OK) ++stats_.quit_requests;
        return result;
    }
    default:
        if (suspended_) return SACCADE_OK;
        ++stats_.interaction_commands;
        return fail(callbacks_.dispatch(callbacks_.context, event.command, event.timestamp_ns));
    }
}

void DesktopHost::observe_physical_input(uint64_t timestamp_ns) noexcept {
    if (!initialized_ || timestamp_ns == 0) return;
    ++stats_.physical_inputs;
    if (callbacks_.observe_input != nullptr) callbacks_.observe_input(callbacks_.context, timestamp_ns);
}

SaccadeResult DesktopHost::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult neutralized = callbacks_.neutralize_input(callbacks_.context);
    if (neutralized != SACCADE_OK) return fail(neutralized);
    ++stats_.neutralizations;
    callbacks_ = {};
    initialized_ = false;
    suspended_ = false;
    return SACCADE_OK;
}

void dispatch_desktop_command(void* context, const CommandEvent& event) noexcept {
    auto* host = static_cast<DesktopHost*>(context);
    if (host != nullptr) (void)host->dispatch(event);
}

void observe_desktop_input(void* context, uint64_t timestamp_ns) noexcept {
    auto* host = static_cast<DesktopHost*>(context);
    if (host != nullptr) host->observe_physical_input(timestamp_ns);
}

} // namespace saccade::application

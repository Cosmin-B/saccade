#include "platform/macos/explicit_window_session.hpp"

#include <cstdint>

namespace {

using saccade::platform::macos::explicit_window_current_space;
using saccade::platform::macos::explicit_window_visible;
using saccade::platform::macos::ExplicitWindowActionToken;
using saccade::platform::macos::ExplicitWindowActivationAdmission;
using saccade::platform::macos::ExplicitWindowActivationKey;
using saccade::platform::macos::ExplicitWindowActivationState;
using saccade::platform::macos::ExplicitWindowIdentity;
using saccade::platform::macos::ExplicitWindowRetirementReason;
using saccade::platform::macos::ExplicitWindowSceneGeneration;
using saccade::platform::macos::ExplicitWindowSession;

enum class TestResult : int {
    success,
    exact_selection_failed,
    foreground_fallback_failed,
    identity_reuse_failed,
    bounds_change_failed,
    disappearance_failed,
    switching_failed,
    permission_loss_failed,
    disconnect_failed,
    shutdown_failed,
    monotonicity_failed,
    scene_attribution_failed,
    activation_replay_failed,
};

constexpr uint32_t visible_current_space = explicit_window_visible | explicit_window_current_space;

ExplicitWindowIdentity identity(uint64_t process_id, uint64_t window_id, uint64_t capture_source_id, int32_t x = 100, int32_t y = 200,
                                int32_t width = 800, int32_t height = 600, uint32_t flags = visible_current_space) noexcept {
    return {process_id, window_id, capture_source_id, {x, y, width, height}, flags, 0};
}

ExplicitWindowSceneGeneration generation(uint64_t scene_epoch, uint64_t transform_epoch, uint64_t permission_epoch) noexcept {
    return {scene_epoch, scene_epoch, transform_epoch, transform_epoch, permission_epoch};
}

struct FakeActionBackend {
    uint32_t calls = 0;
    uint64_t process_id = 0;
    uint64_t window_id = 0;

    SaccadeResult perform(const ExplicitWindowActionToken& token) noexcept {
        ++calls;
        process_id = token.process_id;
        window_id = token.window_id;
        return SACCADE_OK;
    }
};

SaccadeResult act_if_current(const ExplicitWindowSession& session, const ExplicitWindowIdentity& observed,
                             const ExplicitWindowActionToken& token, FakeActionBackend* backend) noexcept {
    const SaccadeResult validated = session.validate(observed, token);
    return validated == SACCADE_OK ? backend->perform(token) : validated;
}

bool exact_selection_and_background_validation() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity background = identity(410, 91, 0x020000000000005bULL);
    if (session.select(background, 10) != SACCADE_OK || !session.active() || session.session_epoch() != 10 ||
        session.retirement_reason() != ExplicitWindowRetirementReason::none) {
        return false;
    }

    ExplicitWindowActionToken token{};
    if (session.publish(background, generation(100, 200, 300), &token) != SACCADE_OK || token.session_epoch != 10 ||
        token.scene_epoch != 100 ||
        token.process_id != 410 || token.window_id != 91 || token.transform_epoch != 200 || token.permission_epoch != 300 ||
        token.bounds.x != 100 || token.bounds.y != 200 || token.bounds.width != 800 || token.bounds.height != 600) {
        return false;
    }

    FakeActionBackend backend;
    return act_if_current(session, background, token, &backend) == SACCADE_OK && backend.calls == 1 && backend.process_id == 410 &&
           backend.window_id == 91;
}

bool foreground_never_substitutes_for_explicit_window() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity requested = identity(410, 91, 0x020000000000005bULL);
    const ExplicitWindowIdentity foreground = identity(600, 17, 0x0200000000000011ULL, 0, 0, 1200, 900);
    ExplicitWindowActionToken token{};
    if (session.select(requested, 20) != SACCADE_OK || session.publish(requested, generation(110, 210, 310), &token) != SACCADE_OK) {
        return false;
    }

    FakeActionBackend backend;
    return act_if_current(session, foreground, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0;
}

bool pid_window_and_capture_source_reuse_are_stale() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    ExplicitWindowActionToken token{};
    if (session.select(selected, 30) != SACCADE_OK || session.publish(selected, generation(120, 220, 320), &token) != SACCADE_OK) {
        return false;
    }

    FakeActionBackend backend;
    const ExplicitWindowIdentity reused_by_process = identity(411, 91, 0x020000000000005bULL);
    const ExplicitWindowIdentity reused_by_capture = identity(410, 91, 0x0200000000000063ULL);
    return act_if_current(session, reused_by_process, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0 &&
           act_if_current(session, reused_by_capture, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0;
}

bool bounds_or_visibility_change_are_stale() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    ExplicitWindowActionToken token{};
    if (session.select(selected, 40) != SACCADE_OK || session.publish(selected, generation(130, 230, 330), &token) != SACCADE_OK) {
        return false;
    }

    FakeActionBackend backend;
    const ExplicitWindowIdentity moved = identity(410, 91, 0x020000000000005bULL, 101, 200, 800, 600);
    const ExplicitWindowIdentity hidden = identity(410, 91, 0x020000000000005bULL, 100, 200, 800, 600, explicit_window_current_space);
    return act_if_current(session, moved, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0 &&
           act_if_current(session, hidden, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0;
}

bool disappearance_retires_and_invalidates_tokens() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    ExplicitWindowActionToken token{};
    if (session.select(selected, 50) != SACCADE_OK || session.publish(selected, generation(140, 240, 340), &token) != SACCADE_OK) {
        return false;
    }

    session.retire(ExplicitWindowRetirementReason::disappeared);
    session.retire(ExplicitWindowRetirementReason::disconnected);
    FakeActionBackend backend;
    return !session.active() && session.retirement_reason() == ExplicitWindowRetirementReason::disappeared &&
           act_if_current(session, selected, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0;
}

bool switching_requires_retirement_and_never_revives_old_tokens() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity a = identity(410, 91, 0x020000000000005bULL);
    const ExplicitWindowIdentity b = identity(512, 33, 0x0200000000000021ULL, 900, 100, 640, 480);
    ExplicitWindowActionToken token_a1{};
    ExplicitWindowActionToken token_b{};
    ExplicitWindowActionToken token_a2{};

    if (session.select(a, 60) != SACCADE_OK || session.publish(a, generation(150, 250, 350), &token_a1) != SACCADE_OK ||
        session.select(b, 61) != SACCADE_ERROR_BUSY) {
        return false;
    }
    session.retire(ExplicitWindowRetirementReason::replaced);
    if (session.select(b, 61) != SACCADE_OK || session.publish(b, generation(151, 251, 351), &token_b) != SACCADE_OK) {
        return false;
    }

    FakeActionBackend backend;
    if (act_if_current(session, a, token_a1, &backend) != SACCADE_ERROR_STALE_HANDLE || backend.calls != 0 ||
        act_if_current(session, b, token_b, &backend) != SACCADE_OK || backend.calls != 1) {
        return false;
    }

    session.retire(ExplicitWindowRetirementReason::replaced);
    if (session.select(a, 62) != SACCADE_OK || session.publish(a, generation(152, 252, 352), &token_a2) != SACCADE_OK ||
        act_if_current(session, a, token_a1, &backend) != SACCADE_ERROR_STALE_HANDLE || backend.calls != 1 ||
        act_if_current(session, a, token_a2, &backend) != SACCADE_OK || backend.calls != 2) {
        return false;
    }
    return session.session_epoch() == 62;
}

bool retirement_reason_invalidates_action(ExplicitWindowRetirementReason reason, uint64_t session_epoch) noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    ExplicitWindowActionToken token{};
    if (session.select(selected, session_epoch) != SACCADE_OK ||
        session.publish(selected, generation(session_epoch + 100, session_epoch + 200, session_epoch + 300), &token) != SACCADE_OK) {
        return false;
    }
    session.retire(reason);
    session.retire(reason);
    FakeActionBackend backend;
    return !session.active() && session.retirement_reason() == reason &&
           act_if_current(session, selected, token, &backend) == SACCADE_ERROR_STALE_HANDLE && backend.calls == 0;
}

bool generation_and_session_epochs_are_monotonic() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    ExplicitWindowActionToken first{};
    ExplicitWindowActionToken second{};
    ExplicitWindowActionToken unchanged{};

    if (session.select(selected, 90) != SACCADE_OK || session.publish(selected, generation(190, 290, 390), &first) != SACCADE_OK ||
        session.publish(selected, generation(190, 290, 390), &unchanged) != SACCADE_ERROR_STALE_HANDLE ||
        session.publish(selected, generation(191, 290, 390), &second) != SACCADE_OK ||
        session.publish(selected, generation(192, 289, 390), &unchanged) != SACCADE_ERROR_STALE_HANDLE ||
        session.publish(selected, generation(192, 291, 389), &unchanged) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }

    FakeActionBackend backend;
    if (act_if_current(session, selected, first, &backend) != SACCADE_ERROR_STALE_HANDLE || backend.calls != 0 ||
        act_if_current(session, selected, second, &backend) != SACCADE_OK || backend.calls != 1) {
        return false;
    }

    session.retire(ExplicitWindowRetirementReason::replaced);
    return session.select(selected, 90) == SACCADE_ERROR_STALE_HANDLE && session.select(selected, 91) == SACCADE_OK &&
           session.session_epoch() == 91;
}

bool published_scene_is_tied_to_capture_generation() noexcept {
    ExplicitWindowSession session;
    const ExplicitWindowIdentity selected = identity(410, 91, 0x020000000000005bULL);
    const ExplicitWindowSceneGeneration generation{190, 77, 290, 291, 390};
    ExplicitWindowActionToken token{};

    if (session.select(selected, 90) != SACCADE_OK || session.publish(selected, generation, &token) != SACCADE_OK) {
        return false;
    }

    return token.session_epoch == 90 && token.scene_epoch == generation.scene_epoch && token.frame_id == generation.frame_id &&
           token.transform_epoch == generation.transform_epoch && token.topology_epoch == generation.topology_epoch &&
           token.permission_epoch == generation.permission_epoch && token.process_id == selected.process_id &&
           token.window_id == selected.window_id && token.capture_source_id == selected.capture_source_id &&
           session.validate(selected, token) == SACCADE_OK;
}

bool cancelled_activation_is_not_replayed() noexcept {
    ExplicitWindowActivationState state;
    const ExplicitWindowActivationKey first{41, 410, 91, 120, 9'000};
    const ExplicitWindowActivationKey second{42, 410, 91, 121, 10'000};

    if (state.admit(first) != ExplicitWindowActivationAdmission::start || !state.waiting() ||
        state.set_previous_scene_epoch(700) != SACCADE_OK || state.previous_scene_epoch() != 700 ||
        state.admit(first) != ExplicitWindowActivationAdmission::resume ||
        state.admit(second) != ExplicitWindowActivationAdmission::busy) {
        return false;
    }

    state.cancel();
    if (state.waiting() || state.admit(first) != ExplicitWindowActivationAdmission::cancelled ||
        state.admit(first) != ExplicitWindowActivationAdmission::start) {
        return false;
    }

    state.cancel();
    if (state.admit(second) != ExplicitWindowActivationAdmission::start ||
        state.set_previous_scene_epoch(701) != SACCADE_OK) {
        return false;
    }
    state.complete();
    const ExplicitWindowActivationKey invalid{};
    return !state.waiting() && state.previous_scene_epoch() == 0 &&
           state.admit(invalid) == ExplicitWindowActivationAdmission::invalid;
}

} // namespace

int main() {
    if (!exact_selection_and_background_validation())
        return static_cast<int>(TestResult::exact_selection_failed);
    if (!foreground_never_substitutes_for_explicit_window())
        return static_cast<int>(TestResult::foreground_fallback_failed);
    if (!pid_window_and_capture_source_reuse_are_stale())
        return static_cast<int>(TestResult::identity_reuse_failed);
    if (!bounds_or_visibility_change_are_stale())
        return static_cast<int>(TestResult::bounds_change_failed);
    if (!disappearance_retires_and_invalidates_tokens())
        return static_cast<int>(TestResult::disappearance_failed);
    if (!switching_requires_retirement_and_never_revives_old_tokens())
        return static_cast<int>(TestResult::switching_failed);
    if (!retirement_reason_invalidates_action(ExplicitWindowRetirementReason::permission_lost, 70))
        return static_cast<int>(TestResult::permission_loss_failed);
    if (!retirement_reason_invalidates_action(ExplicitWindowRetirementReason::disconnected, 80))
        return static_cast<int>(TestResult::disconnect_failed);
    if (!retirement_reason_invalidates_action(ExplicitWindowRetirementReason::shutdown, 89))
        return static_cast<int>(TestResult::shutdown_failed);
    if (!generation_and_session_epochs_are_monotonic())
        return static_cast<int>(TestResult::monotonicity_failed);
    if (!published_scene_is_tied_to_capture_generation())
        return static_cast<int>(TestResult::scene_attribution_failed);
    if (!cancelled_activation_is_not_replayed())
        return static_cast<int>(TestResult::activation_replay_failed);
    return static_cast<int>(TestResult::success);
}

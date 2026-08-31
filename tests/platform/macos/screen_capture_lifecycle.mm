#import <Foundation/Foundation.h>

#include "platform/macos/screen_capture_lifecycle.hpp"

#include <cstdint>
#include <unistd.h>

@interface SaccadeLifecycleTestOutput : NSObject
- (instancetype)initWithState:(SaccadeScreenCaptureSessionState*)state;
- (BOOL)beginCallbackForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)endCallbackForSession:
    (saccade::platform::macos::ScreenCaptureSessionIdentity)session;
@property(nonatomic, strong) SaccadeScreenCaptureSessionState* state;
@end

@implementation SaccadeLifecycleTestOutput

- (instancetype)initWithState:(SaccadeScreenCaptureSessionState*)state {
    self = [super init];
    if (self != nil) {
        _state = state;
    }
    return self;
}

- (BOOL)beginCallbackForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    return [_state beginCallbackForSession:session];
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)endCallbackForSession:
    (saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    return [_state endCallbackForSession:session];
}

@end

namespace {

using saccade::platform::macos::ScreenCaptureLifecycle;
using saccade::platform::macos::ScreenCaptureLifecycleAction;
using saccade::platform::macos::ScreenCaptureLifecyclePhase;
using saccade::platform::macos::ScreenCaptureOperation;
using saccade::platform::macos::ScreenCaptureSessionIdentity;

bool same_identity(const ScreenCaptureSessionIdentity& left, const ScreenCaptureSessionIdentity& right) noexcept {
    return left.slot == right.slot && left.generation == right.generation && left.source_id == right.source_id;
}

int completion_after_cancellation_stops_the_late_start() {
    const ScreenCaptureSessionIdentity identity{2, 7, UINT64_C(0x02000000000000A1)};
    ScreenCaptureLifecycle lifecycle(identity);

    const auto start = lifecycle.request_start();
    if (start.action != ScreenCaptureLifecycleAction::start_native || start.operation.token == 0 ||
        !same_identity(start.operation.session, identity) || lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::start_pending) {
        return 1;
    }

    const auto cancelled = lifecycle.cancel();
    const auto cancelled_state = lifecycle.snapshot();
    if (cancelled.action != ScreenCaptureLifecycleAction::none || !cancelled_state.retirement_requested ||
        cancelled_state.accepts_callbacks || cancelled_state.phase != ScreenCaptureLifecyclePhase::start_pending) {
        return 2;
    }

    const auto late_start = lifecycle.complete_start(start.operation, SACCADE_OK);
    if (late_start.action != ScreenCaptureLifecycleAction::stop_native || late_start.operation.token == 0 ||
        late_start.operation.token == start.operation.token || !same_identity(late_start.operation.session, identity) ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::stop_pending) {
        return 3;
    }

    const auto stopped = lifecycle.complete_stop(late_start.operation, SACCADE_OK);
    if (stopped.action != ScreenCaptureLifecycleAction::release_session ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired) {
        return 4;
    }
    return 0;
}

int rapid_switch_rejects_late_callbacks_from_the_previous_generation() {
    const ScreenCaptureSessionIdentity first_identity{5, 41, UINT64_C(0x02000000000000B1)};
    ScreenCaptureLifecycle first(first_identity);
    const auto first_start = first.request_start();
    if (first.complete_start(first_start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none) {
        return 1;
    }
    const auto first_stop = first.request_stop();
    if (first_stop.action != ScreenCaptureLifecycleAction::stop_native ||
        first.complete_stop(first_stop.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::release_session) {
        return 2;
    }

    const ScreenCaptureSessionIdentity second_identity{5, 42, UINT64_C(0x02000000000000B2)};
    ScreenCaptureLifecycle second(second_identity);
    const auto second_start = second.request_start();
    if (second.complete_start(second_start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        second.snapshot().phase != ScreenCaptureLifecyclePhase::running) {
        return 3;
    }

    if (first.begin_callback(first_identity) || second.begin_callback(first_identity) || !second.begin_callback(second_identity)) {
        return 4;
    }
    if (second.snapshot().callback_borrowers != 1 || second.end_callback(second_identity).action != ScreenCaptureLifecycleAction::none) {
        return 5;
    }

    const auto duplicate_first_stop = first.complete_stop(first_stop.operation, SACCADE_OK);
    const auto second_state = second.snapshot();
    if (duplicate_first_stop.action != ScreenCaptureLifecycleAction::none || second_state.phase != ScreenCaptureLifecyclePhase::running ||
        second_state.callback_borrowers != 0 || !same_identity(second_state.session, second_identity)) {
        return 6;
    }
    return 0;
}

int timeout_keeps_the_session_retained_until_the_late_completion() {
    const ScreenCaptureSessionIdentity identity{1, 9, UINT64_C(0x02000000000000C1)};
    ScreenCaptureLifecycle lifecycle(identity);
    const auto start = lifecycle.request_start();

    const auto start_timeout = lifecycle.timeout(start.operation);
    const auto timed_out_start = lifecycle.snapshot();
    if (start_timeout.action != ScreenCaptureLifecycleAction::none || !timed_out_start.retirement_requested ||
        timed_out_start.accepts_callbacks || timed_out_start.phase != ScreenCaptureLifecyclePhase::start_pending) {
        return 1;
    }

    const auto late_start = lifecycle.complete_start(start.operation, SACCADE_OK);
    if (late_start.action != ScreenCaptureLifecycleAction::stop_native ||
        lifecycle.timeout(late_start.operation).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::stop_pending) {
        return 2;
    }

    const auto late_stop = lifecycle.complete_stop(late_start.operation, SACCADE_OK);
    if (late_stop.action != ScreenCaptureLifecycleAction::release_session ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired ||
        lifecycle.complete_stop(late_start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none) {
        return 3;
    }
    return 0;
}

int shutdown_waits_for_a_held_frame_lease() {
    const ScreenCaptureSessionIdentity identity{3, 17, UINT64_C(0x02000000000000D1)};
    ScreenCaptureLifecycle lifecycle(identity);
    const auto start = lifecycle.request_start();
    if (lifecycle.complete_start(start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.acquire_frame_lease() != SACCADE_OK) {
        return 1;
    }

    const auto shutdown = lifecycle.shutdown();
    if (shutdown.action != ScreenCaptureLifecycleAction::stop_native || lifecycle.begin_callback(identity) ||
        lifecycle.snapshot().accepts_callbacks) {
        return 2;
    }

    const auto stopped = lifecycle.complete_stop(shutdown.operation, SACCADE_OK);
    const auto draining = lifecycle.snapshot();
    if (stopped.action != ScreenCaptureLifecycleAction::none || draining.phase != ScreenCaptureLifecyclePhase::draining ||
        draining.frame_leases != 1) {
        return 3;
    }

    const auto released = lifecycle.release_frame_lease();
    if (released.action != ScreenCaptureLifecycleAction::release_session ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired) {
        return 4;
    }
    return 0;
}

int repeated_explicit_sessions_retire_before_the_next_generation() {
    constexpr uint32_t session_count = 64;
    constexpr uint32_t slot = 8;

    for (uint32_t generation = 1; generation <= session_count; ++generation) {
        const ScreenCaptureSessionIdentity identity{slot, generation, UINT64_C(0x0200000000001000) + generation};
        ScreenCaptureLifecycle lifecycle(identity);
        const auto start = lifecycle.request_start();
        if (lifecycle.complete_start(start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
            !lifecycle.begin_callback(identity) || lifecycle.end_callback(identity).action != ScreenCaptureLifecycleAction::none ||
            lifecycle.acquire_frame_lease() != SACCADE_OK || lifecycle.release_frame_lease().action != ScreenCaptureLifecycleAction::none) {
            return 1;
        }

        const auto stop = lifecycle.request_stop();
        if (stop.action != ScreenCaptureLifecycleAction::stop_native ||
            lifecycle.complete_stop(stop.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::release_session ||
            lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired || lifecycle.begin_callback(identity)) {
            return 2;
        }
    }
    return 0;
}

int duplicate_tokens_never_repeat_final_release() {
    const ScreenCaptureSessionIdentity identity{11, 23, UINT64_C(0x02000000000000E1)};
    ScreenCaptureLifecycle lifecycle(identity);
    const auto start = lifecycle.request_start();
    ScreenCaptureOperation stale_start = start.operation;
    ++stale_start.token;

    if (lifecycle.complete_start(stale_start, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::start_pending ||
        lifecycle.complete_start(start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        !lifecycle.begin_callback(identity)) {
        return 1;
    }

    const auto stop = lifecycle.request_stop();
    if (lifecycle.complete_start(start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.complete_stop(stop.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.complete_stop(stop.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::draining) {
        return 2;
    }

    if (lifecycle.end_callback(identity).action != ScreenCaptureLifecycleAction::release_session ||
        lifecycle.complete_stop(stop.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired) {
        return 3;
    }
    return 0;
}

int retained_state_survives_late_callbacks_without_reaching_the_replacement_generation() {
    const ScreenCaptureSessionIdentity first_identity{6, 71, UINT64_C(0x02000000000000F1)};
    __weak SaccadeScreenCaptureSessionState* weak_first = nil;

    @autoreleasepool {
        __strong SaccadeScreenCaptureSessionState* provider_slot =
            [[SaccadeScreenCaptureSessionState alloc] initWithSession:first_identity];
        weak_first = provider_slot;
        __strong SaccadeLifecycleTestOutput* output = [[SaccadeLifecycleTestOutput alloc] initWithState:provider_slot];
        const auto start = [provider_slot requestStart];
        if (start.action != ScreenCaptureLifecycleAction::start_native ||
            [provider_slot cancel].action != ScreenCaptureLifecycleAction::none) {
            return 1;
        }

        __block saccade::platform::macos::ScreenCaptureLifecycleTransition late_start{};
        __strong SaccadeScreenCaptureSessionState* completion_state = provider_slot;
        __strong void (^completion)(void) = ^{ late_start = [completion_state completeStart:start.operation result:SACCADE_OK]; };
        provider_slot = nil;
        if (weak_first == nil) {
            return 2;
        }

        const ScreenCaptureSessionIdentity second_identity{6, 72, UINT64_C(0x02000000000000F2)};
        __strong SaccadeScreenCaptureSessionState* replacement = [[SaccadeScreenCaptureSessionState alloc] initWithSession:second_identity];
        const auto replacement_start = [replacement requestStart];
        if ([replacement completeStart:replacement_start.operation result:SACCADE_OK].action != ScreenCaptureLifecycleAction::none) {
            return 3;
        }

        completion();
        if (late_start.action != ScreenCaptureLifecycleAction::stop_native || [output beginCallbackForSession:first_identity] ||
            [replacement beginCallbackForSession:first_identity] || ![replacement beginCallbackForSession:second_identity] ||
            [replacement endCallbackForSession:second_identity].action != ScreenCaptureLifecycleAction::none ||
            [replacement snapshot].phase != ScreenCaptureLifecyclePhase::running) {
            return 4;
        }
        if ([completion_state completeStop:late_start.operation result:SACCADE_OK].action !=
            ScreenCaptureLifecycleAction::release_session) {
            return 5;
        }
        [completion_state markNativeStopError];
        if ([replacement nativeStopFailed] || [replacement snapshot].phase != ScreenCaptureLifecyclePhase::running) {
            return 6;
        }

        output = nil;
        completion = nil;
        completion_state = nil;
    }
    if (weak_first != nil) {
        return 7;
    }
    return 0;
}

int retained_state_releases_only_after_callback_and_frame_borrowers_return() {
    const ScreenCaptureSessionIdentity identity{7, 81, UINT64_C(0x02000000000000F3)};
    __weak SaccadeScreenCaptureSessionState* weak_state = nil;

    @autoreleasepool {
        __strong SaccadeScreenCaptureSessionState* provider_slot = [[SaccadeScreenCaptureSessionState alloc] initWithSession:identity];
        weak_state = provider_slot;
        const auto start = [provider_slot requestStart];
        if ([provider_slot completeStart:start.operation result:SACCADE_OK].action != ScreenCaptureLifecycleAction::none ||
            ![provider_slot beginCallbackForSession:identity] || [provider_slot acquireFrameLease] != SACCADE_OK) {
            return 1;
        }

        const auto stop = [provider_slot requestStop];
        if ([provider_slot completeStop:stop.operation result:SACCADE_OK].action != ScreenCaptureLifecycleAction::none ||
            [provider_slot snapshot].phase != ScreenCaptureLifecyclePhase::draining) {
            return 2;
        }

        provider_slot = nil;
        __strong SaccadeScreenCaptureSessionState* borrower = weak_state;
        if (borrower == nil || [borrower endCallbackForSession:identity].action != ScreenCaptureLifecycleAction::none ||
            [borrower releaseFrameLease].action != ScreenCaptureLifecycleAction::release_session) {
            return 3;
        }
        borrower = nil;
    }
    return weak_state == nil ? 0 : 4;
}

int failed_native_stop_can_be_retried_without_reopening_callback_admission() {
    const ScreenCaptureSessionIdentity identity{9, 91, UINT64_C(0x02000000000000F4)};
    ScreenCaptureLifecycle lifecycle(identity);
    const auto start = lifecycle.request_start();
    if (lifecycle.complete_start(start.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::none)
        return 1;

    const auto first_stop = lifecycle.request_stop();
    if (first_stop.action != ScreenCaptureLifecycleAction::stop_native ||
        lifecycle.complete_stop(first_stop.operation, SACCADE_ERROR_BACKEND).action != ScreenCaptureLifecycleAction::none) {
        return 2;
    }
    const auto failed = lifecycle.snapshot();
    if (failed.phase != ScreenCaptureLifecyclePhase::running || failed.accepts_callbacks || !failed.retirement_requested)
        return 3;

    const auto retry = lifecycle.cancel();
    if (retry.action != ScreenCaptureLifecycleAction::stop_native || retry.operation.token == first_stop.operation.token ||
        lifecycle.complete_stop(retry.operation, SACCADE_OK).action != ScreenCaptureLifecycleAction::release_session ||
        lifecycle.snapshot().phase != ScreenCaptureLifecyclePhase::retired) {
        return 4;
    }
    return 0;
}

int held_delegate_callback_wakes_retirement_and_blocks_context_clear() {
    const ScreenCaptureSessionIdentity identity{10, 101, UINT64_C(0x02000000000000F5)};
    __strong SaccadeScreenCaptureSessionState* state = [[SaccadeScreenCaptureSessionState alloc] initWithSession:identity];
    int callback_context = 1;
    [state setCallbackContext:&callback_context];
    const auto start = [state requestStart];
    if ([state completeStart:start.operation result:SACCADE_OK].action != ScreenCaptureLifecycleAction::none ||
        [state beginCallbackContextForSession:identity] != &callback_context) {
        return 1;
    }
    const auto stop = [state requestStop];
    if ([state completeStop:stop.operation result:SACCADE_OK].action != ScreenCaptureLifecycleAction::none ||
        [state snapshot].phase != ScreenCaptureLifecyclePhase::draining || [state clearCallbackContext]) {
        return 2;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      usleep(1000);
      (void)[state endCallbackForSession:identity];
    });
    if ([state waitAvailable:dispatch_time(DISPATCH_TIME_NOW, INT64_C(1000000000))] != 0 ||
        [state snapshot].phase != ScreenCaptureLifecyclePhase::retired || ![state clearCallbackContext]) {
        return 3;
    }
    return 0;
}

int failed_stop_state_releases_after_safe_context_detach() {
    const ScreenCaptureSessionIdentity identity{12, 111, UINT64_C(0x02000000000000F6)};
    __weak SaccadeScreenCaptureSessionState* weak_state = nil;
    @autoreleasepool {
        __strong SaccadeScreenCaptureSessionState* state = [[SaccadeScreenCaptureSessionState alloc] initWithSession:identity];
        weak_state = state;
        int callback_context = 1;
        [state setCallbackContext:&callback_context];
        const auto start = [state requestStart];
        const auto stop = [state completeStart:start.operation result:SACCADE_OK].action == ScreenCaptureLifecycleAction::none
                              ? [state requestStop]
                              : saccade::platform::macos::ScreenCaptureLifecycleTransition{};
        if (stop.action != ScreenCaptureLifecycleAction::stop_native ||
            [state completeStop:stop.operation result:SACCADE_ERROR_BACKEND].action != ScreenCaptureLifecycleAction::none ||
            ![state clearCallbackContext]) {
            return 1;
        }
        [state releaseDetachedLifetimeHold];
        state = nil;
    }
    return weak_state == nil ? 0 : 2;
}

} // namespace

int main() {
    if (const int result = completion_after_cancellation_stops_the_late_start(); result != 0) {
        return 10 + result;
    }
    if (const int result = rapid_switch_rejects_late_callbacks_from_the_previous_generation(); result != 0) {
        return 20 + result;
    }
    if (const int result = timeout_keeps_the_session_retained_until_the_late_completion(); result != 0) {
        return 30 + result;
    }
    if (const int result = shutdown_waits_for_a_held_frame_lease(); result != 0) {
        return 40 + result;
    }
    if (const int result = repeated_explicit_sessions_retire_before_the_next_generation(); result != 0) {
        return 50 + result;
    }
    if (const int result = duplicate_tokens_never_repeat_final_release(); result != 0) {
        return 60 + result;
    }
    if (const int result = retained_state_survives_late_callbacks_without_reaching_the_replacement_generation(); result != 0) {
        return 70 + result;
    }
    if (const int result = retained_state_releases_only_after_callback_and_frame_borrowers_return(); result != 0) {
        return 80 + result;
    }
    if (const int result = failed_native_stop_can_be_retried_without_reopening_callback_admission(); result != 0) {
        return 90 + result;
    }
    if (const int result = held_delegate_callback_wakes_retirement_and_blocks_context_clear(); result != 0) {
        return 100 + result;
    }
    if (const int result = failed_stop_state_releases_after_safe_context_detach(); result != 0) {
        return 110 + result;
    }
    return 0;
}

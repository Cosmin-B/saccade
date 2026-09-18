#include "platform/macos/screen_capture_lifecycle.hpp"

#include <limits>
#include <new>

namespace saccade::platform::macos {
namespace {

constexpr uint32_t maximum_frame_leases = 3;

bool identities_equal(ScreenCaptureSessionIdentity left, ScreenCaptureSessionIdentity right) noexcept {
    return left.slot == right.slot && left.generation == right.generation && left.source_id == right.source_id;
}

} // namespace

ScreenCaptureLifecycle::ScreenCaptureLifecycle(ScreenCaptureSessionIdentity session) noexcept : session_(session) {}

bool ScreenCaptureLifecycle::same_identity(ScreenCaptureSessionIdentity value) const noexcept {
    return identities_equal(value, session_);
}

bool ScreenCaptureLifecycle::same_operation(ScreenCaptureOperation operation, PendingOperation kind) const noexcept {
    return pending_kind_ == kind && operation.token != 0 && operation.token == pending_.token &&
           identities_equal(operation.session, pending_.session);
}

ScreenCaptureOperation ScreenCaptureLifecycle::next_operation() noexcept {
    ScreenCaptureOperation operation{session_, next_token_++};
    if (next_token_ == 0) {
        next_token_ = 1;
    }
    return operation;
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::request_start() noexcept {
    if (phase_ != ScreenCaptureLifecyclePhase::idle) {
        return {};
    }

    pending_ = next_operation();
    pending_kind_ = PendingOperation::start;
    phase_ = ScreenCaptureLifecyclePhase::start_pending;
    return {ScreenCaptureLifecycleAction::start_native, pending_};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::begin_stop() noexcept {
    accepts_callbacks_ = false;
    retirement_requested_ = true;
    pending_ = next_operation();
    pending_kind_ = PendingOperation::stop;
    phase_ = ScreenCaptureLifecyclePhase::stop_pending;
    return {ScreenCaptureLifecycleAction::stop_native, pending_};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::request_stop() noexcept {
    if (phase_ == ScreenCaptureLifecyclePhase::running) {
        return begin_stop();
    }
    if (phase_ == ScreenCaptureLifecyclePhase::start_pending) {
        retirement_requested_ = true;
        accepts_callbacks_ = false;
    }
    return {};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::cancel() noexcept {
    retirement_requested_ = true;
    accepts_callbacks_ = false;

    if (phase_ == ScreenCaptureLifecyclePhase::idle) {
        phase_ = ScreenCaptureLifecyclePhase::draining;
        return retire_if_quiescent();
    }
    if (phase_ == ScreenCaptureLifecyclePhase::running) {
        return begin_stop();
    }
    return {};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::timeout(ScreenCaptureOperation operation) noexcept {
    if (!same_operation(operation, pending_kind_)) {
        return {};
    }

    retirement_requested_ = true;
    accepts_callbacks_ = false;
    return {};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::complete_start(ScreenCaptureOperation operation, SaccadeResult result) noexcept {
    if (phase_ != ScreenCaptureLifecyclePhase::start_pending || !same_operation(operation, PendingOperation::start)) {
        return {};
    }

    pending_ = {};
    pending_kind_ = PendingOperation::none;
    if (result != SACCADE_OK) {
        accepts_callbacks_ = false;
        retirement_requested_ = true;
        phase_ = ScreenCaptureLifecyclePhase::draining;
        return retire_if_quiescent();
    }
    if (retirement_requested_) {
        return begin_stop();
    }

    phase_ = ScreenCaptureLifecyclePhase::running;
    accepts_callbacks_ = true;
    return {};
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::complete_stop(ScreenCaptureOperation operation, SaccadeResult result) noexcept {
    if (phase_ != ScreenCaptureLifecyclePhase::stop_pending || !same_operation(operation, PendingOperation::stop)) {
        return {};
    }
    if (result != SACCADE_OK) {
        // The native stop request failed, so the stream may still exist. Keep
        // callback admission permanently closed, but clear this operation so
        // an owner can issue another bounded stop attempt during retirement.
        pending_ = {};
        pending_kind_ = PendingOperation::none;
        phase_ = ScreenCaptureLifecyclePhase::running;
        accepts_callbacks_ = false;
        retirement_requested_ = true;
        return {};
    }

    pending_ = {};
    pending_kind_ = PendingOperation::none;
    phase_ = ScreenCaptureLifecyclePhase::draining;
    return retire_if_quiescent();
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::native_stopped() noexcept {
    if (phase_ == ScreenCaptureLifecyclePhase::retired)
        return {};
    accepts_callbacks_ = false;
    retirement_requested_ = true;
    pending_ = {};
    pending_kind_ = PendingOperation::none;
    phase_ = ScreenCaptureLifecyclePhase::draining;
    return retire_if_quiescent();
}

bool ScreenCaptureLifecycle::begin_callback(ScreenCaptureSessionIdentity session) noexcept {
    if (!same_identity(session) || !accepts_callbacks_ || retirement_requested_ || phase_ != ScreenCaptureLifecyclePhase::running ||
        callback_borrowers_ == std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    ++callback_borrowers_;
    return true;
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::end_callback(ScreenCaptureSessionIdentity session) noexcept {
    if (!same_identity(session) || callback_borrowers_ == 0) {
        return {};
    }

    --callback_borrowers_;
    return retire_if_quiescent();
}

SaccadeResult ScreenCaptureLifecycle::acquire_frame_lease() noexcept {
    if (phase_ != ScreenCaptureLifecyclePhase::running || !accepts_callbacks_ || retirement_requested_) {
        return SACCADE_ERROR_STATE;
    }
    if (frame_leases_ == maximum_frame_leases) {
        return SACCADE_ERROR_CAPACITY;
    }

    ++frame_leases_;
    return SACCADE_OK;
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::release_frame_lease() noexcept {
    if (frame_leases_ == 0) {
        return {};
    }

    --frame_leases_;
    return retire_if_quiescent();
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::shutdown() noexcept {
    return cancel();
}

ScreenCaptureLifecycleTransition ScreenCaptureLifecycle::retire_if_quiescent() noexcept {
    if (phase_ != ScreenCaptureLifecyclePhase::draining || callback_borrowers_ != 0 || frame_leases_ != 0) {
        return {};
    }

    phase_ = ScreenCaptureLifecyclePhase::retired;
    return {ScreenCaptureLifecycleAction::release_session, {}};
}

ScreenCaptureLifecycleSnapshot ScreenCaptureLifecycle::snapshot() const noexcept {
    return {session_, phase_, callback_borrowers_, frame_leases_, accepts_callbacks_, retirement_requested_};
}

} // namespace saccade::platform::macos

@interface SaccadeScreenCaptureSessionState () {
    alignas(saccade::platform::macos::ScreenCaptureLifecycle) std::byte
        lifecycle_storage_[sizeof(saccade::platform::macos::ScreenCaptureLifecycle)];
    dispatch_queue_t callback_queue_;
    dispatch_semaphore_t available_;
    __strong SaccadeScreenCaptureSessionState* lifetime_hold_;
    void* callback_context_;
    void* queue_identity_;
    BOOL lifecycle_initialized_;
    BOOL native_stop_failed_;
}

@end

@implementation SaccadeScreenCaptureSessionState

- (saccade::platform::macos::ScreenCaptureLifecycle&)lifecycle {
    return *std::launder(reinterpret_cast<saccade::platform::macos::ScreenCaptureLifecycle*>(lifecycle_storage_));
}

- (BOOL)ownsCallbackQueue {
    return dispatch_get_specific(queue_identity_) == queue_identity_;
}

- (void)performOnCallbackQueue:(void (^)(void))operation {
    if ([self ownsCallbackQueue]) {
        operation();
    } else {
        dispatch_sync(callback_queue_, operation);
    }
}

- (void)observeTransition:(saccade::platform::macos::ScreenCaptureLifecycleTransition)transition {
    if (transition.action == saccade::platform::macos::ScreenCaptureLifecycleAction::release_session) {
        lifetime_hold_ = nil;
        dispatch_semaphore_signal(available_);
    }
}

- (instancetype)initWithSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    self = [super init];
    if (self != nil) {
        callback_queue_ = dispatch_queue_create("dev.saccade.capture.session", DISPATCH_QUEUE_SERIAL);
        available_ = dispatch_semaphore_create(0);
        queue_identity_ = lifecycle_storage_;
        dispatch_queue_set_specific(callback_queue_, queue_identity_, queue_identity_, nullptr);
        ::new (static_cast<void*>(lifecycle_storage_)) saccade::platform::macos::ScreenCaptureLifecycle(session);
        lifecycle_initialized_ = YES;
    }
    return self;
}

- (void)dealloc {
    if (lifecycle_initialized_) {
        [self lifecycle].~ScreenCaptureLifecycle();
    }
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)requestStart {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{ transition = [self lifecycle].request_start(); }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)requestStop {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].request_stop();
      [self observeTransition:transition];
    }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)cancel {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].cancel();
      [self observeTransition:transition];
    }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)timeout:(saccade::platform::macos::ScreenCaptureOperation)operation {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{ transition = [self lifecycle].timeout(operation); }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)completeStart:(saccade::platform::macos::ScreenCaptureOperation)operation
                                                                     result:(SaccadeResult)result {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].complete_start(operation, result);
      if ([self lifecycle].snapshot().phase == saccade::platform::macos::ScreenCaptureLifecyclePhase::running && lifetime_hold_ == nil) {
          lifetime_hold_ = self;
      }
      [self observeTransition:transition];
    }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)completeStop:(saccade::platform::macos::ScreenCaptureOperation)operation
                                                                    result:(SaccadeResult)result {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].complete_stop(operation, result);
      [self observeTransition:transition];
    }];
    return transition;
}

- (BOOL)beginCallbackForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    __block BOOL accepted = NO;
    [self performOnCallbackQueue:^{ accepted = [self lifecycle].begin_callback(session) ? YES : NO; }];
    return accepted;
}

- (void*)beginCallbackContextForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    __block void* context = nullptr;
    [self performOnCallbackQueue:^{
      if ([self lifecycle].begin_callback(session)) {
          context = callback_context_;
          if (context == nullptr) {
              const auto transition = [self lifecycle].end_callback(session);
              [self observeTransition:transition];
          }
      }
    }];
    return context;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)endCallbackForSession:
    (saccade::platform::macos::ScreenCaptureSessionIdentity)session {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].end_callback(session);
      [self observeTransition:transition];
      [self signalAvailable];
    }];
    return transition;
}

- (SaccadeResult)acquireFrameLease {
    __block SaccadeResult result = SACCADE_ERROR_STATE;
    [self performOnCallbackQueue:^{ result = [self lifecycle].acquire_frame_lease(); }];
    return result;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)releaseFrameLease {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].release_frame_lease();
      [self observeTransition:transition];
    }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleTransition)shutdown {
    __block saccade::platform::macos::ScreenCaptureLifecycleTransition transition{};
    [self performOnCallbackQueue:^{
      transition = [self lifecycle].shutdown();
      [self observeTransition:transition];
    }];
    return transition;
}

- (saccade::platform::macos::ScreenCaptureLifecycleSnapshot)snapshot {
    __block saccade::platform::macos::ScreenCaptureLifecycleSnapshot value{};
    [self performOnCallbackQueue:^{
      value = [self lifecycle].snapshot();
      if (native_stop_failed_) {
          value.accepts_callbacks = false;
      }
    }];
    return value;
}

- (void)setCallbackContext:(void*)context {
    [self performOnCallbackQueue:^{ callback_context_ = context; }];
}

- (BOOL)clearCallbackContext {
    __block BOOL cleared = NO;
    [self performOnCallbackQueue:^{
      if ([self lifecycle].snapshot().callback_borrowers == 0) {
          callback_context_ = nullptr;
          cleared = YES;
      }
    }];
    return cleared;
}

- (void)releaseDetachedLifetimeHold {
    [self performOnCallbackQueue:^{
      const auto snapshot = [self lifecycle].snapshot();
      if (callback_context_ == nullptr && snapshot.callback_borrowers == 0 && !snapshot.accepts_callbacks)
          lifetime_hold_ = nil;
    }];
}

- (dispatch_queue_t)callbackQueue {
    return callback_queue_;
}

- (void)signalAvailable {
    dispatch_semaphore_signal(available_);
}

- (long)waitAvailable:(dispatch_time_t)timeout {
    return dispatch_semaphore_wait(available_, timeout);
}

- (void)drainAvailable {
    while (dispatch_semaphore_wait(available_, DISPATCH_TIME_NOW) == 0) {}
}

- (void)markNativeStopError {
    [self performOnCallbackQueue:^{ native_stop_failed_ = YES; }];
    [self signalAvailable];
}

- (void)clearNativeStopError {
    [self performOnCallbackQueue:^{ native_stop_failed_ = NO; }];
}

- (void)nativeDidStopWithError {
    [self performOnCallbackQueue:^{
      native_stop_failed_ = YES;
      const auto stopped = [self lifecycle].native_stopped();
      [self observeTransition:stopped];
    }];
    [self signalAvailable];
}

- (BOOL)nativeStopFailed {
    __block BOOL failed = NO;
    [self performOnCallbackQueue:^{ failed = native_stop_failed_; }];
    return failed;
}

@end

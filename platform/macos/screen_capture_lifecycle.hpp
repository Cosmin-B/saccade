#ifndef SACCADE_PLATFORM_MACOS_SCREEN_CAPTURE_LIFECYCLE_HPP
#define SACCADE_PLATFORM_MACOS_SCREEN_CAPTURE_LIFECYCLE_HPP

#include <saccade/saccade.h>

#include <cstdint>

namespace saccade::platform::macos {

struct ScreenCaptureSessionIdentity {
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint64_t source_id = 0;
};

struct ScreenCaptureOperation {
    ScreenCaptureSessionIdentity session{};
    uint64_t token = 0;
};

enum class ScreenCaptureLifecyclePhase : uint8_t {
    idle,
    start_pending,
    running,
    stop_pending,
    draining,
    retired,
};

enum class ScreenCaptureLifecycleAction : uint8_t {
    none,
    start_native,
    stop_native,
    release_session,
};

struct ScreenCaptureLifecycleTransition {
    ScreenCaptureLifecycleAction action = ScreenCaptureLifecycleAction::none;
    ScreenCaptureOperation operation{};
};

struct ScreenCaptureLifecycleSnapshot {
    ScreenCaptureSessionIdentity session{};
    ScreenCaptureLifecyclePhase phase = ScreenCaptureLifecyclePhase::idle;
    uint32_t callback_borrowers = 0;
    uint32_t frame_leases = 0;
    bool accepts_callbacks = false;
    bool retirement_requested = false;
};

class ScreenCaptureLifecycle final {
  public:
    explicit ScreenCaptureLifecycle(ScreenCaptureSessionIdentity) noexcept;

    ScreenCaptureLifecycle(const ScreenCaptureLifecycle&) = delete;
    ScreenCaptureLifecycle& operator=(const ScreenCaptureLifecycle&) = delete;
    ScreenCaptureLifecycle(ScreenCaptureLifecycle&&) = delete;
    ScreenCaptureLifecycle& operator=(ScreenCaptureLifecycle&&) = delete;

    [[nodiscard]] ScreenCaptureLifecycleTransition request_start() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition request_stop() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition cancel() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition timeout(ScreenCaptureOperation) noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition complete_start(ScreenCaptureOperation, SaccadeResult) noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition complete_stop(ScreenCaptureOperation, SaccadeResult) noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition native_stopped() noexcept;
    [[nodiscard]] bool begin_callback(ScreenCaptureSessionIdentity) noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition end_callback(ScreenCaptureSessionIdentity) noexcept;
    [[nodiscard]] SaccadeResult acquire_frame_lease() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition release_frame_lease() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition shutdown() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleSnapshot snapshot() const noexcept;

  private:
    enum class PendingOperation : uint8_t { none, start, stop };

    [[nodiscard]] bool same_identity(ScreenCaptureSessionIdentity) const noexcept;
    [[nodiscard]] bool same_operation(ScreenCaptureOperation, PendingOperation) const noexcept;
    [[nodiscard]] ScreenCaptureOperation next_operation() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition begin_stop() noexcept;
    [[nodiscard]] ScreenCaptureLifecycleTransition retire_if_quiescent() noexcept;

    ScreenCaptureSessionIdentity session_{};
    ScreenCaptureOperation pending_{};
    ScreenCaptureLifecyclePhase phase_ = ScreenCaptureLifecyclePhase::idle;
    PendingOperation pending_kind_ = PendingOperation::none;
    uint64_t next_token_ = 1;
    uint32_t callback_borrowers_ = 0;
    uint32_t frame_leases_ = 0;
    bool accepts_callbacks_ = false;
    bool retirement_requested_ = false;
};

} // namespace saccade::platform::macos

#if defined(__OBJC__)

#import <Foundation/Foundation.h>

@interface SaccadeScreenCaptureSessionState : NSObject
- (instancetype)initWithSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)requestStart;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)requestStop;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)cancel;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)timeout:(saccade::platform::macos::ScreenCaptureOperation)operation;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)completeStart:(saccade::platform::macos::ScreenCaptureOperation)operation
                                                                     result:(SaccadeResult)result;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)completeStop:(saccade::platform::macos::ScreenCaptureOperation)operation
                                                                    result:(SaccadeResult)result;
- (BOOL)beginCallbackForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session;
- (void*)beginCallbackContextForSession:(saccade::platform::macos::ScreenCaptureSessionIdentity)session;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)endCallbackForSession:
    (saccade::platform::macos::ScreenCaptureSessionIdentity)session;
- (SaccadeResult)acquireFrameLease;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)releaseFrameLease;
- (saccade::platform::macos::ScreenCaptureLifecycleTransition)shutdown;
- (saccade::platform::macos::ScreenCaptureLifecycleSnapshot)snapshot;
- (void)setCallbackContext:(void*)context;
- (BOOL)clearCallbackContext;
- (void)releaseDetachedLifetimeHold;
- (dispatch_queue_t)callbackQueue;
- (void)signalAvailable;
- (long)waitAvailable:(dispatch_time_t)timeout;
- (void)drainAvailable;
- (void)markNativeStopError;
- (void)clearNativeStopError;
- (void)nativeDidStopWithError;
- (BOOL)nativeStopFailed;
@end

#endif

#endif

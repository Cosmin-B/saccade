#ifndef SACCADE_PLATFORM_MACOS_EXPLICIT_WINDOW_CAPTURE_HPP
#define SACCADE_PLATFORM_MACOS_EXPLICIT_WINDOW_CAPTURE_HPP

#include "platform/macos/explicit_window_session.hpp"
#include "platform/macos/scene_capture.hpp"

#include <cstdint>
#include <pthread.h>

namespace saccade::platform::macos {

using ReadNativeFrameFn = SaccadeResult (*)(void*, SaccadeCaptureStreamHandle, SaccadeFrameHandle, NativeCapturedFrame*) noexcept;

/* Owner-thread capture lease for one exact public window source. A selected
   stream is never retargeted; replacement requires retirement and a newer
   session epoch. A published frame remains releasable while retirement drains.
   Shutdown waits for its release; destruction invalidates any remaining frame. */
class ExplicitWindowCapture final {
  public:
    ExplicitWindowCapture() noexcept = default;
    ~ExplicitWindowCapture();

    ExplicitWindowCapture(const ExplicitWindowCapture&) = delete;
    ExplicitWindowCapture& operator=(const ExplicitWindowCapture&) = delete;
    ExplicitWindowCapture(ExplicitWindowCapture&&) = delete;
    ExplicitWindowCapture& operator=(ExplicitWindowCapture&&) = delete;

    SaccadeResult initialize(SaccadeCaptureProviderDesc, void* native_context, ReadNativeFrameFn, uint32_t max_width,
                             uint32_t max_height) noexcept;
    SaccadeResult select(const ExplicitWindowIdentity&, uint64_t session_epoch) noexcept;
    SaccadeResult synchronize(const ExplicitWindowIdentity&) noexcept;
    SaccadeResult acquire(SceneCaptureFrame*) noexcept;
    SaccadeResult release(const SceneCaptureFrame&) noexcept;
    SaccadeResult retire(ExplicitWindowRetirementReason) noexcept;
    SaccadeResult drain_retirement() noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] bool retiring() const noexcept { return retiring_; }

    [[nodiscard]] uint64_t session_epoch() const noexcept { return session_epoch_; }

    [[nodiscard]] ExplicitWindowRetirementReason retirement_reason() const noexcept { return retirement_reason_; }

  private:
    [[nodiscard]] bool owns_thread() const noexcept;
    SaccadeResult find_source(const ExplicitWindowIdentity&, SaccadeCaptureSourceInfo*) noexcept;
    SaccadeResult finalize_retirement() noexcept;
    SaccadeResult release_internal_frame() noexcept;

    SaccadeCaptureProviderDesc backend_{};
    void* native_context_ = nullptr;
    ReadNativeFrameFn read_native_frame_ = nullptr;
    ExplicitWindowIdentity identity_{};
    SaccadeCaptureStreamHandle stream_ = 0;
    SaccadeFrameHandle leased_frame_ = 0;
    uint64_t session_epoch_ = 0;
    uint64_t last_session_epoch_ = 0;
    pthread_t owner_{};
    uint32_t max_width_ = 0;
    uint32_t max_height_ = 0;
    uint32_t generation_ = 1;
    ExplicitWindowRetirementReason retirement_reason_ = ExplicitWindowRetirementReason::none;
    bool initialized_ = false;
    bool active_ = false;
    bool retiring_ = false;
    bool running_ = false;
    bool leased_ = false;
    bool lease_published_ = false;
};

} // namespace saccade::platform::macos

#endif

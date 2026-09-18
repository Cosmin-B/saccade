#ifndef SACCADE_PLATFORM_MACOS_EXPLICIT_WINDOW_SESSION_HPP
#define SACCADE_PLATFORM_MACOS_EXPLICIT_WINDOW_SESSION_HPP

#include <saccade/saccade_backend.h>

#include <cstdint>

namespace saccade::platform::macos {

enum ExplicitWindowIdentityFlags : uint32_t {
    explicit_window_visible = UINT32_C(1) << 0,
    explicit_window_current_space = UINT32_C(1) << 1,
};

struct ExplicitWindowIdentity {
    uint64_t process_id = 0;
    uint64_t window_id = 0;
    uint64_t capture_source_id = 0;
    SaccadeRectI32 bounds{};
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct ExplicitWindowSceneGeneration {
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
};

enum class ExplicitWindowRetirementReason : uint32_t {
    none,
    replaced,
    disappeared,
    identity_changed,
    permission_lost,
    disconnected,
    shutdown,
};

struct ExplicitWindowActionToken {
    uint64_t session_epoch = 0;
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
    uint64_t process_id = 0;
    uint64_t window_id = 0;
    uint64_t capture_source_id = 0;
    SaccadeRectI32 bounds{};
};

struct ExplicitWindowActivationKey {
    uint64_t request_id = 0;
    uint64_t process_id = 0;
    uint64_t window_id = 0;
    uint64_t source_generation = 0;
    uint64_t deadline_ns = 0;
};

enum class ExplicitWindowActivationAdmission : uint8_t {
    start,
    resume,
    cancelled,
    busy,
    invalid,
};

/* Owner-thread state only. A cancelled key is retained until the caller
   observes cancellation, preventing a retained socket request from starting
   the activation again. */
class ExplicitWindowActivationState final {
  public:
    ExplicitWindowActivationAdmission admit(const ExplicitWindowActivationKey&) noexcept;
    SaccadeResult set_previous_scene_epoch(uint64_t) noexcept;
    void cancel() noexcept;
    void complete() noexcept;

    [[nodiscard]] bool waiting() const noexcept { return phase_ == Phase::waiting; }

    [[nodiscard]] uint64_t previous_scene_epoch() const noexcept { return previous_scene_epoch_; }

  private:
    enum class Phase : uint8_t {
        idle,
        waiting,
        cancelled,
    };

    ExplicitWindowActivationKey key_{};
    uint64_t previous_scene_epoch_ = 0;
    Phase phase_ = Phase::idle;
};

/* Owner-thread identity state only. This object performs no platform calls,
   allocation, synchronization, capture retirement, or input dispatch. */
class ExplicitWindowSession final {
  public:
    SaccadeResult select(const ExplicitWindowIdentity&, uint64_t session_epoch) noexcept;
    SaccadeResult publish(const ExplicitWindowIdentity&, const ExplicitWindowSceneGeneration&, ExplicitWindowActionToken*) noexcept;
    SaccadeResult validate(const ExplicitWindowIdentity&, const ExplicitWindowActionToken&) const noexcept;
    void retire(ExplicitWindowRetirementReason) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] uint64_t session_epoch() const noexcept { return session_epoch_; }

    [[nodiscard]] ExplicitWindowRetirementReason retirement_reason() const noexcept { return retirement_reason_; }

  private:
    ExplicitWindowIdentity identity_{};
    ExplicitWindowSceneGeneration generation_{};
    uint64_t session_epoch_ = 0;
    ExplicitWindowRetirementReason retirement_reason_ = ExplicitWindowRetirementReason::none;
    bool active_ = false;
};

static_assert(sizeof(ExplicitWindowIdentity) == 48);
static_assert(sizeof(ExplicitWindowSceneGeneration) == 40);
static_assert(sizeof(ExplicitWindowActionToken) == 88);
static_assert(sizeof(ExplicitWindowActivationKey) == 40);

} // namespace saccade::platform::macos

#endif

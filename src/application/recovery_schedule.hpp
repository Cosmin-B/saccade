#ifndef SACCADE_APPLICATION_RECOVERY_SCHEDULE_HPP
#define SACCADE_APPLICATION_RECOVERY_SCHEDULE_HPP

#include <algorithm>
#include <cstdint>
#include <limits>

namespace saccade::application {

constexpr uint64_t recovery_initial_delay_ns = UINT64_C(250'000'000);
constexpr uint64_t recovery_maximum_delay_ns = UINT64_C(8'000'000'000);
constexpr uint32_t recovery_maximum_backoff_shift = 5;

class RecoverySchedule final {
  public:
    void start(uint64_t now_ns) noexcept {
        attempt_ = 0;
        schedule(now_ns);
    }

    void retry(uint64_t now_ns) noexcept {
        attempt_ = std::min(attempt_ + 1U, recovery_maximum_backoff_shift);
        schedule(now_ns);
    }

    void complete() noexcept {
        next_attempt_ns_ = 0;
        attempt_ = 0;
        pending_ = false;
    }

    [[nodiscard]] bool due(uint64_t now_ns) const noexcept { return pending_ && now_ns >= next_attempt_ns_; }

    [[nodiscard]] bool pending() const noexcept { return pending_; }

    [[nodiscard]] uint32_t attempt() const noexcept { return attempt_; }

    [[nodiscard]] uint64_t next_attempt_ns() const noexcept { return next_attempt_ns_; }

  private:
    void schedule(uint64_t now_ns) noexcept {
        const uint64_t delay = std::min(recovery_initial_delay_ns << attempt_, recovery_maximum_delay_ns);
        next_attempt_ns_ = now_ns > std::numeric_limits<uint64_t>::max() - delay ? std::numeric_limits<uint64_t>::max()
                                                                                 : now_ns + delay;
        pending_ = true;
    }

    uint64_t next_attempt_ns_ = 0;
    uint32_t attempt_ = 0;
    bool pending_ = false;
};

static_assert(recovery_initial_delay_ns << recovery_maximum_backoff_shift == recovery_maximum_delay_ns);

} // namespace saccade::application

#endif

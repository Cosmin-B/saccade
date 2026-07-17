#include "application/recovery_schedule.hpp"

#include <cstdint>
#include <limits>

namespace {

enum class TestResult : int {
    success,
    initial_delay_failed,
    backoff_failed,
    maximum_failed,
    saturation_failed,
    completion_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    saccade::application::RecoverySchedule recovery;
    constexpr uint64_t start_ns = 100;
    recovery.start(start_ns);
    if (!recovery.pending() || recovery.attempt() != 0 ||
        recovery.next_attempt_ns() != start_ns + saccade::application::recovery_initial_delay_ns ||
        recovery.due(recovery.next_attempt_ns() - 1U) || !recovery.due(recovery.next_attempt_ns()))
        return result(TestResult::initial_delay_failed);

    recovery.retry(recovery.next_attempt_ns());
    if (recovery.attempt() != 1 ||
        recovery.next_attempt_ns() != start_ns + 3U * saccade::application::recovery_initial_delay_ns)
        return result(TestResult::backoff_failed);

    for (uint32_t index = 0; index < 16; ++index)
        recovery.retry(recovery.next_attempt_ns());
    if (recovery.attempt() != saccade::application::recovery_maximum_backoff_shift)
        return result(TestResult::maximum_failed);

    recovery.start(std::numeric_limits<uint64_t>::max());
    if (recovery.next_attempt_ns() != std::numeric_limits<uint64_t>::max())
        return result(TestResult::saturation_failed);

    recovery.complete();
    if (recovery.pending() || recovery.attempt() != 0 || recovery.next_attempt_ns() != 0)
        return result(TestResult::completion_failed);
    return result(TestResult::success);
}

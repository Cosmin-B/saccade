#include "scheduler/dual_rate_scheduler.hpp"

#include <cstdint>

int main() {
    using saccade::scheduler::DualRateScheduler;
    using saccade::scheduler::ScheduledWork;

    DualRateScheduler scheduler;
    ScheduledWork work{};
    if (scheduler.advance(0, &work) != SACCADE_ERROR_STATE || scheduler.initialize(100) != SACCADE_OK ||
        scheduler.initialize(100) != SACCADE_ERROR_ALREADY_EXISTS || scheduler.advance(100, &work) != SACCADE_OK ||
        !work.interaction_due || !work.scene_start || work.interaction_time_ns != 100 || work.scene_time_ns != 100 ||
        !scheduler.scene_running() || scheduler.scene_pending()) {
        return 1;
    }

    if (scheduler.advance(UINT64_C(8'333'432), &work) != SACCADE_OK || work.interaction_due || work.scene_start ||
        scheduler.advance(UINT64_C(8'333'433), &work) != SACCADE_OK || !work.interaction_due || work.scene_start ||
        work.interaction_time_ns != UINT64_C(8'333'433)) {
        return 2;
    }

    if (scheduler.advance(UINT64_C(33'333'433), &work) != SACCADE_OK || !work.interaction_due || work.scene_start ||
        !scheduler.scene_pending()) {
        return 3;
    }
    if (scheduler.advance(UINT64_C(100'000'099), &work) != SACCADE_OK || !work.interaction_due || work.scene_start ||
        scheduler.stats().scene_replaced != 2) {
        return 4;
    }

    if (scheduler.complete_scene(&work) != SACCADE_OK || !work.scene_start ||
        work.scene_time_ns != UINT64_C(100'000'099) || !scheduler.scene_running() || scheduler.scene_pending() ||
        scheduler.complete_scene(&work) != SACCADE_OK || work.scene_start || scheduler.scene_running() ||
        scheduler.complete_scene(&work) != SACCADE_ERROR_STATE) {
        return 5;
    }

    if (scheduler.advance(UINT64_C(100'000'100), &work) != SACCADE_OK || work.interaction_due || work.scene_start ||
        scheduler.advance(UINT64_C(133'333'432), &work) != SACCADE_OK || !work.interaction_due || !work.scene_start ||
        work.scene_time_ns != UINT64_C(133'333'432)) {
        return 6;
    }
    if (scheduler.advance(UINT64_C(166'666'765), &work) != SACCADE_OK || work.scene_start ||
        scheduler.cancel_scene(false, &work) != SACCADE_OK || !work.scene_start || !scheduler.scene_running() ||
        scheduler.cancel_scene(true, &work) != SACCADE_OK || scheduler.scene_running() || scheduler.scene_pending()) {
        return 7;
    }

    const auto stats = scheduler.stats();
    if (stats.interaction_deadlines != 21 || stats.interaction_ticks != 6 || stats.interaction_skipped != 15 ||
        stats.scene_deadlines != 6 || stats.scene_started != 4 || stats.scene_completed != 2 ||
        stats.scene_replaced != 2 || stats.time_regressions != 0) {
        return 8;
    }
    if (scheduler.advance(1, &work) != SACCADE_ERROR_INVALID_ARGUMENT || scheduler.stats().time_regressions != 1) {
        return 9;
    }

    DualRateScheduler invalid;
    if (invalid.initialize(0, {0, 1}) != SACCADE_ERROR_INVALID_ARGUMENT ||
        invalid.initialize(0, {1, 0}) != SACCADE_ERROR_INVALID_ARGUMENT ||
        invalid.advance(0, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 10;
    }
    return 0;
}

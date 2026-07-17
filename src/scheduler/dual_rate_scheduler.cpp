#include "scheduler/dual_rate_scheduler.hpp"

#include <limits>

namespace saccade::scheduler {
namespace {

struct DueSet {
    uint64_t count = 0;
    uint64_t newest_ns = 0;
};

DueSet consume_due(uint64_t now_ns, uint64_t period_ns, uint64_t* next_ns) noexcept {
    if (now_ns < *next_ns) {
        return {};
    }
    const uint64_t count = ((now_ns - *next_ns) / period_ns) + 1U;
    const uint64_t newest = *next_ns + ((count - 1U) * period_ns);
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    *next_ns = count > ((maximum - *next_ns) / period_ns) ? maximum : *next_ns + (count * period_ns);
    return {count, newest};
}

void clear_work(ScheduledWork* output) noexcept {
    *output = {};
}

} // namespace

SaccadeResult DualRateScheduler::initialize(uint64_t now_ns, DualRateConfig config) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    if (config.interaction_period_ns == 0 || config.scene_period_ns == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    config_ = config;
    last_now_ns_ = now_ns;
    next_interaction_ns_ = now_ns;
    next_scene_ns_ = now_ns;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DualRateScheduler::advance(uint64_t now_ns, ScheduledWork* output) noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    clear_work(output);
    if (!initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (now_ns < last_now_ns_) {
        ++stats_.time_regressions;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    last_now_ns_ = now_ns;

    const DueSet interaction = consume_due(now_ns, config_.interaction_period_ns, &next_interaction_ns_);
    if (interaction.count != 0) {
        output->interaction_due = true;
        output->interaction_time_ns = interaction.newest_ns;
        stats_.interaction_deadlines += interaction.count;
        ++stats_.interaction_ticks;
        stats_.interaction_skipped += interaction.count - 1U;
    }

    const DueSet scene = consume_due(now_ns, config_.scene_period_ns, &next_scene_ns_);
    if (scene.count == 0) {
        return SACCADE_OK;
    }
    stats_.scene_deadlines += scene.count;
    if (!scene_running_) {
        output->scene_start = true;
        output->scene_time_ns = scene.newest_ns;
        scene_running_ = true;
        ++stats_.scene_started;
        stats_.scene_replaced += scene.count - 1U;
        return SACCADE_OK;
    }

    stats_.scene_replaced += scene.count - 1U;
    if (scene_pending_) {
        ++stats_.scene_replaced;
    }
    pending_scene_ns_ = scene.newest_ns;
    scene_pending_ = true;
    return SACCADE_OK;
}

SaccadeResult DualRateScheduler::complete_scene(ScheduledWork* output) noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    clear_work(output);
    if (!initialized_ || !scene_running_) {
        return SACCADE_ERROR_STATE;
    }
    ++stats_.scene_completed;
    if (scene_pending_) {
        output->scene_start = true;
        output->scene_time_ns = pending_scene_ns_;
        scene_pending_ = false;
        ++stats_.scene_started;
    } else {
        scene_running_ = false;
    }
    return SACCADE_OK;
}

SaccadeResult DualRateScheduler::cancel_scene(bool discard_pending, ScheduledWork* output) noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    clear_work(output);
    if (!initialized_ || !scene_running_) {
        return SACCADE_ERROR_STATE;
    }
    if (discard_pending || !scene_pending_) {
        scene_running_ = false;
        scene_pending_ = false;
        return SACCADE_OK;
    }
    output->scene_start = true;
    output->scene_time_ns = pending_scene_ns_;
    scene_pending_ = false;
    ++stats_.scene_started;
    return SACCADE_OK;
}

} // namespace saccade::scheduler

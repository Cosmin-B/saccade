#include "scheduler/desktop_neural_coordinator.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace saccade::scheduler {
namespace {

bool frame_valid(const DesktopNeuralFrame& frame) noexcept {
    const geometry::TransformDesc& transform = frame.source_to_desktop.descriptor();
    return frame.frame != 0 && frame.source_id != 0 && frame.topology_epoch != 0 && frame.transform_epoch != 0 &&
           frame.scene_transform_epoch != 0 && frame.width != 0 && frame.height != 0 && frame.source_count != 0 &&
           frame.source_count <= desktop_neural_source_capacity && frame.reserved == 0 &&
           frame.width <= static_cast<uint32_t>(INT32_MAX) && frame.height <= static_cast<uint32_t>(INT32_MAX) &&
           frame.source_to_desktop.valid() && transform.epoch == frame.transform_epoch &&
           transform.destination_space == geometry::CoordinateSpace::desktop;
}

bool better(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    if (left.confidence_q16 != right.confidence_q16) {
        return left.confidence_q16 > right.confidence_q16;
    }
    if (left.y_q8 != right.y_q8) return left.y_q8 < right.y_q8;
    if (left.x_q8 != right.x_q8) return left.x_q8 < right.x_q8;
    return left.target_id < right.target_id;
}

void sift_up(SaccadeTargetRecord* heap, uint32_t index) noexcept {
    while (index != 0) {
        const uint32_t parent = (index - 1U) / 2U;
        if (!better(heap[parent], heap[index])) break;
        std::swap(heap[parent], heap[index]);
        index = parent;
    }
}

void sift_down(SaccadeTargetRecord* heap, uint32_t count, uint32_t index) noexcept {
    for (;;) {
        const uint32_t left = index * 2U + 1U;
        if (left >= count) return;
        const uint32_t right = left + 1U;
        uint32_t worse = left;
        if (right < count && better(heap[left], heap[right])) worse = right;
        if (!better(heap[index], heap[worse])) return;
        std::swap(heap[index], heap[worse]);
        index = worse;
    }
}

} // namespace

DesktopNeuralCoordinator::~DesktopNeuralCoordinator() {
    (void)shutdown();
}

SaccadeResult DesktopNeuralCoordinator::initialize(const DesktopNeuralCoordinatorConfig& config,
                                                   DesktopNeuralCoordinatorStorage* storage,
                                                   scene::SceneStore* scenes) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (storage == nullptr || scenes == nullptr || config.runtime == 0 || config.session == 0 ||
        config.model_epoch == 0 || config.session_epoch == 0 || config.desktop_source_id == 0 ||
        config.first_scene_epoch == 0 || config.maximum_output_bytes == 0 ||
        config.maximum_output_bytes > storage->inference_output.size() || config.maximum_targets == 0 ||
        config.maximum_targets > SACCADE_TARGET_PACKET_MAX_TARGETS) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult scheduled = scheduler_.initialize(config.start_time_ns, config.rates);
    if (scheduled != SACCADE_OK) return scheduled;
    config_ = config;
    storage_ = storage;
    scenes_ = scenes;
    next_scene_epoch_ = config.first_scene_epoch;
    initialized_ = true;
    return SACCADE_OK;
}

void DesktopNeuralCoordinator::release_frame(DesktopNeuralFrame* frame) noexcept {
    if (frame->frame == 0) return;
    const SaccadeFrameHandle handle = frame->frame;
    (void)saccade_frame_release(config_.runtime, handle);
    if (frame->retire != nullptr) {
        frame->retire(frame->retire_context, handle);
    }
    *frame = {};
}

void DesktopNeuralCoordinator::clear_frames(
    std::array<DesktopNeuralFrame, desktop_neural_source_capacity>* frames) noexcept {
    for (DesktopNeuralFrame& frame : *frames)
        release_frame(&frame);
}

SaccadeResult DesktopNeuralCoordinator::offer(DesktopNeuralFrame frame) noexcept {
    if (!initialized_ || !frame_valid(frame)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    DesktopNeuralFrame* empty = nullptr;
    for (DesktopNeuralFrame& pending : pending_) {
        if (pending.frame == frame.frame || running_.frame == frame.frame) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (pending.frame != 0 && pending.source_id == frame.source_id) {
            if (frame.topology_epoch < pending.topology_epoch ||
                (frame.topology_epoch == pending.topology_epoch &&
                 frame.scene_transform_epoch < pending.scene_transform_epoch)) {
                release_frame(&frame);
                ++stats_.frames_stale;
                return SACCADE_ERROR_STALE_HANDLE;
            }
            release_frame(&pending);
            pending = frame;
            ++stats_.frames_offered;
            ++stats_.frames_replaced;
            return SACCADE_OK;
        }
        if (pending.frame == 0 && empty == nullptr) empty = &pending;
    }
    if (empty == nullptr) return SACCADE_ERROR_CAPACITY;
    *empty = frame;
    ++stats_.frames_offered;
    return SACCADE_OK;
}

SaccadeResult DesktopNeuralCoordinator::begin_batch(DesktopNeuralAdvance* advance) noexcept {
    uint64_t topology_epoch = 0;
    uint64_t scene_transform_epoch = 0;
    for (const DesktopNeuralFrame& frame : pending_) {
        if (frame.frame == 0) continue;
        if (frame.topology_epoch > topology_epoch ||
            (frame.topology_epoch == topology_epoch && frame.scene_transform_epoch > scene_transform_epoch)) {
            topology_epoch = frame.topology_epoch;
            scene_transform_epoch = frame.scene_transform_epoch;
        }
    }
    uint32_t available_count = 0;
    uint32_t expected_count = 0;
    for (const DesktopNeuralFrame& frame : pending_) {
        if (frame.frame == 0 || frame.topology_epoch != topology_epoch ||
            frame.scene_transform_epoch != scene_transform_epoch) {
            continue;
        }
        if (expected_count == 0) expected_count = frame.source_count;
        if (frame.source_count != expected_count) {
            ++stats_.failures;
            return SACCADE_ERROR_STATE;
        }
        ++available_count;
    }
    if (available_count != 0 && available_count < expected_count) {
        ++stats_.batches_incomplete;
        advance->sources_expected = expected_count;
        ScheduledWork promoted{};
        const SaccadeResult completed = scheduler_.complete_scene(&promoted);
        if (completed != SACCADE_OK) return completed;
        return promoted.scene_start ? begin_batch(advance) : SACCADE_OK;
    }
    if (available_count > expected_count) {
        ++stats_.failures;
        return SACCADE_ERROR_STATE;
    }

    batch_count_ = 0;
    batch_index_ = 0;
    batch_expected_count_ = expected_count;
    batch_completed_count_ = 0;
    batch_failed_count_ = 0;
    batch_capture_time_ns_ = UINT64_MAX;
    for (DesktopNeuralFrame& frame : pending_) {
        if (frame.frame == 0) continue;
        if (frame.topology_epoch != topology_epoch || frame.scene_transform_epoch != scene_transform_epoch) {
            release_frame(&frame);
            ++stats_.frames_stale;
            continue;
        }
        batch_[batch_count_++] = frame;
        if (frame.capture_time_ns != 0)
            batch_capture_time_ns_ = std::min(batch_capture_time_ns_, frame.capture_time_ns);
        frame = {};
    }
    if (batch_count_ == 0) {
        ScheduledWork promoted{};
        const SaccadeResult completed = scheduler_.complete_scene(&promoted);
        if (completed != SACCADE_OK) return completed;
        return promoted.scene_start ? begin_batch(advance) : SACCADE_OK;
    }
    advance->sources_expected = batch_expected_count_;
    std::sort(batch_.begin(), batch_.begin() + batch_count_,
              [](const DesktopNeuralFrame& left, const DesktopNeuralFrame& right) noexcept {
                  return left.source_id < right.source_id;
              });
    if (next_scene_epoch_ == std::numeric_limits<uint64_t>::max()) {
        clear_frames(&batch_);
        return SACCADE_ERROR_CAPACITY;
    }
    const SaccadeResult begun = scenes_->begin_write(&aggregate_);
    if (begun != SACCADE_OK) {
        clear_frames(&batch_);
        ++stats_.failures;
        return begun;
    }
    aggregate_header_ = {};
    aggregate_header_.struct_size = sizeof(aggregate_header_);
    aggregate_header_.packet_version = SACCADE_TARGET_PACKET_VERSION;
    aggregate_header_.target_stride = sizeof(SaccadeTargetRecord);
    aggregate_header_.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    aggregate_header_.scene_epoch = next_scene_epoch_;
    aggregate_header_.capture_time_ns = batch_capture_time_ns_ == UINT64_MAX ? 0 : batch_capture_time_ns_;
    aggregate_header_.model_epoch = config_.model_epoch;
    aggregate_header_.session_epoch = config_.session_epoch;
    aggregate_header_.transform_epoch = scene_transform_epoch;
    aggregate_header_.topology_epoch = topology_epoch;
    aggregate_header_.source_id = config_.desktop_source_id;
    aggregate_header_.targets_offset = sizeof(aggregate_header_);
    aggregate_header_.total_size = sizeof(aggregate_header_);
    std::memcpy(aggregate_.data, &aggregate_header_, sizeof(aggregate_header_));
    heap_size_ = 0;
    batch_active_ = true;
    batch_started_ns_ = current_time_ns_;
    ++stats_.batches_started;
    return start_next(advance);
}

SaccadeResult DesktopNeuralCoordinator::start_next(DesktopNeuralAdvance* advance) noexcept {
    while (batch_index_ < batch_count_) {
        running_ = batch_[batch_index_];
        batch_[batch_index_++] = {};
        SaccadeInferenceSubmitDesc submit{};
        submit.struct_size = sizeof(submit);
        submit.api_version = SACCADE_API_VERSION;
        submit.frame = running_.frame;
        submit.scope = {0, 0, static_cast<int32_t>(running_.width), static_cast<int32_t>(running_.height)};
        submit.output_capacity = config_.maximum_output_bytes;
        submit.model_epoch = config_.model_epoch;
        submit.session_epoch = config_.session_epoch;
        submit.transform_epoch = running_.transform_epoch;
        submit.topology_epoch = running_.topology_epoch;
        submit.source_id = running_.source_id;
        const SaccadeResult submitted =
            saccade_inference_submit(config_.runtime, config_.session, &submit, &running_ticket_);
        if (submitted == SACCADE_OK) {
            ++stats_.sources_submitted;
            return SACCADE_OK;
        }
        release_frame(&running_);
        ++stats_.sources_failed;
        ++stats_.failures;
        ++batch_failed_count_;
        ++advance->sources_failed;
    }
    return finish_batch(advance);
}

void DesktopNeuralCoordinator::consider_target(SaccadeTargetRecord target) noexcept {
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(aggregate_.data + sizeof(SaccadeTargetPacketHeader));
    ++stats_.targets_considered;
    if (heap_size_ < config_.maximum_targets) {
        targets[heap_size_] = target;
        sift_up(targets, heap_size_++);
    } else if (better(target, targets[0])) {
        targets[0] = target;
        sift_down(targets, heap_size_, 0);
        ++stats_.targets_replaced;
    }
}

SaccadeResult DesktopNeuralCoordinator::append_output(size_t byte_size) noexcept {
    scene::PacketView input{};
    const SaccadeResult validated = scene::validate_packet({storage_->inference_output.data(), byte_size}, &input);
    if (validated != SACCADE_OK) return validated;
    const SaccadeTargetPacketHeader& header = *input.header;
    if (header.coordinate_space == SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || header.scene_epoch != 0 ||
        header.frame_id == 0 || header.model_epoch != config_.model_epoch ||
        header.session_epoch != config_.session_epoch || header.transform_epoch != running_.transform_epoch ||
        header.topology_epoch != running_.topology_epoch || header.source_id != running_.source_id) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    aggregate_header_.frame_id = std::max(aggregate_header_.frame_id, header.frame_id);
    for (uint32_t index = 0; index < header.target_count; ++index) {
        const SaccadeTargetRecord& source = input.targets[index];
        geometry::RectQ8 mapped{};
        const SaccadeResult mapped_result = running_.source_to_desktop.map_rect_clipped(
            {source.x_q8, source.y_q8, source.width_q8, source.height_q8}, &mapped);
        if (mapped_result == SACCADE_ERROR_NOT_FOUND) continue;
        if (mapped_result != SACCADE_OK) return mapped_result;
        geometry::PointQ8 safe{};
        if (running_.source_to_desktop.map_point({source.safe_x_q8, source.safe_y_q8}, &safe) != SACCADE_OK ||
            safe.x < mapped.x || safe.y < mapped.y ||
            static_cast<int64_t>(safe.x) >= static_cast<int64_t>(mapped.x) + mapped.width ||
            static_cast<int64_t>(safe.y) >= static_cast<int64_t>(mapped.y) + mapped.height) {
            safe = {mapped.x + mapped.width / 2, mapped.y + mapped.height / 2};
        }
        SaccadeTargetRecord target = source;
        target.x_q8 = mapped.x;
        target.y_q8 = mapped.y;
        target.width_q8 = mapped.width;
        target.height_q8 = mapped.height;
        target.safe_x_q8 = safe.x;
        target.safe_y_q8 = safe.y;
        consider_target(target);
    }
    return SACCADE_OK;
}

SaccadeResult DesktopNeuralCoordinator::retire_running(DesktopNeuralAdvance* advance) noexcept {
    if (running_ticket_ == 0) return SACCADE_OK;
    SaccadeInferenceStatus status{};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    const SaccadeResult polled = saccade_inference_poll(config_.runtime, config_.session, running_ticket_, &status);
    if (polled != SACCADE_OK) {
        (void)saccade_inference_reset(config_.runtime, config_.session);
        release_frame(&running_);
        running_ticket_ = 0;
        ++stats_.sources_failed;
        ++stats_.failures;
        ++batch_failed_count_;
        ++advance->sources_failed;
        const SaccadeResult next = start_next(advance);
        return next == SACCADE_OK ? polled : next;
    }
    if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING) return SACCADE_OK;
    size_t required = 0;
    const SaccadeResult collected =
        saccade_inference_collect(config_.runtime, config_.session, running_ticket_,
                                  {storage_->inference_output.data(), config_.maximum_output_bytes}, &required);
    SaccadeResult result = collected;
    if (status.state == SACCADE_TICKET_COMPLETE && collected == SACCADE_OK) {
        result = append_output(required);
    }
    if (result == SACCADE_OK) {
        ++stats_.sources_completed;
        ++batch_completed_count_;
        ++advance->sources_completed;
    } else {
        ++stats_.sources_failed;
        ++stats_.failures;
        ++batch_failed_count_;
        ++advance->sources_failed;
    }
    release_frame(&running_);
    running_ticket_ = 0;
    const SaccadeResult next = start_next(advance);
    return next == SACCADE_OK ? result : next;
}

SaccadeResult DesktopNeuralCoordinator::finish_batch(DesktopNeuralAdvance* advance) noexcept {
    const bool scope_complete =
        batch_expected_count_ == batch_count_ && batch_completed_count_ == batch_count_ && batch_failed_count_ == 0;
    if (!scope_complete) {
        const SaccadeResult aborted = scenes_->abort_write(aggregate_);
        if (aborted != SACCADE_OK) {
            ++stats_.failures;
            return aborted;
        }
        ++stats_.batches_incomplete;
        aggregate_ = {};
        aggregate_header_ = {};
        batch_active_ = false;
        batch_count_ = 0;
        batch_index_ = 0;
        batch_expected_count_ = 0;
        batch_completed_count_ = 0;
        batch_failed_count_ = 0;
        heap_size_ = 0;
        batch_capture_time_ns_ = 0;
        ScheduledWork promoted{};
        const SaccadeResult completed = scheduler_.complete_scene(&promoted);
        if (completed != SACCADE_OK) return completed;
        return promoted.scene_start ? begin_batch(advance) : SACCADE_OK;
    }

    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(aggregate_.data + sizeof(SaccadeTargetPacketHeader));
    for (uint32_t count = heap_size_; count > 1; --count) {
        std::swap(targets[0], targets[count - 1U]);
        sift_down(targets, count - 1U, 0);
    }
    for (uint32_t index = 0; index < heap_size_; ++index) {
        targets[index].order = index;
    }
    aggregate_header_.target_count = heap_size_;
    aggregate_header_.total_size =
        sizeof(aggregate_header_) + static_cast<uint64_t>(heap_size_) * sizeof(SaccadeTargetRecord);
    std::memcpy(aggregate_.data, &aggregate_header_, sizeof(aggregate_header_));
    const SaccadeResult committed =
        scenes_->commit_trusted(aggregate_, static_cast<size_t>(aggregate_header_.total_size));
    if (committed != SACCADE_OK) {
        (void)scenes_->abort_write(aggregate_);
        ++stats_.failures;
        return committed;
    }
    ++next_scene_epoch_;
    ++stats_.batches_published;
    stats_.targets_published += heap_size_;
    const uint64_t batch_latency_ns = current_time_ns_ >= batch_started_ns_ ? current_time_ns_ - batch_started_ns_ : 0;
    const uint64_t full_scope_latency_ns =
        batch_capture_time_ns_ != UINT64_MAX && current_time_ns_ >= batch_capture_time_ns_
            ? current_time_ns_ - batch_capture_time_ns_
            : 0;
    stats_.batch_latency_total_ns += batch_latency_ns;
    stats_.batch_latency_max_ns = std::max(stats_.batch_latency_max_ns, batch_latency_ns);
    if (batch_latency_ns > config_.rates.scene_period_ns) ++stats_.batch_deadlines_missed;
    stats_.full_scope_latency_total_ns += full_scope_latency_ns;
    stats_.full_scope_latency_max_ns = std::max(stats_.full_scope_latency_max_ns, full_scope_latency_ns);
    if (full_scope_latency_ns > config_.rates.scene_period_ns) ++stats_.full_scope_deadlines_missed;
    advance->scene_published = true;
    advance->scope_complete = true;
    advance->target_count = heap_size_;
    advance->scene_epoch = aggregate_header_.scene_epoch;
    advance->frame_id = aggregate_header_.frame_id;
    advance->transform_epoch = aggregate_header_.transform_epoch;
    advance->topology_epoch = aggregate_header_.topology_epoch;
    advance->batch_latency_ns = batch_latency_ns;
    advance->full_scope_latency_ns = full_scope_latency_ns;
    aggregate_ = {};
    aggregate_header_ = {};
    batch_active_ = false;
    batch_count_ = 0;
    batch_index_ = 0;
    batch_expected_count_ = 0;
    batch_completed_count_ = 0;
    batch_failed_count_ = 0;
    heap_size_ = 0;
    batch_capture_time_ns_ = 0;
    ScheduledWork promoted{};
    const SaccadeResult completed = scheduler_.complete_scene(&promoted);
    if (completed != SACCADE_OK) return completed;
    return promoted.scene_start ? begin_batch(advance) : SACCADE_OK;
}

SaccadeResult DesktopNeuralCoordinator::advance(uint64_t now_ns, DesktopNeuralAdvance* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    current_time_ns_ = now_ns;
    *output = {};
    if (batch_active_) output->sources_expected = batch_expected_count_;
    SaccadeResult result = retire_running(output);
    ScheduledWork work{};
    const SaccadeResult advanced = scheduler_.advance(now_ns, &work);
    if (advanced != SACCADE_OK) return advanced;
    output->interaction_due = work.interaction_due;
    output->interaction_time_ns = work.interaction_time_ns;
    if (work.scene_start && !batch_active_ && running_ticket_ == 0) {
        const SaccadeResult begun = begin_batch(output);
        if (result == SACCADE_OK) result = begun;
    }
    return result;
}

SaccadeResult DesktopNeuralCoordinator::shutdown() noexcept {
    if (!initialized_) return SACCADE_OK;
    SaccadeResult result = SACCADE_OK;
    if (running_ticket_ != 0) {
        result = saccade_inference_cancel(config_.runtime, config_.session, running_ticket_);
        size_t required = 0;
        const SaccadeResult collected =
            saccade_inference_collect(config_.runtime, config_.session, running_ticket_,
                                      {storage_->inference_output.data(), config_.maximum_output_bytes}, &required);
        if (collected != SACCADE_ERROR_CANCELLED && collected != SACCADE_OK) {
            (void)saccade_inference_reset(config_.runtime, config_.session);
        }
        release_frame(&running_);
        running_ticket_ = 0;
    }
    clear_frames(&pending_);
    clear_frames(&batch_);
    if (batch_active_) {
        (void)scenes_->abort_write(aggregate_);
    }
    aggregate_ = {};
    initialized_ = false;
    return result;
}

} // namespace saccade::scheduler

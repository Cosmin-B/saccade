#include "scheduler/neural_coordinator.hpp"

#include <cstring>
#include <limits>

namespace saccade::scheduler {

NeuralCoordinator::~NeuralCoordinator() {
    (void)shutdown();
}

SaccadeResult NeuralCoordinator::initialize(const NeuralCoordinatorConfig& config, NeuralCoordinatorStorage* storage,
                                            scene::SceneStore* scenes) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    if (storage == nullptr || scenes == nullptr || config.runtime == 0 || config.session == 0 ||
        config.model_epoch == 0 || config.session_epoch == 0 || config.maximum_output_bytes == 0 ||
        config.maximum_output_bytes > storage->inference_output.size() || config.reserved != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult scheduled = scheduler_.initialize(config.start_time_ns, config.rates);
    if (scheduled != SACCADE_OK) {
        return scheduled;
    }
    config_ = config;
    storage_ = storage;
    scenes_ = scenes;
    initialized_ = true;
    return SACCADE_OK;
}

void NeuralCoordinator::release_frame(NeuralFrame* frame) noexcept {
    if (frame->frame != 0) {
        const SaccadeFrameHandle handle = frame->frame;
        (void)saccade_frame_release(config_.runtime, handle);
        if (frame->retire != nullptr) {
            frame->retire(frame->retire_context, handle);
        }
        *frame = {};
    }
}

SaccadeResult NeuralCoordinator::offer(NeuralFrame frame) noexcept {
    const geometry::TransformDesc& transform = frame.source_to_desktop.descriptor();
    if (!initialized_ || frame.frame == 0 || frame.source_id == 0 || frame.topology_epoch == 0 ||
        frame.transform_epoch == 0 || frame.width == 0 || frame.height == 0 ||
        frame.width > static_cast<uint32_t>(INT32_MAX) || frame.height > static_cast<uint32_t>(INT32_MAX) ||
        !frame.source_to_desktop.valid() || transform.epoch != frame.transform_epoch ||
        transform.destination_space != geometry::CoordinateSpace::desktop || pending_frame_.frame == frame.frame ||
        running_frame_.frame == frame.frame) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (pending_frame_.frame != 0) {
        release_frame(&pending_frame_);
        ++stats_.frames_replaced;
    }
    pending_frame_ = frame;
    ++stats_.frames_offered;
    return SACCADE_OK;
}

void NeuralCoordinator::finish_scene_without_frame() noexcept {
    ScheduledWork promoted{};
    if (scheduler_.complete_scene(&promoted) == SACCADE_OK && promoted.scene_start) {
        ++stats_.empty_scene_deadlines;
        finish_scene_without_frame();
    }
}

SaccadeResult NeuralCoordinator::start_pending(uint64_t, NeuralAdvance*) noexcept {
    if (pending_frame_.frame == 0) {
        ++stats_.empty_scene_deadlines;
        finish_scene_without_frame();
        return SACCADE_OK;
    }
    SaccadeInferenceSubmitDesc submit{};
    submit.struct_size = sizeof(submit);
    submit.api_version = SACCADE_API_VERSION;
    submit.frame = pending_frame_.frame;
    submit.scope = {0, 0, static_cast<int32_t>(pending_frame_.width), static_cast<int32_t>(pending_frame_.height)};
    submit.output_capacity = config_.maximum_output_bytes;
    submit.model_epoch = config_.model_epoch;
    submit.session_epoch = config_.session_epoch;
    submit.transform_epoch = pending_frame_.transform_epoch;
    submit.topology_epoch = pending_frame_.topology_epoch;
    submit.source_id = pending_frame_.source_id;
    const SaccadeResult submitted =
        saccade_inference_submit(config_.runtime, config_.session, &submit, &running_ticket_);
    if (submitted != SACCADE_OK) {
        ++stats_.failures;
        release_frame(&pending_frame_);
        finish_scene_without_frame();
        return submitted;
    }
    running_frame_ = pending_frame_;
    pending_frame_ = {};
    ++stats_.frames_submitted;
    return SACCADE_OK;
}

SaccadeResult NeuralCoordinator::publish_output(size_t byte_size, NeuralAdvance* advance) noexcept {
    scene::PacketView input{};
    const SaccadeResult validated = scene::validate_packet({storage_->inference_output.data(), byte_size}, &input);
    if (validated != SACCADE_OK) {
        ++stats_.invalid_outputs;
        return validated;
    }
    const SaccadeTargetPacketHeader& input_header = *input.header;
    if (input_header.coordinate_space == SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || input_header.scene_epoch != 0 ||
        input_header.frame_id == 0 || input_header.model_epoch != config_.model_epoch ||
        input_header.session_epoch != config_.session_epoch ||
        input_header.transform_epoch != running_frame_.transform_epoch ||
        input_header.topology_epoch != running_frame_.topology_epoch ||
        input_header.source_id != running_frame_.source_id) {
        ++stats_.stale_outputs;
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (next_scene_epoch_ == std::numeric_limits<uint64_t>::max()) {
        return SACCADE_ERROR_CAPACITY;
    }

    scene::MutableScenePacket output{};
    const SaccadeResult begun = scenes_->begin_write(&output);
    if (begun != SACCADE_OK) {
        return begun;
    }
    SaccadeTargetPacketHeader header = input_header;
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = next_scene_epoch_++;
    header.target_count = 0;
    std::memcpy(output.data, &header, sizeof(header));
    auto* output_targets = reinterpret_cast<SaccadeTargetRecord*>(output.data + sizeof(SaccadeTargetPacketHeader));
    for (uint32_t index = 0; index < input_header.target_count; ++index) {
        const SaccadeTargetRecord& source = input.targets[index];
        geometry::RectQ8 mapped{};
        const geometry::RectQ8 source_rect{source.x_q8, source.y_q8, source.width_q8, source.height_q8};
        const SaccadeResult mapped_result = running_frame_.source_to_desktop.map_rect_clipped(source_rect, &mapped);
        if (mapped_result == SACCADE_ERROR_NOT_FOUND) {
            continue;
        }
        if (mapped_result != SACCADE_OK) {
            (void)scenes_->abort_write(output);
            return mapped_result;
        }
        geometry::PointQ8 safe{};
        if (running_frame_.source_to_desktop.map_point({source.safe_x_q8, source.safe_y_q8}, &safe) != SACCADE_OK ||
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
        target.order = header.target_count;
        output_targets[header.target_count++] = target;
    }
    header.total_size = sizeof(header) + static_cast<uint64_t>(header.target_count) * sizeof(SaccadeTargetRecord);
    std::memcpy(output.data, &header, sizeof(header));
    const SaccadeResult committed = scenes_->commit_trusted(output, static_cast<size_t>(header.total_size));
    if (committed != SACCADE_OK) {
        (void)scenes_->abort_write(output);
        return committed;
    }
    ++stats_.scenes_published;
    stats_.targets_published += header.target_count;
    advance->scene_published = true;
    advance->target_count = header.target_count;
    advance->scene_epoch = header.scene_epoch;
    return SACCADE_OK;
}

SaccadeResult NeuralCoordinator::retire_running(NeuralAdvance* advance) noexcept {
    if (running_ticket_ == 0) {
        return SACCADE_OK;
    }
    SaccadeInferenceStatus status{};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    const SaccadeResult polled = saccade_inference_poll(config_.runtime, config_.session, running_ticket_, &status);
    if (polled != SACCADE_OK) {
        ++stats_.failures;
        (void)saccade_inference_reset(config_.runtime, config_.session);
        release_frame(&running_frame_);
        running_ticket_ = 0;
        finish_scene_without_frame();
        return polled;
    }
    if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING) {
        return SACCADE_OK;
    }

    size_t required = 0;
    const SaccadeResult collected =
        saccade_inference_collect(config_.runtime, config_.session, running_ticket_,
                                  {storage_->inference_output.data(), config_.maximum_output_bytes}, &required);
    SaccadeResult result = collected;
    if (status.state == SACCADE_TICKET_COMPLETE && collected == SACCADE_OK) {
        result = publish_output(required, advance);
        ++stats_.tickets_completed;
    } else if (status.state == SACCADE_TICKET_CANCELLED) {
        ++stats_.tickets_cancelled;
    } else {
        ++stats_.failures;
    }
    release_frame(&running_frame_);
    running_ticket_ = 0;
    ScheduledWork promoted{};
    const SaccadeResult completed = scheduler_.complete_scene(&promoted);
    if (completed != SACCADE_OK) {
        return completed;
    }
    if (promoted.scene_start) {
        const SaccadeResult started = start_pending(promoted.scene_time_ns, advance);
        if (started != SACCADE_OK && result == SACCADE_OK) {
            result = started;
        }
    }
    return result;
}

SaccadeResult NeuralCoordinator::advance(uint64_t now_ns, NeuralAdvance* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    SaccadeResult result = retire_running(output);
    ScheduledWork work{};
    const SaccadeResult advanced = scheduler_.advance(now_ns, &work);
    if (advanced != SACCADE_OK) {
        return advanced;
    }
    output->interaction_due = work.interaction_due;
    output->interaction_time_ns = work.interaction_time_ns;
    if (work.scene_start && running_ticket_ == 0) {
        const SaccadeResult started = start_pending(work.scene_time_ns, output);
        if (started != SACCADE_OK && result == SACCADE_OK) {
            result = started;
        }
    }
    return result;
}

SaccadeResult NeuralCoordinator::shutdown() noexcept {
    if (!initialized_) {
        return SACCADE_OK;
    }
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
        release_frame(&running_frame_);
        running_ticket_ = 0;
    }
    release_frame(&pending_frame_);
    initialized_ = false;
    return result;
}

} // namespace saccade::scheduler

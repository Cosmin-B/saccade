#include "application/scene_coordinator.hpp"

#include <limits>

namespace saccade::application {
namespace {

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

bool accessibility_valid(const SaccadeAccessibilityProviderDesc& provider) noexcept {
    return provider.struct_size >= sizeof(provider) && provider.api_version == SACCADE_API_VERSION && provider.context != nullptr &&
           provider.ops.request != nullptr && provider.ops.poll != nullptr && provider.ops.wait != nullptr &&
           provider.ops.collect != nullptr && provider.ops.cancel != nullptr && provider.ops.release != nullptr;
}

bool source_valid(SceneSource source) noexcept {
    return source >= SceneSource::pixel && source <= SceneSource::fused;
}

bool semantic_source(SceneSource source) noexcept {
    return source == SceneSource::semantic || source == SceneSource::fused;
}

bool visual_source(SceneSource source) noexcept {
    return source == SceneSource::pixel || source == SceneSource::fused;
}

bool filter_valid(const TargetFilterConfig& filter) noexcept {
    return filter.confidence_q16 != 0 && filter.text_confidence_q16 != 0 && filter.minimum_width_q8 != 0 && filter.minimum_height_q8 != 0 &&
           filter.order >= TargetOrderPolicy::balanced && filter.order <= TargetOrderPolicy::controls_first && filter.reserved[0] == 0 &&
           filter.reserved[1] == 0 && filter.reserved[2] == 0;
}

bool fusion_valid(const scene::FusionConfig& fusion) noexcept {
    return fusion.maximum_targets != 0 && fusion.maximum_targets <= SACCADE_TARGET_PACKET_MAX_TARGETS && fusion.iou_threshold_q16 != 0 &&
           fusion.containment_threshold_q16 != 0 && fusion.maximum_area_ratio_q8 >= 256 && fusion.reserved == 0;
}

bool packet_epochs_match(const scene::PacketView& left, const scene::PacketView& right) noexcept {
    return left.header->frame_id == right.header->frame_id && left.header->session_epoch == right.header->session_epoch &&
           left.header->transform_epoch == right.header->transform_epoch && left.header->topology_epoch == right.header->topology_epoch;
}

bool newer_than(uint64_t frame_id, uint64_t transform_epoch, uint64_t topology_epoch, uint64_t published_frame_id,
                uint64_t published_transform_epoch, uint64_t published_topology_epoch) noexcept {
    if (topology_epoch != published_topology_epoch) {
        return topology_epoch > published_topology_epoch;
    }
    if (transform_epoch != published_transform_epoch) {
        return transform_epoch > published_transform_epoch;
    }
    return frame_id >= published_frame_id;
}

bool point_in_rect(int32_t x, int32_t y, const geometry::RectQ8& rect) noexcept {
    return x >= rect.x && y >= rect.y && static_cast<int64_t>(x) < static_cast<int64_t>(rect.x) + rect.width &&
           static_cast<int64_t>(y) < static_cast<int64_t>(rect.y) + rect.height;
}

bool text_target(const SaccadeTargetRecord& target) noexcept {
    return target.role == SACCADE_TARGET_ROLE_TEXT || target.role == SACCADE_TARGET_ROLE_TEXT_FIELD;
}

uint32_t filter_targets(scene::MutableScenePacket destination, size_t* byte_size, const TargetFilterConfig& filter,
                        const geometry::RectQ8* scope) noexcept {
    auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(destination.data);
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(destination.data + header->targets_offset);
    const uint32_t original_count = header->target_count;
    uint8_t* const original_text = reinterpret_cast<uint8_t*>(targets + original_count);
    const size_t original_text_size =
        static_cast<size_t>(header->total_size) -
        (static_cast<size_t>(header->targets_offset) + static_cast<size_t>(original_count) * sizeof(SaccadeTargetRecord));
    uint32_t written = 0;
    for (uint32_t index = 0; index < header->target_count; ++index) {
        const SaccadeTargetRecord& target = targets[index];
        const uint16_t confidence = text_target(target) ? filter.text_confidence_q16 : filter.confidence_q16;
        if (target.confidence_q16 < confidence || target.width_q8 < filter.minimum_width_q8 ||
            target.height_q8 < filter.minimum_height_q8 || (scope != nullptr && !point_in_rect(target.safe_x_q8, target.safe_y_q8, *scope)))
            continue;
        if (written != index)
            targets[written] = targets[index];
        uint32_t priority = 0;
        if (filter.order != TargetOrderPolicy::balanced) {
            const bool text = text_target(targets[written]);
            priority = filter.order == TargetOrderPolicy::text_first ? static_cast<uint32_t>(!text) : static_cast<uint32_t>(text);
        }
        targets[written].order = priority * SACCADE_TARGET_PACKET_MAX_TARGETS + written;
        ++written;
    }
    uint8_t* const compacted_text = reinterpret_cast<uint8_t*>(targets + written);
    std::memmove(compacted_text, original_text, original_text_size);
    uint32_t text_size = 0;
    for (uint32_t index = 0; index < written; ++index) {
        SaccadeTargetTextRef& ref = targets[index].text;
        if (ref.size == 0)
            continue;
        std::memmove(compacted_text + text_size, compacted_text + ref.offset, ref.size);
        ref.offset = static_cast<uint16_t>(text_size);
        text_size += ref.size;
    }
    header->target_count = written;
    header->total_size = sizeof(SaccadeTargetPacketHeader) + static_cast<uint64_t>(written) * sizeof(SaccadeTargetRecord) + text_size;
    *byte_size = static_cast<size_t>(header->total_size);
    return written;
}

} // namespace

SceneCoordinator::~SceneCoordinator() {
    (void)shutdown();
}

SceneCoordinatorStatus SceneCoordinator::status() const noexcept {
    SceneCoordinatorStatus result = status_;
    result.source = source_;
    result.semantic_running = semantic_ticket_ != 0;
    result.semantic_available = semantic_.header != nullptr;
    return result;
}

void SceneCoordinator::record_publication(const SaccadeTargetPacketHeader& header) noexcept {
    status_.scene_epoch = header.scene_epoch;
    status_.frame_id = header.frame_id;
    status_.transform_epoch = header.transform_epoch;
    status_.topology_epoch = header.topology_epoch;
    status_.source_id = header.source_id;
    status_.target_count = header.target_count;
    status_.packet_flags = header.flags;
    stats_.incomplete_publications += (header.flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0 ? 1U : 0U;
    stats_.text_truncated_publications += (header.flags & SACCADE_TARGET_PACKET_TEXT_TRUNCATED) != 0 ? 1U : 0U;
}

SaccadeResult SceneCoordinator::initialize(const SceneCoordinatorConfig& config, SceneCoordinatorStorage* storage) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (storage == nullptr || config.neural_scenes == nullptr || config.output_scenes == nullptr ||
        config.neural_scenes == config.output_scenes || !accessibility_valid(config.accessibility) || config.first_scene_epoch == 0 ||
        !fusion_valid(config.fusion) || !source_valid(config.source) || !filter_valid(config.filter) ||
        ((config.semantic_freshness.context == nullptr) != (config.semantic_freshness.current == nullptr))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SaccadeResult tracker_initialized = visual_tracker_.initialize(&storage->visual_tracker, config.visual_tracker);
    if (tracker_initialized != SACCADE_OK) {
        return tracker_initialized;
    }
    config_ = config;
    storage_ = storage;
    next_scene_epoch_ = config.first_scene_epoch;
    source_ = config.source;
    status_.source = config.source;
    source_dirty_ = true;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::set_filter(TargetFilterConfig filter) noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (!filter_valid(filter))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    config_.filter = filter;
    source_dirty_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::set_fusion(scene::FusionConfig fusion) noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (!fusion_valid(fusion))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    config_.fusion = fusion;
    source_dirty_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::set_source(SceneSource source) noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (!source_valid(source))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (source_ == source)
        return SACCADE_OK;
    if (semantic_source(source_) && !semantic_source(source)) {
        const SaccadeResult cancelled = cancel_semantic();
        if (cancelled != SACCADE_OK)
            return cancelled;
    }
    if (visual_source(source_) != visual_source(source)) {
        visual_tracker_.reset();
        latest_visual_tracker_stats_ = {};
    }
    source_ = source;
    status_.source = source;
    source_dirty_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::set_scope(const geometry::RectQ8* scope) noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (scope != nullptr && !geometry::rect_valid(*scope))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const bool same = scope_enabled_ == (scope != nullptr) &&
                      (scope == nullptr ||
                       (scope_.x == scope->x && scope_.y == scope->y && scope_.width == scope->width && scope_.height == scope->height));
    if (same)
        return SACCADE_OK;
    const SaccadeResult cancelled = cancel_semantic();
    if (cancelled != SACCADE_OK)
        return cancelled;
    scope_enabled_ = scope != nullptr;
    scope_ = scope == nullptr ? geometry::RectQ8{} : *scope;
    source_dirty_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::publish_grid(scene::GridSceneConfig config, SceneCoordinatorAdvance* output) noexcept {
    if (!initialized_ || output == nullptr || source_ != SceneSource::grid)
        return SACCADE_ERROR_STATE;
    *output = {};
    if (next_scene_epoch_ == std::numeric_limits<uint64_t>::max())
        return SACCADE_ERROR_CAPACITY;
    scene::MutableScenePacket destination{};
    SaccadeResult result = config_.output_scenes->begin_write(&destination);
    if (result != SACCADE_OK)
        return result;
    config.scene_epoch = next_scene_epoch_;
    size_t byte_size = 0;
    result = scene::build_grid_scene(config, {destination.data, destination.capacity}, &byte_size);
    if (result != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        return result;
    }
    result = config_.output_scenes->commit_trusted(destination, byte_size);
    if (result != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        return result;
    }
    ++next_scene_epoch_;
    last_frame_id_ = config.frame_id;
    last_transform_epoch_ = config.transform_epoch;
    last_topology_epoch_ = config.topology_epoch;
    source_dirty_ = false;
    const auto& header = *reinterpret_cast<const SaccadeTargetPacketHeader*>(destination.data);
    record_publication(header);
    latest_fusion_stats_ = {};
    latest_fusion_stats_.packets_read = 1;
    latest_fusion_stats_.candidates_read = static_cast<uint64_t>(config.rows) * config.columns;
    latest_fusion_stats_.targets_written = static_cast<uint32_t>(config.rows) * config.columns;
    latest_fusion_input_count_ = 1;
    ++stats_.single_source_publications;
    stats_.targets_published += static_cast<uint64_t>(config.rows) * config.columns;
    output->scene_epoch = config.scene_epoch;
    output->frame_id = config.frame_id;
    output->target_count = static_cast<uint32_t>(config.rows) * config.columns;
    output->packet_flags = header.flags;
    output->scene_published = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::publish_windows(scene::WindowSceneConfig config, const SaccadeWindowInfo* windows, uint32_t count,
                                                SceneCoordinatorAdvance* output) noexcept {
    if (!initialized_ || output == nullptr || source_ != SceneSource::windows)
        return SACCADE_ERROR_STATE;
    *output = {};
    if (next_scene_epoch_ == std::numeric_limits<uint64_t>::max())
        return SACCADE_ERROR_CAPACITY;
    scene::MutableScenePacket destination{};
    SaccadeResult result = config_.output_scenes->begin_write(&destination);
    if (result != SACCADE_OK)
        return result;
    config.scene_epoch = next_scene_epoch_;
    size_t byte_size = 0;
    result = scene::build_window_scene(config, windows, count, {destination.data, destination.capacity}, &byte_size);
    if (result != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        return result;
    }
    result = config_.output_scenes->commit_trusted(destination, byte_size);
    if (result != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        return result;
    }
    ++next_scene_epoch_;
    last_frame_id_ = config.frame_id;
    last_transform_epoch_ = config.transform_epoch;
    last_topology_epoch_ = config.topology_epoch;
    source_dirty_ = false;
    const auto& header = *reinterpret_cast<const SaccadeTargetPacketHeader*>(destination.data);
    record_publication(header);
    latest_fusion_stats_ = {};
    latest_fusion_stats_.packets_read = 1;
    latest_fusion_stats_.candidates_read = count;
    latest_fusion_stats_.targets_written = count;
    latest_fusion_input_count_ = 1;
    ++stats_.single_source_publications;
    stats_.targets_published += count;
    output->scene_epoch = config.scene_epoch;
    output->frame_id = config.frame_id;
    output->target_count = count;
    output->packet_flags = header.flags;
    output->scene_published = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::request_semantic(const SaccadeAccessibilityQueryDesc& query) noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (semantic_ticket_ != 0)
        return SACCADE_ERROR_BUSY;
    SaccadeTicketHandle ticket = 0;
    const SaccadeResult requested = config_.accessibility.ops.request(config_.accessibility.context, &query, &ticket);
    if (requested != SACCADE_OK) {
        ++stats_.failures;
        return requested;
    }
    if (ticket == 0) {
        ++stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    semantic_ticket_ = ticket;
    semantic_query_ = query;
    ++stats_.semantic_requests;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::cancel_semantic() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;

    SaccadeResult result = SACCADE_OK;
    if (semantic_ticket_ != 0) {
        const SaccadeResult cancelled = config_.accessibility.ops.cancel(config_.accessibility.context, semantic_ticket_);
        if (cancelled != SACCADE_OK && cancelled != SACCADE_ERROR_STATE && cancelled != SACCADE_ERROR_STALE_HANDLE) {
            result = cancelled;
        }

        SaccadeAccessibilityStatus status = output_structure<SaccadeAccessibilityStatus>();
        const SaccadeResult waited = config_.accessibility.ops.wait(config_.accessibility.context, semantic_ticket_, UINT64_MAX, &status);
        if (result == SACCADE_OK && waited != SACCADE_OK && waited != SACCADE_ERROR_CANCELLED && waited != SACCADE_ERROR_STALE_HANDLE) {
            result = waited;
        }
        if (status.state == SACCADE_TICKET_CANCELLED)
            ++stats_.semantic_cancelled;
        if (status.snapshot != 0) {
            semantic_snapshot_ = status.snapshot;
            const SaccadeResult released = release_semantic_snapshot();
            if (result == SACCADE_OK && released != SACCADE_OK)
                result = released;
        }
    }

    clear_semantic();
    source_dirty_ = true;
    return result;
}

SaccadeResult SceneCoordinator::release_semantic_snapshot() noexcept {
    if (semantic_snapshot_ == 0)
        return SACCADE_OK;
    const SaccadeResult released = config_.accessibility.ops.release(config_.accessibility.context, semantic_snapshot_);
    if (released == SACCADE_OK)
        semantic_snapshot_ = 0;
    return released;
}

void SceneCoordinator::clear_semantic() noexcept {
    semantic_ = {};
    semantic_query_ = {};
    semantic_snapshot_ = 0;
    semantic_ticket_ = 0;
}

SaccadeResult SceneCoordinator::poll_semantic(bool* changed) noexcept {
    *changed = false;
    if (semantic_ticket_ == 0)
        return SACCADE_OK;
    SaccadeAccessibilityStatus status = output_structure<SaccadeAccessibilityStatus>();
    const SaccadeResult polled = config_.accessibility.ops.poll(config_.accessibility.context, semantic_ticket_, &status);
    if (polled != SACCADE_OK) {
        ++stats_.failures;
        return polled;
    }
    if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING) {
        return SACCADE_OK;
    }
    if (status.state == SACCADE_TICKET_CANCELLED) {
        ++stats_.semantic_cancelled;
        clear_semantic();
        return SACCADE_OK;
    }
    const bool status_matches_query = status.ticket == semantic_ticket_ && status.session_epoch == semantic_query_.session_epoch &&
                                      status.transform_epoch == semantic_query_.transform_epoch &&
                                      status.topology_epoch == semantic_query_.topology_epoch &&
                                      status.frame_id == semantic_query_.frame_id;
    const bool query_is_current = config_.semantic_freshness.current == nullptr ||
                                  config_.semantic_freshness.current(config_.semantic_freshness.context, semantic_query_);
    if (status.state == SACCADE_TICKET_COMPLETE && (!status_matches_query || !query_is_current)) {
        ++stats_.semantic_stale;
        if (status.snapshot != 0) {
            semantic_snapshot_ = status.snapshot;
            const SaccadeResult released = release_semantic_snapshot();
            if (released != SACCADE_OK) {
                ++stats_.failures;
                clear_semantic();
                return released;
            }
        }
        clear_semantic();
        return SACCADE_OK;
    }
    if (status.state == SACCADE_TICKET_FAILED) {
        ++stats_.semantic_failed;
        const SaccadeResult failure = status.result;
        clear_semantic();
        if (failure == SACCADE_ERROR_NOT_FOUND || failure == SACCADE_ERROR_STALE_HANDLE)
            return SACCADE_OK;
        ++stats_.failures;
        return failure == SACCADE_OK ? SACCADE_ERROR_BACKEND : failure;
    }
    if (status.state != SACCADE_TICKET_COMPLETE || status.result != SACCADE_OK || status.snapshot == 0 || status.required_bytes == 0 ||
        status.required_bytes > storage_->semantic_packet.size()) {
        ++stats_.semantic_failed;
        if (status.snapshot != 0) {
            semantic_snapshot_ = status.snapshot;
            const SaccadeResult released = release_semantic_snapshot();
            if (released != SACCADE_OK) {
                ++stats_.failures;
                return released;
            }
        }
        clear_semantic();
        return SACCADE_ERROR_BACKEND;
    }

    semantic_snapshot_ = status.snapshot;
    size_t byte_size = 0;
    const SaccadeResult collected =
        config_.accessibility.ops.collect(config_.accessibility.context, semantic_snapshot_,
                                          {storage_->semantic_packet.data(), storage_->semantic_packet.size()}, &byte_size);
    if (collected != SACCADE_OK) {
        ++stats_.failures;
        (void)release_semantic_snapshot();
        clear_semantic();
        return collected;
    }
    scene::PacketView packet{};
    const SaccadeResult validated = scene::validate_packet({storage_->semantic_packet.data(), byte_size}, &packet);
    if (validated != SACCADE_OK || packet.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        packet.header->frame_id != semantic_query_.frame_id || packet.header->session_epoch != semantic_query_.session_epoch ||
        packet.header->transform_epoch != semantic_query_.transform_epoch ||
        packet.header->topology_epoch != semantic_query_.topology_epoch || packet.header->source_id != semantic_query_.window_id) {
        ++stats_.semantic_failed;
        (void)release_semantic_snapshot();
        clear_semantic();
        return validated == SACCADE_OK ? SACCADE_ERROR_STALE_HANDLE : validated;
    }
    const SaccadeResult released = release_semantic_snapshot();
    if (released != SACCADE_OK) {
        ++stats_.failures;
        semantic_ = {};
        return released;
    }
    semantic_ticket_ = 0;
    semantic_ = packet;
    *changed = true;
    ++stats_.semantic_completed;
    stats_.semantic_incomplete += (packet.header->flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0 ? 1U : 0U;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::publish(bool neural_changed, bool semantic_changed, SceneCoordinatorAdvance* output) noexcept {
    const scene::PacketView* primary = nullptr;
    std::array<scene::PacketView, 2> inputs{};
    uint32_t input_count = 0;
    bool primary_changed = false;

    if (source_ == SceneSource::grid || source_ == SceneSource::windows)
        return SACCADE_OK;
    if (source_ == SceneSource::pixel && neural_.header != nullptr) {
        primary = &neural_;
        inputs[input_count++] = neural_;
        primary_changed = neural_changed || source_dirty_;
    } else if (source_ == SceneSource::semantic && semantic_.header != nullptr) {
        primary = &semantic_;
        inputs[input_count++] = semantic_;
        primary_changed = semantic_changed || source_dirty_;
    } else if (source_ == SceneSource::fused && neural_.header != nullptr && semantic_.header != nullptr &&
               packet_epochs_match(semantic_, neural_)) {
        primary = &semantic_;
        inputs[input_count++] = semantic_;
        inputs[input_count++] = neural_;
        primary_changed = neural_changed || semantic_changed || source_dirty_;
    }
    if (primary == nullptr && source_ == SceneSource::fused && semantic_changed && semantic_.header != nullptr && neural_.header != nullptr)
        ++stats_.semantic_stale;
    if (primary == nullptr || !primary_changed) {
        return SACCADE_OK;
    }

    const SaccadeTargetPacketHeader& header = *primary->header;
    if (!newer_than(header.frame_id, header.transform_epoch, header.topology_epoch, last_frame_id_, last_transform_epoch_,
                    last_topology_epoch_)) {
        if (semantic_changed)
            ++stats_.semantic_stale;
        return SACCADE_OK;
    }
    if (next_scene_epoch_ == std::numeric_limits<uint64_t>::max()) {
        ++stats_.failures;
        return SACCADE_ERROR_CAPACITY;
    }

    scene::MutableScenePacket destination{};
    const SaccadeResult begun = config_.output_scenes->begin_write(&destination);
    if (begun != SACCADE_OK) {
        ++stats_.failures;
        return begun;
    }
    scene::FusionEpochs epochs{};
    epochs.scene_epoch = next_scene_epoch_;
    epochs.frame_id = header.frame_id;
    epochs.capture_time_ns = header.capture_time_ns;
    if (neural_.header != nullptr && (primary == &neural_ || packet_epochs_match(*primary, neural_))) {
        epochs.capture_time_ns = neural_.header->capture_time_ns;
    }
    epochs.model_epoch = header.model_epoch;
    epochs.session_epoch = header.session_epoch;
    epochs.transform_epoch = header.transform_epoch;
    epochs.topology_epoch = header.topology_epoch;
    epochs.source_id = header.source_id;
    size_t byte_size = 0;
    scene::FusionStats fusion_stats{};
    const SaccadeResult fused = scene::fuse(inputs.data(), input_count, config_.fusion, epochs, &storage_->fusion,
                                            {destination.data, destination.capacity}, &byte_size, &fusion_stats);
    if (fused != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        ++stats_.failures;
        return fused;
    }
    fusion_stats.targets_written = filter_targets(destination, &byte_size, config_.filter, scope_enabled_ ? &scope_ : nullptr);
    latest_visual_tracker_stats_ = {};
    if (visual_source(source_)) {
        auto* mutable_header = reinterpret_cast<SaccadeTargetPacketHeader*>(destination.data);
        auto* mutable_targets = reinterpret_cast<SaccadeTargetRecord*>(destination.data + mutable_header->targets_offset);
        const SaccadeResult tracked = visual_tracker_.remap(mutable_header, mutable_targets, &latest_visual_tracker_stats_);
        if (tracked != SACCADE_OK) {
            (void)config_.output_scenes->abort_write(destination);
            ++stats_.failures;
            return tracked;
        }
    }
    const SaccadeResult committed = config_.output_scenes->commit_trusted(destination, byte_size);
    if (committed != SACCADE_OK) {
        (void)config_.output_scenes->abort_write(destination);
        ++stats_.failures;
        return committed;
    }
    ++next_scene_epoch_;
    last_frame_id_ = header.frame_id;
    last_transform_epoch_ = header.transform_epoch;
    last_topology_epoch_ = header.topology_epoch;
    source_dirty_ = false;
    const auto& published_header = *reinterpret_cast<const SaccadeTargetPacketHeader*>(destination.data);
    record_publication(published_header);
    latest_fusion_stats_ = fusion_stats;
    latest_fusion_input_count_ = input_count;
    if (input_count == 2)
        ++stats_.fused_publications;
    else
        ++stats_.single_source_publications;
    stats_.targets_published += fusion_stats.targets_written;
    output->scene_epoch = epochs.scene_epoch;
    output->frame_id = epochs.frame_id;
    output->target_count = fusion_stats.targets_written;
    output->packet_flags = published_header.flags;
    output->scene_published = true;
    return SACCADE_OK;
}

SaccadeResult SceneCoordinator::advance(SceneCoordinatorAdvance* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    ++stats_.advances;
    bool semantic_changed = false;
    SaccadeResult result = poll_semantic(&semantic_changed);
    output->semantic_collected = semantic_changed;

    bool neural_changed = false;
    scene::PacketView latest{};
    const SaccadeResult acquired = config_.neural_scenes->acquire_latest(&latest);
    if (acquired == SACCADE_OK && latest.header->scene_epoch != neural_scene_epoch_) {
        neural_ = latest;
        neural_scene_epoch_ = latest.header->scene_epoch;
        neural_changed = true;
        ++stats_.neural_updates;
    } else if (acquired != SACCADE_OK && acquired != SACCADE_ERROR_NOT_FOUND && result == SACCADE_OK) {
        result = acquired;
        ++stats_.failures;
    }
    const SaccadeResult published = publish(neural_changed, semantic_changed, output);
    if (published != SACCADE_OK && result == SACCADE_OK)
        result = published;
    return result;
}

SaccadeResult SceneCoordinator::shutdown() noexcept {
    if (!initialized_)
        return SACCADE_OK;
    const SaccadeResult result = cancel_semantic();
    config_ = {};
    storage_ = nullptr;
    neural_ = {};
    semantic_ = {};
    semantic_query_ = {};
    semantic_ticket_ = 0;
    semantic_snapshot_ = 0;
    neural_scene_epoch_ = 0;
    visual_tracker_.shutdown();
    latest_visual_tracker_stats_ = {};
    source_ = SceneSource::pixel;
    scope_ = {};
    scope_enabled_ = false;
    source_dirty_ = false;
    initialized_ = false;
    return result;
}

} // namespace saccade::application

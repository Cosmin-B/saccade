#include "scene/visual_target_tracker.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

namespace saccade::scene {
namespace {

constexpr uint16_t visual_source_mask = SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_PIXEL;
constexpr uint64_t visual_target_id_prefix = UINT64_C(0x5654524B00000000);
constexpr uint64_t visual_target_id_sequence_mask = UINT64_C(0x00000000FFFFFFFF);
constexpr uint32_t unit_q16 = 65536;

uint8_t role_group(SaccadeTargetRole role) noexcept {
    switch (role) {
    case SACCADE_TARGET_ROLE_BUTTON:
    case SACCADE_TARGET_ROLE_LINK:
    case SACCADE_TARGET_ROLE_CHECKBOX:
    case SACCADE_TARGET_ROLE_RADIO:
    case SACCADE_TARGET_ROLE_MENU_ITEM:
        return 1;
    case SACCADE_TARGET_ROLE_TEXT:
    case SACCADE_TARGET_ROLE_TEXT_FIELD:
        return 2;
    case SACCADE_TARGET_ROLE_SLIDER:
        return 3;
    case SACCADE_TARGET_ROLE_IMAGE:
        return 4;
    case SACCADE_TARGET_ROLE_WINDOW:
        return 5;
    default:
        return 0;
    }
}

bool visual_target(const SaccadeTargetRecord& target) noexcept {
    return (target.source_bits & visual_source_mask) != 0 &&
           (target.source_bits & (SACCADE_TARGET_SOURCE_ACCESSIBILITY | SACCADE_TARGET_SOURCE_GRID)) == 0;
}

bool compatible_role(SaccadeTargetRole left, SaccadeTargetRole right) noexcept {
    const uint8_t left_group = role_group(left);
    const uint8_t right_group = role_group(right);
    return left_group == 0 || right_group == 0 || left_group == right_group;
}

bool compatible_owner(const VisualTargetTrackerTrack& track, const SaccadeTargetRecord& target) noexcept {
    return track.target.window_id == target.window_id &&
           (track.target.display_id == 0 || target.display_id == 0 || track.target.display_id == target.display_id);
}

bool compatible_source(const VisualTargetTrackerTrack& track, const SaccadeTargetRecord& target) noexcept {
    return (track.target.source_bits & target.source_bits & visual_source_mask) != 0;
}

uint64_t absolute_u64(int64_t value) noexcept {
    return value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1U : static_cast<uint64_t>(value);
}

uint64_t saturated_square(uint64_t value) noexcept {
    constexpr uint64_t maximum_root = UINT64_C(4294967295);
    return value > maximum_root ? UINT64_MAX : value * value;
}

uint64_t saturated_add(uint64_t left, uint64_t right) noexcept {
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

uint32_t scaled_ratio(uint64_t numerator, uint64_t denominator, uint32_t scale) noexcept {
    if (numerator == 0 || denominator == 0) {
        return 0;
    }

    const uint64_t quotient = numerator / denominator;
    if (quotient > UINT32_MAX / scale) {
        return UINT32_MAX;
    }

    uint64_t remainder = numerator % denominator;
    uint64_t reduced_denominator = denominator;
    const uint32_t bits = static_cast<uint32_t>(std::bit_width(remainder));
    if (bits > 47U) {
        const uint32_t shift = bits - 47U;
        remainder >>= shift;
        reduced_denominator >>= shift;
    }
    if (reduced_denominator == 0) {
        return UINT32_MAX;
    }

    const uint64_t fraction = remainder * scale / reduced_denominator;
    const uint64_t result = quotient * scale + fraction;
    return result > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(result);
}

uint64_t rectangle_area(const SaccadeTargetRecord& target) noexcept {
    return static_cast<uint64_t>(target.width_q8) * static_cast<uint64_t>(target.height_q8);
}

uint16_t rectangle_iou_q16(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    const int64_t intersection_left = std::max<int64_t>(left.x_q8, right.x_q8);
    const int64_t intersection_top = std::max<int64_t>(left.y_q8, right.y_q8);
    const int64_t intersection_right = std::min<int64_t>(static_cast<int64_t>(left.x_q8) + left.width_q8,
                                                         static_cast<int64_t>(right.x_q8) + right.width_q8);
    const int64_t intersection_bottom = std::min<int64_t>(static_cast<int64_t>(left.y_q8) + left.height_q8,
                                                          static_cast<int64_t>(right.y_q8) + right.height_q8);
    if (intersection_left >= intersection_right || intersection_top >= intersection_bottom) {
        return 0;
    }

    const uint64_t intersection = static_cast<uint64_t>(intersection_right - intersection_left) *
                                  static_cast<uint64_t>(intersection_bottom - intersection_top);
    const uint64_t combined_area = rectangle_area(left) + rectangle_area(right) - intersection;
    return static_cast<uint16_t>(std::min<uint32_t>(scaled_ratio(intersection, combined_area, UINT16_MAX), UINT16_MAX));
}

uint32_t center_distance_squared_q16(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    const int64_t left_x = static_cast<int64_t>(left.x_q8) * 2 + left.width_q8;
    const int64_t left_y = static_cast<int64_t>(left.y_q8) * 2 + left.height_q8;
    const int64_t right_x = static_cast<int64_t>(right.x_q8) * 2 + right.width_q8;
    const int64_t right_y = static_cast<int64_t>(right.y_q8) * 2 + right.height_q8;
    const uint64_t dx = absolute_u64(left_x - right_x);
    const uint64_t dy = absolute_u64(left_y - right_y);
    const uint64_t numerator = saturated_add(saturated_square(dx), saturated_square(dy));
    const uint64_t scale =
        static_cast<uint64_t>(std::max({left.width_q8, left.height_q8, right.width_q8, right.height_q8}));
    const uint64_t denominator = saturated_square(scale * 2U);
    return scaled_ratio(numerator, denominator, unit_q16);
}

bool size_ratio_allowed(int32_t left, int32_t right, uint16_t maximum_ratio_q8) noexcept {
    const uint32_t smaller = static_cast<uint32_t>(std::min(left, right));
    const uint32_t larger = static_cast<uint32_t>(std::max(left, right));
    return smaller != 0 && scaled_ratio(larger, smaller, 256) <= maximum_ratio_q8;
}

int32_t saturated_i32(int64_t value) noexcept {
    return static_cast<int32_t>(
        std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

SaccadeTargetRecord predicted_target(const VisualTargetTrackerTrack& track) noexcept {
    SaccadeTargetRecord predicted = track.target;
    const int64_t steps = static_cast<int64_t>(track.missed_frames) + 1;
    predicted.x_q8 = saturated_i32(static_cast<int64_t>(predicted.x_q8) + track.velocity_x_q8 * steps);
    predicted.y_q8 = saturated_i32(static_cast<int64_t>(predicted.y_q8) + track.velocity_y_q8 * steps);
    predicted.width_q8 =
        std::max(1, saturated_i32(static_cast<int64_t>(predicted.width_q8) + track.velocity_width_q8 * steps));
    predicted.height_q8 =
        std::max(1, saturated_i32(static_cast<int64_t>(predicted.height_q8) + track.velocity_height_q8 * steps));
    predicted.safe_x_q8 = predicted.x_q8 + predicted.width_q8 / 2;
    predicted.safe_y_q8 = predicted.y_q8 + predicted.height_q8 / 2;
    return predicted;
}

bool same_window_geometry(const VisualTargetTrackerWindow& left, const VisualTargetTrackerWindow& right) noexcept {
    return left.window_id == right.window_id && left.display_id == right.display_id && left.x_q8 == right.x_q8 &&
           left.y_q8 == right.y_q8 && left.width_q8 == right.width_q8 && left.height_q8 == right.height_q8;
}

int32_t normalized_q16(int64_t value, int64_t extent) noexcept {
    return saturated_i32(value * unit_q16 / extent);
}

void relative_geometry(const SaccadeTargetRecord& target, const VisualTargetTrackerWindow& window,
                       int32_t* center_x_q16, int32_t* center_y_q16, int32_t* width_q16, int32_t* height_q16) noexcept {
    const int64_t center_x = static_cast<int64_t>(target.x_q8) * 2 + target.width_q8;
    const int64_t center_y = static_cast<int64_t>(target.y_q8) * 2 + target.height_q8;
    const int64_t window_center_x = static_cast<int64_t>(window.x_q8) * 2;
    const int64_t window_center_y = static_cast<int64_t>(window.y_q8) * 2;
    *center_x_q16 = normalized_q16(center_x - window_center_x, static_cast<int64_t>(window.width_q8) * 2);
    *center_y_q16 = normalized_q16(center_y - window_center_y, static_cast<int64_t>(window.height_q8) * 2);
    *width_q16 = normalized_q16(target.width_q8, window.width_q8);
    *height_q16 = normalized_q16(target.height_q8, window.height_q8);
}

uint32_t relative_distance_squared_q16(const VisualTargetTrackerTrack& track, const SaccadeTargetRecord& target,
                                       const VisualTargetTrackerWindow& window,
                                       uint32_t* size_difference_q16) noexcept {
    int32_t center_x_q16 = 0;
    int32_t center_y_q16 = 0;
    int32_t width_q16 = 0;
    int32_t height_q16 = 0;
    relative_geometry(target, window, &center_x_q16, &center_y_q16, &width_q16, &height_q16);

    const uint64_t dx = absolute_u64(static_cast<int64_t>(center_x_q16) - track.relative_center_x_q16);
    const uint64_t dy = absolute_u64(static_cast<int64_t>(center_y_q16) - track.relative_center_y_q16);
    const uint64_t distance_q32 = saturated_add(saturated_square(dx), saturated_square(dy));
    *size_difference_q16 = static_cast<uint32_t>(
        std::min<uint64_t>(absolute_u64(static_cast<int64_t>(width_q16) - track.relative_width_q16) +
                               absolute_u64(static_cast<int64_t>(height_q16) - track.relative_height_q16),
                           UINT32_MAX));
    return static_cast<uint32_t>(std::min<uint64_t>(distance_q32 >> 16U, UINT32_MAX));
}

bool config_valid(const VisualTargetTrackerConfig& config) noexcept {
    return config.maximum_tracks != 0 && config.maximum_tracks <= visual_target_tracker_maximum_tracks &&
           config.maximum_missed_frames != 0 && config.minimum_predicted_iou_q16 != 0 &&
           config.maximum_center_distance_q8 != 0 && config.maximum_window_center_distance_q16 != 0 &&
           config.maximum_size_ratio_q8 >= 256 && config.reserved[0] == 0 && config.reserved[1] == 0 &&
           config.reserved[2] == 0;
}

const VisualTargetTrackerWindow* find_window(const VisualTargetTrackerStorage& storage, uint32_t window_count,
                                             uint64_t window_id) noexcept {
    if (window_id == 0) {
        return nullptr;
    }
    for (uint32_t index = 0; index < window_count; ++index) {
        if (storage.windows[index].window_id == window_id) {
            return &storage.windows[index];
        }
    }
    return nullptr;
}

uint32_t collect_windows(VisualTargetTrackerStorage* storage, const SaccadeTargetPacketHeader& header,
                         const SaccadeTargetRecord* targets) noexcept {
    uint32_t count = 0;
    for (uint32_t index = 0; index < header.target_count && count < visual_target_tracker_maximum_windows; ++index) {
        const SaccadeTargetRecord& target = targets[index];
        if (target.role != SACCADE_TARGET_ROLE_WINDOW || target.width_q8 <= 0 || target.height_q8 <= 0) {
            continue;
        }

        const uint64_t window_id = target.window_id != 0 ? target.window_id : target.target_id;
        if (find_window(*storage, count, window_id) != nullptr) {
            continue;
        }
        storage->windows[count++] = {window_id,   target.display_id, target.x_q8,
                                     target.y_q8, target.width_q8,   target.height_q8};
    }
    return count;
}

bool edge_less(const VisualTargetTrackerEdge& left, const VisualTargetTrackerEdge& right) noexcept {
    if (left.score != right.score) {
        return left.score < right.score;
    }
    if (left.target_slot != right.target_slot) {
        return left.target_slot < right.target_slot;
    }
    return left.track_slot < right.track_slot;
}

bool association_edge(const VisualTargetTrackerTrack& track, const SaccadeTargetRecord& target,
                      const VisualTargetTrackerWindow* window, const VisualTargetTrackerConfig& config,
                      uint16_t target_slot, uint16_t track_slot, VisualTargetTrackerEdge* output) noexcept {
    if (!compatible_owner(track, target) || !compatible_role(track.target.role, target.role) ||
        !compatible_source(track, target) ||
        !size_ratio_allowed(track.target.width_q8, target.width_q8, config.maximum_size_ratio_q8) ||
        !size_ratio_allowed(track.target.height_q8, target.height_q8, config.maximum_size_ratio_q8)) {
        return false;
    }

    const SaccadeTargetRecord predicted = predicted_target(track);
    const uint16_t iou_q16 = rectangle_iou_q16(predicted, target);
    const uint32_t center_distance_q16 = center_distance_squared_q16(predicted, target);
    const uint32_t center_limit_q16 =
        static_cast<uint32_t>(config.maximum_center_distance_q8) * config.maximum_center_distance_q8;

    bool use_relative = false;
    uint32_t relative_distance_q16 = UINT32_MAX;
    uint32_t relative_size_difference_q16 = UINT32_MAX;
    if (config.use_window_relative_geometry && track.has_relative_geometry && window != nullptr &&
        !same_window_geometry(track.window, *window)) {
        relative_distance_q16 = relative_distance_squared_q16(track, target, *window, &relative_size_difference_q16);
        use_relative = true;
    }

    const uint32_t relative_limit_q16 = static_cast<uint32_t>(
        static_cast<uint64_t>(config.maximum_window_center_distance_q16) * config.maximum_window_center_distance_q16 >>
        16U);
    if (iou_q16 < config.minimum_predicted_iou_q16 && center_distance_q16 > center_limit_q16 &&
        (!use_relative || relative_distance_q16 > relative_limit_q16)) {
        return false;
    }

    uint64_t score = 0;
    if (use_relative && relative_distance_q16 <= relative_limit_q16) {
        score = static_cast<uint64_t>(relative_distance_q16) * 32U +
                static_cast<uint64_t>(relative_size_difference_q16) * 4U;
    } else {
        score =
            static_cast<uint64_t>(UINT16_MAX - iou_q16) * 8U + std::min<uint32_t>(center_distance_q16, unit_q16 * 16U);
    }
    if (track.target.role != target.role) {
        score += 8192U;
    }
    if ((track.target.source_bits & visual_source_mask) != (target.source_bits & visual_source_mask)) {
        score += 4096U;
    }

    output->score = static_cast<uint32_t>(std::min<uint64_t>(score, UINT32_MAX));
    output->target_slot = target_slot;
    output->track_slot = track_slot;
    return true;
}

void insert_best_edge(std::array<VisualTargetTrackerEdge, visual_target_tracker_edges_per_target>* edges,
                      uint32_t* count, const VisualTargetTrackerEdge& edge) noexcept {
    uint32_t position = *count;
    if (position == visual_target_tracker_edges_per_target) {
        if (!edge_less(edge, (*edges)[position - 1U])) {
            return;
        }
        --position;
    } else {
        ++*count;
    }

    while (position != 0 && edge_less(edge, (*edges)[position - 1U])) {
        (*edges)[position] = (*edges)[position - 1U];
        --position;
    }
    (*edges)[position] = edge;
}

void update_relative_geometry(VisualTargetTrackerTrack* track, const VisualTargetTrackerWindow* window) noexcept {
    track->window = window == nullptr ? VisualTargetTrackerWindow{} : *window;
    track->has_relative_geometry = window != nullptr;
    if (window == nullptr) {
        track->relative_center_x_q16 = 0;
        track->relative_center_y_q16 = 0;
        track->relative_width_q16 = 0;
        track->relative_height_q16 = 0;
        return;
    }
    relative_geometry(track->target, *window, &track->relative_center_x_q16, &track->relative_center_y_q16,
                      &track->relative_width_q16, &track->relative_height_q16);
}

void update_track(VisualTargetTrackerTrack* track, const SaccadeTargetRecord& target,
                  const VisualTargetTrackerWindow* window, uint64_t frame_id) noexcept {
    if (frame_id > track->last_frame_id) {
        const int32_t steps = static_cast<int32_t>(track->missed_frames + 1U);
        track->velocity_x_q8 = saturated_i32((static_cast<int64_t>(target.x_q8) - track->target.x_q8) / steps);
        track->velocity_y_q8 = saturated_i32((static_cast<int64_t>(target.y_q8) - track->target.y_q8) / steps);
        track->velocity_width_q8 =
            saturated_i32((static_cast<int64_t>(target.width_q8) - track->target.width_q8) / steps);
        track->velocity_height_q8 =
            saturated_i32((static_cast<int64_t>(target.height_q8) - track->target.height_q8) / steps);
        track->missed_frames = 0;
        ++track->age;
    }
    track->target = target;
    track->last_frame_id = frame_id;
    update_relative_geometry(track, window);
}

uint32_t find_available_track(VisualTargetTrackerStorage* storage, uint32_t maximum_tracks) noexcept {
    for (uint32_t index = 0; index < maximum_tracks; ++index) {
        if (!storage->tracks[index].active) {
            return index;
        }
    }

    uint32_t selected = UINT32_MAX;
    for (uint32_t index = 0; index < maximum_tracks; ++index) {
        if (storage->track_matched[index] != 0) {
            continue;
        }
        if (selected == UINT32_MAX || storage->tracks[index].missed_frames > storage->tracks[selected].missed_frames ||
            (storage->tracks[index].missed_frames == storage->tracks[selected].missed_frames &&
             storage->tracks[index].last_frame_id < storage->tracks[selected].last_frame_id)) {
            selected = index;
        }
    }
    return selected;
}

uint64_t next_target_id(uint32_t* sequence) noexcept {
    uint32_t value = (*sequence)++;
    if (value == 0) {
        value = (*sequence)++;
    }
    return visual_target_id_prefix | (static_cast<uint64_t>(value) & visual_target_id_sequence_mask);
}

} // namespace

SaccadeResult VisualTargetTracker::initialize(VisualTargetTrackerStorage* storage,
                                              const VisualTargetTrackerConfig& config) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    if (storage == nullptr || !config_valid(config)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    storage_ = storage;
    config_ = config;
    initialized_ = true;
    reset();
    return SACCADE_OK;
}

void VisualTargetTracker::reset() noexcept {
    if (!initialized_) {
        return;
    }
    storage_->tracks.fill({});
    storage_->target_matched.fill(0);
    storage_->track_matched.fill(0);
    model_epoch_ = 0;
    session_epoch_ = 0;
    topology_epoch_ = 0;
    last_scene_epoch_ = 0;
    last_frame_id_ = 0;
    next_sequence_ = 1;
}

void VisualTargetTracker::shutdown() noexcept {
    if (!initialized_) {
        return;
    }
    reset();
    storage_ = nullptr;
    config_ = {};
    initialized_ = false;
}

SaccadeResult VisualTargetTracker::remap(SaccadeTargetPacketHeader* header, SaccadeTargetRecord* targets,
                                         VisualTargetTrackerStats* stats) noexcept {
    if (!initialized_ || header == nullptr || targets == nullptr || stats == nullptr ||
        header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        header->target_count > SACCADE_TARGET_PACKET_MAX_TARGETS || header->scene_epoch == 0 || header->frame_id == 0 ||
        header->session_epoch == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (session_epoch_ == header->session_epoch && model_epoch_ == header->model_epoch &&
        topology_epoch_ == header->topology_epoch && last_scene_epoch_ != 0 &&
        (header->scene_epoch <= last_scene_epoch_ || header->frame_id < last_frame_id_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (session_epoch_ != 0 && (session_epoch_ != header->session_epoch || model_epoch_ != header->model_epoch ||
                                topology_epoch_ != header->topology_epoch)) {
        reset();
    }

    *stats = {};
    stats->frame_id = header->frame_id;
    model_epoch_ = header->model_epoch;
    session_epoch_ = header->session_epoch;
    topology_epoch_ = header->topology_epoch;
    const bool frame_advanced = last_frame_id_ == 0 || header->frame_id > last_frame_id_;
    const uint32_t window_count = collect_windows(storage_, *header, targets);

    uint32_t target_count = 0;
    for (uint32_t index = 0; index < header->target_count; ++index) {
        if (!visual_target(targets[index])) {
            ++stats->passthrough_targets;
            continue;
        }
        ++stats->visual_targets;
        if (target_count == config_.maximum_tracks) {
            ++stats->capacity_drops;
            continue;
        }
        storage_->target_indices[target_count++] = static_cast<uint16_t>(index);
    }

    storage_->target_matched.fill(0);
    storage_->track_matched.fill(0);

    uint32_t edge_count = 0;
    for (uint32_t target_slot = 0; target_slot < target_count; ++target_slot) {
        const SaccadeTargetRecord& target = targets[storage_->target_indices[target_slot]];
        const VisualTargetTrackerWindow* window = find_window(*storage_, window_count, target.window_id);
        std::array<VisualTargetTrackerEdge, visual_target_tracker_edges_per_target> best_edges{};
        uint32_t best_edge_count = 0;
        for (uint32_t track_slot = 0; track_slot < config_.maximum_tracks; ++track_slot) {
            const VisualTargetTrackerTrack& track = storage_->tracks[track_slot];
            if (!track.active) {
                continue;
            }
            ++stats->association_tests;
            VisualTargetTrackerEdge edge{};
            if (association_edge(track, target, window, config_, static_cast<uint16_t>(target_slot),
                                 static_cast<uint16_t>(track_slot), &edge)) {
                insert_best_edge(&best_edges, &best_edge_count, edge);
            }
        }
        for (uint32_t index = 0; index < best_edge_count; ++index) {
            storage_->edges[edge_count++] = best_edges[index];
        }
    }

    std::sort(storage_->edges.begin(), storage_->edges.begin() + edge_count, edge_less);
    stats->association_edges = edge_count;
    for (uint32_t index = 0; index < edge_count; ++index) {
        const VisualTargetTrackerEdge& edge = storage_->edges[index];
        if (storage_->target_matched[edge.target_slot] != 0 || storage_->track_matched[edge.track_slot] != 0) {
            continue;
        }

        const uint16_t target_index = storage_->target_indices[edge.target_slot];
        SaccadeTargetRecord& target = targets[target_index];
        VisualTargetTrackerTrack& track = storage_->tracks[edge.track_slot];
        target.target_id = track.target.target_id;
        update_track(&track, target, find_window(*storage_, window_count, target.window_id), header->frame_id);
        storage_->target_matched[edge.target_slot] = 1;
        storage_->track_matched[edge.track_slot] = 1;
        ++stats->matched_targets;
    }

    for (uint32_t track_slot = 0; track_slot < config_.maximum_tracks; ++track_slot) {
        VisualTargetTrackerTrack& track = storage_->tracks[track_slot];
        if (!track.active || storage_->track_matched[track_slot] != 0) {
            continue;
        }
        track.missed_frames += frame_advanced ? 1U : 0U;
        if (track.missed_frames > config_.maximum_missed_frames) {
            track = {};
            ++stats->retired_tracks;
        }
    }

    for (uint32_t target_slot = 0; target_slot < target_count; ++target_slot) {
        if (storage_->target_matched[target_slot] != 0) {
            continue;
        }
        ++stats->unmatched_targets;
        const uint32_t track_slot = find_available_track(storage_, config_.maximum_tracks);
        if (track_slot == UINT32_MAX) {
            ++stats->capacity_drops;
            continue;
        }

        VisualTargetTrackerTrack& track = storage_->tracks[track_slot];
        if (track.active) {
            ++stats->retired_tracks;
        }
        SaccadeTargetRecord& target = targets[storage_->target_indices[target_slot]];
        target.target_id = next_target_id(&next_sequence_);
        track = {};
        track.target = target;
        track.last_frame_id = header->frame_id;
        track.age = 1;
        track.active = true;
        update_relative_geometry(&track, find_window(*storage_, window_count, target.window_id));
        storage_->track_matched[track_slot] = 1;
        ++stats->created_tracks;
    }

    for (uint32_t index = 0; index < config_.maximum_tracks; ++index) {
        stats->active_tracks += storage_->tracks[index].active ? 1U : 0U;
    }
    last_scene_epoch_ = header->scene_epoch;
    last_frame_id_ = header->frame_id;
    return SACCADE_OK;
}

} // namespace saccade::scene

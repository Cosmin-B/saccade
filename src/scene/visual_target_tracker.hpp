#ifndef SACCADE_SCENE_VISUAL_TARGET_TRACKER_HPP
#define SACCADE_SCENE_VISUAL_TARGET_TRACKER_HPP

#include "scene/packet.hpp"

#include <array>
#include <cstdint>

namespace saccade::scene {

// The visual hot set is intentionally bounded. Packets may contain more
// targets, but only the first 512 eligible records retain history.
constexpr uint32_t visual_target_tracker_maximum_tracks = 512;
constexpr uint32_t visual_target_tracker_edges_per_target = 8;
constexpr uint32_t visual_target_tracker_maximum_edges =
    visual_target_tracker_maximum_tracks * visual_target_tracker_edges_per_target;
constexpr uint32_t visual_target_tracker_maximum_windows = 64;

struct VisualTargetTrackerConfig {
    uint32_t maximum_tracks = visual_target_tracker_maximum_tracks;
    uint32_t maximum_missed_frames = 3;
    uint16_t minimum_predicted_iou_q16 = 4096;
    uint16_t maximum_center_distance_q8 = 768;
    uint16_t maximum_window_center_distance_q16 = 13107;
    uint16_t maximum_size_ratio_q8 = 768;
    bool use_window_relative_geometry = true;
    uint8_t reserved[3]{};
};

struct VisualTargetTrackerWindow {
    uint64_t window_id = 0;
    uint64_t display_id = 0;
    int32_t x_q8 = 0;
    int32_t y_q8 = 0;
    int32_t width_q8 = 0;
    int32_t height_q8 = 0;
};

struct VisualTargetTrackerTrack {
    SaccadeTargetRecord target{};
    VisualTargetTrackerWindow window{};
    uint64_t last_frame_id = 0;
    int32_t velocity_x_q8 = 0;
    int32_t velocity_y_q8 = 0;
    int32_t velocity_width_q8 = 0;
    int32_t velocity_height_q8 = 0;
    int32_t relative_center_x_q16 = 0;
    int32_t relative_center_y_q16 = 0;
    int32_t relative_width_q16 = 0;
    int32_t relative_height_q16 = 0;
    uint32_t age = 0;
    uint32_t missed_frames = 0;
    bool active = false;
    bool has_relative_geometry = false;
    uint8_t reserved[6]{};
};

struct VisualTargetTrackerEdge {
    uint32_t score = 0;
    uint16_t target_slot = 0;
    uint16_t track_slot = 0;
};

struct VisualTargetTrackerStorage {
    std::array<VisualTargetTrackerTrack, visual_target_tracker_maximum_tracks> tracks{};
    std::array<VisualTargetTrackerEdge, visual_target_tracker_maximum_edges> edges{};
    std::array<uint16_t, visual_target_tracker_maximum_tracks> target_indices{};
    std::array<uint8_t, visual_target_tracker_maximum_tracks> target_matched{};
    std::array<uint8_t, visual_target_tracker_maximum_tracks> track_matched{};
    std::array<VisualTargetTrackerWindow, visual_target_tracker_maximum_windows> windows{};
};

struct VisualTargetTrackerStats {
    uint64_t frame_id = 0;
    uint64_t association_tests = 0;
    uint64_t reserved_u64 = 0;
    uint32_t visual_targets = 0;
    uint32_t association_edges = 0;
    uint32_t matched_targets = 0;
    uint32_t created_tracks = 0;
    uint32_t retired_tracks = 0;
    uint32_t active_tracks = 0;
    uint32_t passthrough_targets = 0;
    uint32_t capacity_drops = 0;
    uint32_t unmatched_targets = 0;
    uint32_t reserved = 0;
};

class VisualTargetTracker final {
  public:
    SaccadeResult initialize(VisualTargetTrackerStorage*, const VisualTargetTrackerConfig& = {}) noexcept;
    // The caller supplies an already validated mutable packet. Only pure
    // neural or pixel target_id fields may be remapped before publication.
    SaccadeResult remap(SaccadeTargetPacketHeader*, SaccadeTargetRecord*, VisualTargetTrackerStats*) noexcept;
    void reset() noexcept;
    void shutdown() noexcept;

  private:
    VisualTargetTrackerStorage* storage_ = nullptr;
    VisualTargetTrackerConfig config_{};
    uint64_t model_epoch_ = 0;
    uint64_t session_epoch_ = 0;
    uint64_t topology_epoch_ = 0;
    uint64_t last_scene_epoch_ = 0;
    uint64_t last_frame_id_ = 0;
    uint32_t next_sequence_ = 1;
    bool initialized_ = false;
};

static_assert(sizeof(VisualTargetTrackerEdge) == 8);
static_assert(sizeof(VisualTargetTrackerStats) == 64);
static_assert(SACCADE_TARGET_PACKET_MAX_TARGETS <= UINT16_MAX);

} // namespace saccade::scene

#endif

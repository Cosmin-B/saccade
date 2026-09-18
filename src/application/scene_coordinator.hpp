#ifndef SACCADE_APPLICATION_SCENE_COORDINATOR_HPP
#define SACCADE_APPLICATION_SCENE_COORDINATOR_HPP

#include "scene/fusion.hpp"
#include "scene/grid.hpp"
#include "scene/store.hpp"
#include "scene/visual_target_tracker.hpp"
#include "scene/windows.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::application {

enum class SceneSource : uint32_t { pixel = 0, semantic = 1, grid = 2, windows = 3, fused = 4 };

using SemanticQueryCurrentFn = bool (*)(void*, const SaccadeAccessibilityQueryDesc&) noexcept;

struct SemanticFreshness {
    void* context = nullptr;
    SemanticQueryCurrentFn current = nullptr;
};

enum class TargetOrderPolicy : uint8_t { balanced = 0, text_first = 1, controls_first = 2 };

struct TargetFilterConfig {
    uint16_t confidence_q16 = 1;
    uint16_t text_confidence_q16 = 1;
    uint16_t minimum_width_q8 = 1;
    uint16_t minimum_height_q8 = 1;
    TargetOrderPolicy order = TargetOrderPolicy::balanced;
    uint8_t reserved[3]{};
};

struct SceneCoordinatorStorage {
    alignas(64) std::array<uint8_t, scene::target_packet_max_bytes> semantic_packet{};
    scene::FusionWorkspace fusion{};
    scene::VisualTargetTrackerStorage visual_tracker{};
};

struct SceneCoordinatorConfig {
    scene::SceneStore* neural_scenes = nullptr;
    scene::SceneStore* output_scenes = nullptr;
    SaccadeAccessibilityProviderDesc accessibility{};
    scene::FusionConfig fusion{};
    scene::VisualTargetTrackerConfig visual_tracker{};
    TargetFilterConfig filter{};
    SemanticFreshness semantic_freshness{};
    SceneSource source = SceneSource::pixel;
    uint64_t first_scene_epoch = 1;
};

struct SceneCoordinatorAdvance {
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint32_t target_count = 0;
    uint32_t packet_flags = 0;
    bool scene_published = false;
    bool semantic_collected = false;
    uint8_t reserved[6]{};
};

struct SceneCoordinatorStatus {
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
    uint32_t target_count = 0;
    uint32_t packet_flags = 0;
    SceneSource source = SceneSource::pixel;
    bool semantic_running = false;
    bool semantic_available = false;
    uint8_t reserved[2]{};
};

struct SceneCoordinatorStats {
    uint64_t advances = 0;
    uint64_t semantic_requests = 0;
    uint64_t semantic_completed = 0;
    uint64_t semantic_cancelled = 0;
    uint64_t semantic_failed = 0;
    uint64_t semantic_stale = 0;
    uint64_t neural_updates = 0;
    uint64_t fused_publications = 0;
    uint64_t single_source_publications = 0;
    uint64_t targets_published = 0;
    uint64_t semantic_incomplete = 0;
    uint64_t incomplete_publications = 0;
    uint64_t text_truncated_publications = 0;
    uint64_t failures = 0;
};

class SceneCoordinator final {
  public:
    SceneCoordinator() noexcept = default;
    ~SceneCoordinator();

    SceneCoordinator(const SceneCoordinator&) = delete;
    SceneCoordinator& operator=(const SceneCoordinator&) = delete;
    SceneCoordinator(SceneCoordinator&&) = delete;
    SceneCoordinator& operator=(SceneCoordinator&&) = delete;

    SaccadeResult initialize(const SceneCoordinatorConfig&, SceneCoordinatorStorage*) noexcept;
    SaccadeResult request_semantic(const SaccadeAccessibilityQueryDesc&) noexcept;
    SaccadeResult cancel_semantic() noexcept;
    SaccadeResult set_source(SceneSource) noexcept;
    SaccadeResult set_scope(const geometry::RectQ8*) noexcept;
    SaccadeResult set_filter(TargetFilterConfig) noexcept;
    SaccadeResult set_fusion(scene::FusionConfig) noexcept;
    SaccadeResult publish_grid(scene::GridSceneConfig, SceneCoordinatorAdvance*) noexcept;
    SaccadeResult publish_windows(scene::WindowSceneConfig, const SaccadeWindowInfo*, uint32_t, SceneCoordinatorAdvance*) noexcept;
    SaccadeResult advance(SceneCoordinatorAdvance*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool semantic_running() const noexcept { return semantic_ticket_ != 0; }

    [[nodiscard]] SceneCoordinatorStats stats() const noexcept { return stats_; }

    [[nodiscard]] SceneCoordinatorStatus status() const noexcept;

    [[nodiscard]] SceneSource source() const noexcept { return source_; }

    [[nodiscard]] scene::FusionStats latest_fusion_stats() const noexcept { return latest_fusion_stats_; }

    [[nodiscard]] uint32_t latest_fusion_input_count() const noexcept { return latest_fusion_input_count_; }

    [[nodiscard]] scene::VisualTargetTrackerStats latest_visual_tracker_stats() const noexcept { return latest_visual_tracker_stats_; }

  private:
    SaccadeResult poll_semantic(bool*) noexcept;
    SaccadeResult publish(bool neural_changed, bool semantic_changed, SceneCoordinatorAdvance*) noexcept;
    SaccadeResult release_semantic_snapshot() noexcept;
    void record_publication(const SaccadeTargetPacketHeader&) noexcept;
    void clear_semantic() noexcept;

    SceneCoordinatorConfig config_{};
    SceneCoordinatorStorage* storage_ = nullptr;
    scene::PacketView neural_{};
    scene::PacketView semantic_{};
    SaccadeAccessibilityQueryDesc semantic_query_{};
    SceneCoordinatorStats stats_{};
    SceneCoordinatorStatus status_{};
    scene::FusionStats latest_fusion_stats_{};
    scene::VisualTargetTracker visual_tracker_{};
    scene::VisualTargetTrackerStats latest_visual_tracker_stats_{};
    SaccadeTicketHandle semantic_ticket_ = 0;
    SaccadeSnapshotHandle semantic_snapshot_ = 0;
    uint64_t next_scene_epoch_ = 1;
    uint64_t neural_scene_epoch_ = 0;
    uint64_t last_frame_id_ = 0;
    uint64_t last_transform_epoch_ = 0;
    uint64_t last_topology_epoch_ = 0;
    uint32_t latest_fusion_input_count_ = 0;
    SceneSource source_ = SceneSource::pixel;
    geometry::RectQ8 scope_{};
    bool scope_enabled_ = false;
    bool source_dirty_ = false;
    bool initialized_ = false;
};

static_assert(sizeof(SceneCoordinatorAdvance) == 32);
static_assert(sizeof(SceneCoordinatorStatus) == 56);
static_assert(sizeof(SceneCoordinatorStats) == 112);
static_assert(sizeof(TargetFilterConfig) == 12);
static_assert(sizeof(SceneCoordinatorStorage) >= 1'347'584 + sizeof(scene::VisualTargetTrackerStorage));

} // namespace saccade::application

#endif

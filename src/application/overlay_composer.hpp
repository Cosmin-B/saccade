#ifndef SACCADE_APPLICATION_OVERLAY_COMPOSER_HPP
#define SACCADE_APPLICATION_OVERLAY_COMPOSER_HPP

#include "geometry/coordinate_transform.hpp"
#include "interaction/hints.hpp"
#include "overlay/packet.hpp"
#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t overlay_placement_bucket_count = 32768;
constexpr uint32_t overlay_target_role_count = SACCADE_TARGET_ROLE_WINDOW + 1U;

enum class LabelPlacement : uint8_t { automatic = 0, above = 1, below = 2, left = 3, right = 4 };

struct OverlayComposeConfig {
    uint64_t display_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t active_target_id = 0;
    const geometry::CoordinateTransform* desktop_to_surface = nullptr;
    const SaccadeOverlayStyle* styles = nullptr;
    const uint16_t* glyph_symbols = nullptr;
    uint32_t style_count = 0;
    uint32_t glyph_symbol_count = 0;
    LabelPlacement placement = LabelPlacement::automatic;
    uint8_t reserved[3]{};
    std::array<uint8_t, overlay_target_role_count> role_styles{};
};

struct OverlayPlacementNode {
    uint32_t next = UINT32_MAX;
    int32_t cell_x = 0;
    int32_t cell_y = 0;
};

struct OverlayComposeWorkspace {
    std::array<uint32_t, overlay_placement_bucket_count> placement_heads{};
    std::array<OverlayPlacementNode, SACCADE_OVERLAY_MAX_TARGETS> placement_nodes{};
    std::array<const interaction::HintLabel*, SACCADE_TARGET_PACKET_MAX_TARGETS> labels_by_scene_index{};
    std::array<uint32_t, SACCADE_TARGET_PACKET_MAX_TARGETS> overlay_index_by_scene_index{};
    std::array<uint32_t, SACCADE_OVERLAY_MAX_TARGETS> scene_index_by_overlay_index{};
};

struct OverlayComposeResult {
    size_t byte_size = 0;
    uint32_t target_count = 0;
    uint32_t labels_placed = 0;
    uint32_t active_target_index = SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
    uint32_t reserved = 0;
};

struct OverlayComposeStats {
    uint64_t targets_read = 0;
    uint64_t targets_filtered = 0;
    uint64_t targets_clipped = 0;
    uint64_t labels_tested = 0;
    uint64_t collision_tests = 0;
    uint64_t labels_repositioned = 0;
    uint64_t compose_failures = 0;
    uint64_t packets_composed = 0;
};

class OverlayComposer final {
  public:
    SaccadeResult compose(const scene::PacketView&, const interaction::HintLabel*, uint32_t label_count,
                          const OverlayComposeConfig&, OverlayComposeWorkspace*, SaccadeMutableSpanU8,
                          OverlayComposeResult*) noexcept;

    [[nodiscard]] OverlayComposeStats stats() const noexcept { return stats_; }

  private:
    OverlayComposeStats stats_{};
};

static_assert(sizeof(OverlayPlacementNode) == 12);
static_assert(sizeof(OverlayComposeStats) == 64);

} // namespace saccade::application

#endif

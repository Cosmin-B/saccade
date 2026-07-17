#ifndef SACCADE_SCENE_FUSION_HPP
#define SACCADE_SCENE_FUSION_HPP

#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::scene {

constexpr uint32_t fusion_bucket_count = 32768;
constexpr uint32_t fusion_nodes_per_target = 5;
constexpr uint32_t fusion_node_count = SACCADE_TARGET_PACKET_MAX_TARGETS * fusion_nodes_per_target;

struct FusionConfig {
    uint32_t maximum_targets = SACCADE_TARGET_PACKET_MAX_TARGETS;
    uint16_t iou_threshold_q16 = 32768;
    uint16_t containment_threshold_q16 = 58982;
    uint16_t maximum_area_ratio_q8 = 1024;
    bool merge_duplicates = true;
    uint8_t reserved = 0;
};

struct FusionEpochs {
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
};

struct FusionStats {
    uint64_t packets_read = 0;
    uint64_t candidates_read = 0;
    uint64_t bucket_visits = 0;
    uint64_t overlap_tests = 0;
    uint64_t duplicates_merged = 0;
    uint64_t safety_merges = 0;
    uint64_t capacity_drops = 0;
    uint32_t targets_written = 0;
    uint32_t reserved = 0;
};

struct FusionNode {
    uint32_t next = UINT32_MAX;
    uint16_t target_index = 0;
    uint8_t level = 0;
    uint8_t reserved = 0;
};

struct FusionWorkspace {
    std::array<uint32_t, fusion_bucket_count> heads{};
    std::array<FusionNode, fusion_node_count> nodes{};
};

static_assert(sizeof(FusionNode) == 8);
static_assert(sizeof(FusionStats) == 64);

SaccadeResult fuse(const PacketView*, uint32_t, const FusionConfig&, const FusionEpochs&, FusionWorkspace*,
                   SaccadeMutableSpanU8, size_t*, FusionStats*) noexcept;

} // namespace saccade::scene

#endif

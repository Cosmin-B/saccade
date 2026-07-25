#ifndef SACCADE_SCENE_TEMPORAL_HPP
#define SACCADE_SCENE_TEMPORAL_HPP

#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::scene {

constexpr uint32_t temporal_hash_slot_count = 32768;
constexpr uint32_t temporal_window_hash_slot_count = 32768;
constexpr size_t temporal_full_target_payload_bytes = sizeof(SaccadeSceneDeltaOwner) +
                                                      sizeof(SaccadeSceneDeltaGeometry) + sizeof(uint32_t) +
                                                      sizeof(SaccadeSceneDeltaClassification) + 3U * sizeof(uint32_t);
constexpr size_t temporal_delta_max_bytes =
    sizeof(SaccadeSceneDeltaHeader) +
    static_cast<size_t>(SACCADE_SCENE_DELTA_MAX_OPERATIONS) * sizeof(SaccadeSceneDeltaRecord) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * temporal_full_target_payload_bytes +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeSceneWindowTransform) +
    SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;

struct TemporalWindowTransform {
    uint64_t window_id = 0;
    uint64_t display_id = 0;
    int32_t translation_x_q8 = 0;
    int32_t translation_y_q8 = 0;
    uint32_t target_count = 0;
    uint32_t flags = 0;
    bool valid = false;
    bool window_role = false;
    uint8_t reserved[6]{};
};

struct TemporalStorage {
    std::array<SaccadeTargetRecord, SACCADE_TARGET_PACKET_MAX_TARGETS> targets{};
    std::array<uint8_t, SACCADE_TARGET_PACKET_MAX_TEXT_BYTES> text{};
    std::array<uint16_t, temporal_hash_slot_count> hash_slots{};
    std::array<uint8_t, SACCADE_TARGET_PACKET_MAX_TARGETS> seen{};
    std::array<TemporalWindowTransform, SACCADE_TARGET_PACKET_MAX_TARGETS> window_transforms{};
    std::array<uint16_t, temporal_window_hash_slot_count> window_hash_slots{};
};

struct TemporalStats {
    uint64_t target_lookups = 0;
    uint64_t hash_probes = 0;
    uint64_t payload_bytes = 0;
    uint32_t previous_targets = 0;
    uint32_t current_targets = 0;
    uint32_t operations = 0;
    uint32_t additions = 0;
    uint32_t updates = 0;
    uint32_t removals = 0;
    uint32_t window_transforms = 0;
    uint32_t transform_changes = 0;
    uint32_t topology_changes = 0;
    uint32_t reserved = 0;
};

class TemporalCompiler final {
  public:
    SaccadeResult initialize(TemporalStorage*) noexcept;
    SaccadeResult compile(const PacketView&, SaccadeMutableSpanU8, size_t*, TemporalStats*) noexcept;

  private:
    static constexpr uint16_t missing_target_ = UINT16_MAX;

    uint16_t find_target(uint64_t, uint64_t, TemporalStats*) const noexcept;
    void insert_target(uint32_t) noexcept;
    uint16_t find_window_transform(uint64_t) const noexcept;
    void build_window_transforms(const PacketView&, TemporalStats*) noexcept;
    void adopt(const PacketView&) noexcept;

    TemporalStorage* storage_ = nullptr;
    SaccadeTargetPacketHeader previous_header_{};
    uint32_t previous_target_count_ = 0;
    uint32_t previous_text_size_ = 0;
    uint32_t window_transform_count_ = 0;
    bool initialized_ = false;
    bool has_previous_ = false;
};

static_assert(sizeof(SaccadeSceneDeltaRecord) == 24);
static_assert(sizeof(SaccadeSceneDeltaOwner) == 16);
static_assert(sizeof(SaccadeSceneDeltaGeometry) == 24);
static_assert(sizeof(SaccadeSceneDeltaClassification) == 4);
static_assert(sizeof(SaccadeSceneWindowTransform) == 24);
static_assert(sizeof(SaccadeSceneDeltaHeader) == 152);
static_assert(sizeof(TemporalStats) == 64);
static_assert(temporal_full_target_payload_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES <= UINT16_MAX);

} // namespace saccade::scene

#endif

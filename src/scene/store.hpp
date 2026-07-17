#ifndef SACCADE_SCENE_STORE_HPP
#define SACCADE_SCENE_STORE_HPP

#include "core/cache_line.hpp"
#include "scene/packet.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace saccade::scene {

constexpr size_t target_packet_max_bytes =
    sizeof(SaccadeTargetPacketHeader) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord) +
    SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;
constexpr size_t scene_slot_alignment = 64;
constexpr size_t scene_slot_payload_bytes = target_packet_max_bytes + sizeof(size_t);
constexpr size_t scene_slot_padding_bytes = scene_slot_alignment - scene_slot_payload_bytes % scene_slot_alignment;

struct alignas(scene_slot_alignment) ScenePacketSlot {
    std::array<uint8_t, target_packet_max_bytes> bytes{};
    size_t byte_size = 0;
    std::array<std::byte, scene_slot_padding_bytes> padding{};
};

struct SceneStoreStorage {
    std::array<ScenePacketSlot, 3> slots{};
};

struct MutableScenePacket {
    uint8_t* data = nullptr;
    size_t capacity = 0;
    uint32_t slot = 0;
    uint32_t reserved = 0;
};

struct SceneStoreStats {
    uint64_t write_begins = 0;
    uint64_t published = 0;
    uint64_t replaced = 0;
    uint64_t producer_busy = 0;
    uint64_t copied_bytes = 0;
    uint64_t acquired = 0;
    uint64_t reused_active = 0;
    uint64_t returned = 0;
};

class SceneStore final {
  public:
    SaccadeResult initialize(SceneStoreStorage*) noexcept;
    SaccadeResult begin_write(MutableScenePacket*) noexcept;
    SaccadeResult abort_write(const MutableScenePacket&) noexcept;
    SaccadeResult commit_checked(const MutableScenePacket&, size_t) noexcept;
    SaccadeResult commit_trusted(const MutableScenePacket&, size_t) noexcept;
    SaccadeResult publish_copy(SaccadeSpanU8) noexcept;
    SaccadeResult acquire_latest(PacketView*) noexcept;

    [[nodiscard]] SceneStoreStats stats() const noexcept;

  private:
    struct alignas(core::destructive_interference_size) Handoff {
        std::atomic<uint32_t> slot{0};
        std::array<std::byte, core::destructive_interference_size - sizeof(std::atomic<uint32_t>)> padding{};
    };

    static_assert(sizeof(Handoff) == core::destructive_interference_size);

    SaccadeResult commit(const MutableScenePacket&, size_t, bool) noexcept;
    void drain_returned() noexcept;

    Handoff pending_{};
    Handoff returned_{};
    SceneStoreStats producer_stats_{};
    SceneStoreStats consumer_stats_{};
    SceneStoreStorage* storage_ = nullptr;
    uint32_t free_mask_ = 0;
    uint32_t writing_slot_ = 0;
    uint32_t active_slot_ = 0;
    uint32_t retired_slot_ = 0;
    bool initialized_ = false;
    static constexpr size_t tail_payload_bytes_ =
        2U * sizeof(SceneStoreStats) + sizeof(SceneStoreStorage*) + 4U * sizeof(uint32_t) + sizeof(bool);
    static constexpr size_t tail_padding_bytes_ =
        core::destructive_interference_size - tail_payload_bytes_ % core::destructive_interference_size;
    [[maybe_unused]] std::array<std::byte, tail_padding_bytes_> padding_{};
};

static_assert(alignof(ScenePacketSlot) == scene_slot_alignment);
static_assert(sizeof(ScenePacketSlot) % scene_slot_alignment == 0);
static_assert(sizeof(SceneStoreStats) == 64);

} // namespace saccade::scene

#endif

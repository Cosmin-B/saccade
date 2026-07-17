#ifndef SACCADE_INTERACTION_HINTS_HPP
#define SACCADE_INTERACTION_HINTS_HPP

#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::interaction {

constexpr uint32_t maximum_hint_symbols = 16;
constexpr uint32_t maximum_hint_alphabet = 32;
constexpr uint32_t hint_target_binding_capacity = 16384;

enum class HintPriority : uint32_t { scene_order = 0, pointer = 1, scope_center = 2, randomized = 3 };

struct HintConfig {
    std::array<uint16_t, maximum_hint_alphabet> alphabet{};
    std::array<uint32_t, maximum_hint_alphabet> physical_keys{};
    uint32_t alphabet_count = 0;
    HintPriority priority = HintPriority::scene_order;
    int32_t pointer_x_q8 = 0;
    int32_t pointer_y_q8 = 0;
    int32_t scope_center_x_q8 = 0;
    int32_t scope_center_y_q8 = 0;
    uint64_t random_seed = 0;
};

uint16_t symbol_for_physical_key(const HintConfig&, uint32_t physical_key) noexcept;

struct HintLabel {
    uint64_t target_id = 0;
    uint32_t target_index = 0;
    uint16_t symbol_count = 0;
    uint16_t reserved = 0;
    std::array<uint16_t, maximum_hint_symbols> symbols{};
};

struct HintTargetBinding {
    uint64_t target_id = 0;
    uint32_t target_index = 0;
    uint32_t reserved = 0;
};

struct HintSessionStorage {
    std::array<HintLabel, SACCADE_TARGET_PACKET_MAX_TARGETS> labels{};
    std::array<uint32_t, SACCADE_TARGET_PACKET_MAX_TARGETS> target_indices{};
    std::array<HintTargetBinding, hint_target_binding_capacity> target_bindings{};
};

struct HintMatch {
    uint64_t target_id = 0;
    uint32_t target_index = 0;
    uint32_t candidate_count = 0;
    bool exact = false;
    uint8_t reserved[7]{};
};

struct HintSessionStats {
    uint64_t freezes = 0;
    uint64_t targets_labeled = 0;
    uint64_t prefix_queries = 0;
    uint64_t symbols_compared = 0;
    uint64_t scene_refreshes = 0;
    uint64_t refresh_failures = 0;
};

class HintSession final {
  public:
    SaccadeResult freeze(const scene::PacketView&, const HintConfig&, HintSessionStorage*) noexcept;
    SaccadeResult refresh_scene(const scene::PacketView&) noexcept;
    SaccadeResult resolve_prefix(const uint16_t*, uint32_t, HintMatch*) noexcept;
    SaccadeResult label_for_target(uint64_t, const HintLabel**) const noexcept;
    SaccadeResult cancel() noexcept;

    [[nodiscard]] const HintLabel* labels() const noexcept;

    [[nodiscard]] uint32_t label_count() const noexcept { return label_count_; }

    [[nodiscard]] uint64_t scene_epoch() const noexcept { return scene_epoch_; }

    [[nodiscard]] HintSessionStats stats() const noexcept { return stats_; }

  private:
    HintSessionStorage* storage_ = nullptr;
    HintSessionStats stats_{};
    uint64_t scene_epoch_ = 0;
    uint64_t transform_epoch_ = 0;
    uint64_t topology_epoch_ = 0;
    uint32_t label_count_ = 0;
    bool frozen_ = false;
};

static_assert(sizeof(HintLabel) == 48);
static_assert(sizeof(HintTargetBinding) == 16);
static_assert(sizeof(HintMatch) == 24);
static_assert((hint_target_binding_capacity & (hint_target_binding_capacity - 1U)) == 0);

} // namespace saccade::interaction

#endif

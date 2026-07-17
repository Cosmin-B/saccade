#ifndef SACCADE_INTERACTION_SELECTION_REDUCER_HPP
#define SACCADE_INTERACTION_SELECTION_REDUCER_HPP

#include "scene/packet.hpp"

#include <array>
#include <cstdint>

namespace saccade::interaction {

constexpr uint32_t maximum_selection_targets = 64;

enum class SelectionMode : uint32_t { single = 1, dual = 2, multi = 3, path = 4 };

enum class SelectionState : uint32_t { idle = 0, collecting = 1, complete = 2, cancelled = 3 };

enum class SelectionCancelReason : uint32_t {
    none = 0,
    user = 1,
    timeout = 2,
    scene_changed = 3,
    transform_changed = 4,
    topology_changed = 5,
    focus_changed = 6,
    permission_lost = 7
};

struct SelectionContext {
    uint64_t scene_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t focus_id = 0;
    uint64_t deadline_ns = 0;
};

struct SelectionStorage {
    std::array<uint64_t, maximum_selection_targets> target_ids{};
};

struct SelectionView {
    const uint64_t* target_ids = nullptr;
    uint32_t target_count = 0;
    SelectionMode mode = SelectionMode::single;
    SelectionState state = SelectionState::idle;
    SelectionCancelReason cancel_reason = SelectionCancelReason::none;
};

struct SelectionStats {
    uint64_t sessions_started = 0;
    uint64_t targets_selected = 0;
    uint64_t backspaces = 0;
    uint64_t confirmations = 0;
    uint64_t cancellations = 0;
    uint64_t path_targets_expanded = 0;
    uint64_t scene_refreshes = 0;
};

class SelectionReducer final {
  public:
    SaccadeResult begin(const scene::PacketView&, SelectionMode, const SelectionContext&, SelectionStorage*) noexcept;
    SaccadeResult select(uint64_t target_id) noexcept;
    SaccadeResult backspace() noexcept;
    SaccadeResult confirm() noexcept;
    SaccadeResult cancel(SelectionCancelReason) noexcept;
    SaccadeResult refresh_scene(const scene::PacketView&) noexcept;
    SaccadeResult validate(const SelectionContext&, uint64_t now_ns) noexcept;
    SaccadeResult reset() noexcept;

    [[nodiscard]] SelectionView view() const noexcept;

    [[nodiscard]] SelectionStats stats() const noexcept { return stats_; }

  private:
    const SaccadeTargetRecord* find_target(uint64_t) const noexcept;
    bool selected(uint64_t) const noexcept;
    SaccadeResult expand_path() noexcept;

    const SaccadeTargetPacketHeader* scene_header_ = nullptr;
    const SaccadeTargetRecord* scene_targets_ = nullptr;
    SelectionStorage* storage_ = nullptr;
    SelectionContext context_{};
    SelectionStats stats_{};
    uint32_t target_count_ = 0;
    SelectionMode mode_ = SelectionMode::single;
    SelectionState state_ = SelectionState::idle;
    SelectionCancelReason cancel_reason_ = SelectionCancelReason::none;
};

static_assert(sizeof(SelectionContext) == 40);
static_assert(sizeof(SelectionStorage) == 512);

} // namespace saccade::interaction

#endif

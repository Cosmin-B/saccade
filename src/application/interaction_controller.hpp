#ifndef SACCADE_APPLICATION_INTERACTION_CONTROLLER_HPP
#define SACCADE_APPLICATION_INTERACTION_CONTROLLER_HPP

#include "application/hotkeys.hpp"
#include "application/session.hpp"
#include "geometry/coordinate_transform.hpp"
#include "interaction/interaction_state.hpp"

#include <array>
#include <cstdint>

namespace saccade::application {

constexpr uint64_t default_interaction_timeout_ns = UINT64_C(2'000'000'000);
constexpr uint64_t default_continuous_scroll_lease_ns = UINT64_C(250'000'000);
constexpr int32_t default_scroll_step_q8 = 3 * 256;

using InteractionState = interaction::InteractionState;
using ReadInteractionStateFn = interaction::ReadInteractionStateFn;

enum class PointerFinalPosition : uint32_t { target = 0, original = 1, anchor = 2 };
using ForwardCommandFn = SaccadeResult (*)(void*, Command, uint64_t) noexcept;
using InputLeaseActiveFn = bool (*)(void*) noexcept;
using NeutralizeInputFn = SaccadeResult (*)(void*) noexcept;

struct InteractionStateSource {
    void* context = nullptr;
    ReadInteractionStateFn read = nullptr;
};

struct InteractionControllerSink {
    void* context = nullptr;
    ForwardCommandFn forward = nullptr;
    InputLeaseActiveFn input_lease_active = nullptr;
    NeutralizeInputFn neutralize_input = nullptr;
};

struct InteractionProfile {
    interaction::HintConfig hints{};
    uint64_t timeout_ns = default_interaction_timeout_ns;
    uint64_t hold_duration_ns = 0;
    uint64_t drag_duration_ns = 0;
    uint64_t scroll_duration_ns = 0;
    int32_t scroll_vertical_q8 = default_scroll_step_q8;
    int32_t scroll_horizontal_q8 = default_scroll_step_q8;
    uint32_t click_modifiers = 0;
    interaction::SelectionMode initial_mode = interaction::SelectionMode::single;
    PointerFinalPosition final_pointer = PointerFinalPosition::target;
    uint32_t pointer_duration_ms = 0;
    geometry::PointQ8 pointer_anchor{};
};

struct InteractionCommandResult {
    SessionEvent session{};
    bool action_started = false;
    bool mode_changed = false;
    bool forwarded = false;
    bool target_adjusted = false;
    uint8_t reserved[4]{};
};

struct InteractionControllerStats {
    uint64_t commands = 0;
    uint64_t actions_started = 0;
    uint64_t modes_changed = 0;
    uint64_t commands_forwarded = 0;
    uint64_t physical_inputs = 0;
    uint64_t sessions_cancelled = 0;
    uint64_t input_neutralizations = 0;
    uint64_t target_adjustments = 0;
    uint64_t failures = 0;
};

class InteractionController final {
  public:
    SaccadeResult initialize(SessionEngine*, InteractionProfile, InteractionStateSource,
                             InteractionControllerSink) noexcept;
    SaccadeResult set_profile(InteractionProfile) noexcept;
    SaccadeResult set_text(SaccadeSpanU8) noexcept;
    SaccadeResult dispatch(Command, uint64_t timestamp_ns, InteractionCommandResult*) noexcept;
    SaccadeResult observe_physical_input(uint64_t timestamp_ns) noexcept;
    SaccadeResult tick(uint64_t timestamp_ns) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] interaction::SelectionMode selection_mode() const noexcept { return selection_mode_; }

    [[nodiscard]] InteractionControllerStats stats() const noexcept { return stats_; }

  private:
    SaccadeResult begin_action(const SessionAction&, interaction::SelectionMode, uint64_t timestamp_ns,
                               InteractionCommandResult*) noexcept;
    SaccadeResult change_mode(interaction::SelectionMode, InteractionCommandResult*) noexcept;
    SaccadeResult forward(Command, uint64_t, InteractionCommandResult*) noexcept;
    [[nodiscard]] bool input_lease_active() const noexcept;
    [[nodiscard]] uint64_t next_plan_id() noexcept;

    SessionEngine* session_ = nullptr;
    InteractionProfile profile_{};
    InteractionStateSource state_source_{};
    InteractionControllerSink sink_{};
    SessionAction last_action_{};
    std::array<uint8_t, interaction::maximum_action_payload_bytes> text_{};
    InteractionControllerStats stats_{};
    uint64_t next_plan_id_ = 1;
    uint32_t text_size_ = 0;
    interaction::SelectionMode selection_mode_ = interaction::SelectionMode::single;
    interaction::SelectionMode last_mode_ = interaction::SelectionMode::single;
    bool initialized_ = false;
    bool has_last_action_ = false;
};

SaccadeResult start_interaction_command(void*, Command, uint64_t) noexcept;
void observe_interaction_input(void*, uint64_t timestamp_ns) noexcept;

static_assert(sizeof(InteractionState) == 88);
static_assert(sizeof(InteractionCommandResult) == 32);
static_assert(sizeof(InteractionControllerStats) == 72);

} // namespace saccade::application

#endif

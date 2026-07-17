#ifndef SACCADE_INPUT_PLAN_HPP
#define SACCADE_INPUT_PLAN_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::input {

constexpr size_t maximum_plan_payload_bytes = 16384;
constexpr size_t maximum_plan_bytes = sizeof(SaccadeInputPlanHeader) +
                                      SACCADE_INPUT_PLAN_MAX_COMMANDS * sizeof(SaccadeInputCommand) +
                                      maximum_plan_payload_bytes;

struct PlanStorage {
    alignas(8) std::array<uint8_t, maximum_plan_bytes> bytes{};
};

struct PlanView {
    const SaccadeInputPlanHeader* header = nullptr;
    const SaccadeInputCommand* commands = nullptr;
    size_t byte_size = 0;
};

SaccadeResult validate_plan(SaccadeSpanU8, PlanView*) noexcept;

} // namespace saccade::input

#endif

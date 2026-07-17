#ifndef SACCADE_INPUT_EXECUTION_PREFLIGHT_HPP
#define SACCADE_INPUT_EXECUTION_PREFLIGHT_HPP

#include "input/plan.hpp"
#include "scene/packet.hpp"

#include <cstdint>

namespace saccade::input {

struct ExecutionPreflightState {
    scene::PacketView scene{};
    uint64_t focus_id = 0;
    uint64_t window_id = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
    uint32_t buttons = 0;
    bool input_available = false;
    bool surface_secure = false;
    bool target_window_available = false;
    bool target_point_secure = false;
    bool validate_active_window = false;
    bool validate_initial_buttons = false;
};

SaccadeResult validate_execution_preflight(const PlanView&, const ExecutionPreflightState&, uint64_t now_ns) noexcept;

} // namespace saccade::input

#endif

#include "input/plan.hpp"
#include "tests/support/input_plan.hpp"

#include <cstdint>

int main() {
    auto fixture = saccade::test::input_plan(3);
    saccade::input::PlanView plan{};
    if (saccade::input::validate_plan({fixture.bytes.data(), fixture.size}, &plan) != SACCADE_OK ||
        plan.header->command_count != 3 || plan.commands[2].target_id != 3) {
        return 1;
    }
    auto* header = reinterpret_cast<SaccadeInputPlanHeader*>(fixture.bytes.data());
    header->expected_buttons = UINT32_C(0x80000000);
    if (saccade::input::validate_plan({fixture.bytes.data(), fixture.size}, &plan) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 2;
    }
    fixture = saccade::test::input_plan(1);
    auto* command = reinterpret_cast<SaccadeInputCommand*>(fixture.bytes.data() + sizeof(SaccadeInputPlanHeader));
    command->data0 = SACCADE_INPUT_BUTTON_LEFT | SACCADE_INPUT_BUTTON_RIGHT;
    if (saccade::input::validate_plan({fixture.bytes.data(), fixture.size}, &plan) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 3;
    }
    return 0;
}

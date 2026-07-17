#include "application/debugger_layout.hpp"

namespace saccade::application {
namespace {

constexpr int32_t margin = 16;
constexpr int32_t gap = 10;
constexpr int32_t vertical_gap = 8;
constexpr int32_t control_height = 28;
constexpr int32_t action_width = 116;
constexpr int32_t fault_width = 150;
constexpr int32_t arm_fault_width = 156;
constexpr int32_t action_count = static_cast<int32_t>(debugger_action_count);
constexpr int32_t standard_footer_width =
    action_count * action_width + (action_count - 1) * gap + gap + fault_width + gap + arm_fault_width;

} // namespace

SaccadeResult make_debugger_layout(int32_t width, int32_t height, DebuggerLayout* output) noexcept {
    if (output == nullptr || width < debugger_minimum_width || height < debugger_minimum_height)
        return SACCADE_ERROR_INVALID_ARGUMENT;

    DebuggerLayout layout{};
    const int32_t available_width = width - margin * 2;
    layout.compact = available_width < standard_footer_width;
    const int32_t footer_height = layout.compact ? control_height * 2 + gap : control_height;
    const int32_t footer_y = height - margin - footer_height;

    layout.views = {margin, margin, available_width, control_height};
    layout.content = {margin, margin + control_height + vertical_gap, available_width,
                      footer_y - vertical_gap - (margin + control_height + vertical_gap)};

    if (!layout.compact) {
        int32_t x = margin;
        for (DebuggerLayoutRect& action : layout.actions) {
            action = {x, footer_y, action_width, control_height};
            x += action_width + gap;
        }
        layout.fault = {x, footer_y, fault_width, control_height};
        layout.arm_fault = {x + fault_width + gap, footer_y, arm_fault_width, control_height};
    } else {
        const int32_t compact_action_width = (available_width - gap * (action_count - 1)) / action_count;
        int32_t x = margin;
        for (uint32_t index = 0; index < debugger_action_count; ++index) {
            const int32_t next_x = index + 1U == debugger_action_count ? width - margin : x + compact_action_width;
            layout.actions[index] = {x, footer_y, next_x - x, control_height};
            x = next_x + gap;
        }

        const int32_t second_row_y = footer_y + control_height + gap;
        const int32_t split_width = (available_width - gap) / 2;
        layout.fault = {margin, second_row_y, split_width, control_height};
        layout.arm_fault = {margin + split_width + gap, second_row_y, width - margin - (margin + split_width + gap),
                            control_height};
    }

    *output = layout;
    return SACCADE_OK;
}

} // namespace saccade::application

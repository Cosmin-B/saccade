#include "application/debugger_layout.hpp"

#include <array>
#include <cstdint>

namespace {

using saccade::application::DebuggerLayout;
using saccade::application::DebuggerLayoutRect;

bool inside(const DebuggerLayoutRect& rect, int32_t width, int32_t height) noexcept {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 && rect.x + rect.width <= width &&
           rect.y + rect.height <= height;
}

bool overlaps(const DebuggerLayoutRect& left, const DebuggerLayoutRect& right) noexcept {
    return left.x < right.x + right.width && left.x + left.width > right.x && left.y < right.y + right.height &&
           left.y + left.height > right.y;
}

bool valid(const DebuggerLayout& layout, int32_t width, int32_t height) noexcept {
    std::array<DebuggerLayoutRect, 8> rects{layout.views,      layout.content,    layout.actions[0], layout.actions[1],
                                            layout.actions[2], layout.actions[3], layout.fault,      layout.arm_fault};
    for (uint32_t index = 0; index < rects.size(); ++index) {
        if (!inside(rects[index], width, height)) return false;
        for (uint32_t other = index + 1U; other < rects.size(); ++other) {
            if (overlaps(rects[index], rects[other])) return false;
        }
    }
    return true;
}

} // namespace

int main() {
    static_assert(saccade::application::debugger_view_count == 8);
    static_assert(static_cast<uint32_t>(saccade::application::DebuggerView::frames_transforms) == 6);
    static_assert(static_cast<uint32_t>(saccade::application::DebuggerView::scene_fusion) == 7);

    DebuggerLayout standard{};
    if (saccade::application::make_debugger_layout(860, 560, &standard) != SACCADE_OK || standard.compact ||
        !valid(standard, 860, 560) || standard.views.x != 16 || standard.views.y != 16 || standard.content.y != 52 ||
        standard.actions[0].width != 116 || standard.fault.width != 150 || standard.arm_fault.width != 156)
        return 1;

    DebuggerLayout compact{};
    if (saccade::application::make_debugger_layout(640, 480, &compact) != SACCADE_OK || !compact.compact ||
        !valid(compact, 640, 480) || compact.actions[0].y != compact.actions[3].y ||
        compact.fault.y <= compact.actions[0].y || compact.arm_fault.y != compact.fault.y)
        return 2;

    DebuggerLayout minimum{};
    if (saccade::application::make_debugger_layout(saccade::application::debugger_minimum_width,
                                                   saccade::application::debugger_minimum_height,
                                                   &minimum) != SACCADE_OK ||
        !minimum.compact ||
        !valid(minimum, saccade::application::debugger_minimum_width, saccade::application::debugger_minimum_height))
        return 3;

    if (saccade::application::make_debugger_layout(saccade::application::debugger_minimum_width - 1,
                                                   saccade::application::debugger_minimum_height,
                                                   &minimum) != SACCADE_ERROR_INVALID_ARGUMENT ||
        saccade::application::make_debugger_layout(saccade::application::debugger_minimum_width,
                                                   saccade::application::debugger_minimum_height - 1,
                                                   &minimum) != SACCADE_ERROR_INVALID_ARGUMENT ||
        saccade::application::make_debugger_layout(saccade::application::debugger_minimum_width,
                                                   saccade::application::debugger_minimum_height,
                                                   nullptr) != SACCADE_ERROR_INVALID_ARGUMENT)
        return 4;

    return 0;
}

#pragma once

#include <saccade/saccade.h>

#include <array>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t debugger_action_count = 4;
constexpr uint32_t debugger_view_count = 8;
constexpr int32_t debugger_minimum_width = 480;
constexpr int32_t debugger_minimum_height = 260;

enum class DebuggerView : uint32_t {
    overview = 0,
    displays,
    runtime,
    overlay_gpu,
    memory,
    trace,
    frames_transforms,
    scene_fusion
};

struct DebuggerLayoutRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct DebuggerLayout {
    DebuggerLayoutRect views{};
    DebuggerLayoutRect content{};
    std::array<DebuggerLayoutRect, debugger_action_count> actions{};
    DebuggerLayoutRect fault{};
    DebuggerLayoutRect arm_fault{};
    bool compact = false;
};

SaccadeResult make_debugger_layout(int32_t width, int32_t height, DebuggerLayout* output) noexcept;

} // namespace saccade::application

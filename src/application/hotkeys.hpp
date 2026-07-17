#ifndef SACCADE_APPLICATION_HOTKEYS_HPP
#define SACCADE_APPLICATION_HOTKEYS_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_input.h>

#include <cstdint>

namespace saccade::application {

constexpr uint32_t maximum_hotkey_bindings = 128;

enum class Command : uint32_t {
    pointer_move = 1,
    hover,
    left_click,
    right_click,
    middle_click,
    double_click,
    hold,
    drag,
    scroll_vertical,
    scroll_horizontal,
    select_text,
    repeat_action,
    free_pointer,
    window_activate,
    window_cycle_forward,
    window_cycle_backward,
    window_activate_behind,
    window_activate_left,
    window_activate_right,
    window_activate_up,
    window_activate_down,
    mode_single,
    mode_dual,
    mode_multi,
    mode_path,
    source_pixel,
    source_semantic,
    source_grid,
    scope_desktop,
    scope_active_window,
    scope_monitor,
    target_position_next,
    edge_snap_left,
    edge_snap_right,
    edge_snap_up,
    edge_snap_down,
    nudge_left,
    nudge_right,
    nudge_up,
    nudge_down,
    confirm,
    backspace,
    cancel,
    suspend_toggle,
    open_settings,
    restart,
    quit,
    type_text,
    source_fused,
    target_position_1,
    target_position_2,
    target_position_3,
    target_position_4,
    target_position_5,
    target_position_6,
    target_position_7,
    target_position_8,
    target_position_9,
    scroll_up,
    scroll_down,
    scroll_left,
    scroll_right,
    scroll_up_continuous,
    scroll_down_continuous,
    scroll_left_continuous,
    scroll_right_continuous,
    scope_toggle
};

constexpr Command last_command = Command::scope_toggle;

constexpr bool command_targets_scene(Command command) noexcept {
    return (command >= Command::pointer_move && command <= Command::window_activate) || command == Command::type_text ||
           (command >= Command::scroll_up && command <= Command::scroll_right_continuous);
}

enum : uint32_t { hotkey_always_active = UINT32_C(1) << 0, hotkey_session_only = UINT32_C(1) << 1 };

struct HotkeyBinding {
    Command command = Command::pointer_move;
    uint32_t physical_key = 0;
    uint32_t modifiers = 0;
    uint16_t flags = 0;
    uint16_t logical_symbol = 0;
};

struct CommandEvent {
    uint64_t timestamp_ns = 0;
    Command command = Command::pointer_move;
    uint32_t physical_key = 0;
    uint32_t modifiers = 0;
    uint32_t flags = 0;
};

struct KeyEvent {
    uint64_t timestamp_ns = 0;
    uint32_t physical_key = 0;
    uint32_t modifiers = 0;
    uint16_t logical_symbol = 0;
    uint16_t reserved = 0;
};

using CommandFn = void (*)(void*, const CommandEvent&) noexcept;
using InputObservedFn = void (*)(void*, uint64_t timestamp_ns) noexcept;
using KeyFn = bool (*)(void*, const KeyEvent&) noexcept;

struct CommandSink {
    void* context = nullptr;
    CommandFn command = nullptr;
    InputObservedFn input_observed = nullptr;
    KeyFn key = nullptr;
    InputObservedFn command_observed = nullptr;
};

struct HotkeyStats {
    uint64_t registrations = 0;
    uint64_t unregisters = 0;
    uint64_t events = 0;
    uint64_t dispatched = 0;
    uint64_t suspended = 0;
    uint64_t unknown = 0;
    uint64_t failures = 0;
    uint64_t replacements = 0;
};

static_assert(sizeof(HotkeyBinding) == 16);
static_assert(sizeof(CommandEvent) == 24);
static_assert(sizeof(KeyEvent) == 24);
static_assert(sizeof(HotkeyStats) == 64);

} // namespace saccade::application

#endif

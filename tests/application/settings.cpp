#include "application/settings_controller.hpp"
#include "application/binding_editor.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    defaults_failed,
    encode_failed,
    decode_failed,
    roundtrip_failed,
    conflict_failed,
    reset_failed,
    corruption_failed,
    controller_failed,
    binding_editor_failed,
    logical_fallback_failed,
    appearance_style_failed,
    hint_mapping_failed,
    migration_failed,
    keyboard_layout_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult apply(void* context, const saccade::application::SettingsDocument&) noexcept {
    ++*static_cast<uint32_t*>(context);
    return SACCADE_OK;
}

const saccade::application::HotkeyBinding* binding_for(const saccade::application::SettingsDocument& settings,
                                                       saccade::application::Command command) noexcept {
    for (uint32_t index = 0; index < settings.binding_count; ++index)
        if (settings.bindings[index].command == command) return &settings.bindings[index];
    return nullptr;
}

} // namespace

int main() {
    using namespace saccade::application;
    SettingsDocument settings = default_settings();
    constexpr std::array global_commands{
        Command::pointer_move,
        Command::hover,
        Command::left_click,
        Command::right_click,
        Command::middle_click,
        Command::double_click,
        Command::hold,
        Command::drag,
        Command::scroll_vertical,
        Command::scroll_horizontal,
        Command::select_text,
        Command::repeat_action,
        Command::free_pointer,
        Command::window_activate,
        Command::type_text,
        Command::scroll_up,
        Command::scroll_down,
        Command::scroll_left,
        Command::scroll_right,
        Command::scroll_up_continuous,
        Command::scroll_down_continuous,
        Command::scroll_left_continuous,
        Command::scroll_right_continuous,
        Command::suspend_toggle,
    };
    constexpr std::array session_commands{
        Command::window_cycle_forward,
        Command::window_cycle_backward,
        Command::window_activate_behind,
        Command::window_activate_left,
        Command::window_activate_right,
        Command::window_activate_up,
        Command::window_activate_down,
        Command::mode_single,
        Command::mode_dual,
        Command::mode_multi,
        Command::mode_path,
        Command::source_pixel,
        Command::source_semantic,
        Command::source_grid,
        Command::source_fused,
        Command::scope_toggle,
        Command::target_position_next,
        Command::target_position_1,
        Command::target_position_2,
        Command::target_position_3,
        Command::target_position_4,
        Command::target_position_5,
        Command::target_position_6,
        Command::target_position_7,
        Command::target_position_8,
        Command::target_position_9,
        Command::edge_snap_left,
        Command::edge_snap_right,
        Command::edge_snap_up,
        Command::edge_snap_down,
        Command::nudge_left,
        Command::nudge_right,
        Command::nudge_up,
        Command::nudge_down,
        Command::confirm,
        Command::backspace,
        Command::cancel,
    };
    if (validate_settings(settings) != SACCADE_OK || settings.detector.confidence_q16 != 1 ||
        settings.detector.text_sensitivity_q16 != 1 || settings.hints.physical_keys[0] != 0x04 ||
        settings.hints.physical_keys[1] != 0x16 ||
        settings.binding_count != global_commands.size() + session_commands.size()) {
        return result(TestResult::defaults_failed);
    }
    for (Command command : global_commands) {
        const HotkeyBinding* binding = binding_for(settings, command);
        if (binding == nullptr || (binding->flags & hotkey_session_only) != 0 ||
            (command == Command::suspend_toggle) != ((binding->flags & hotkey_always_active) != 0)) {
            return result(TestResult::defaults_failed);
        }
    }
    for (Command command : session_commands) {
        const HotkeyBinding* binding = binding_for(settings, command);
        if (binding == nullptr || binding->flags != hotkey_session_only) return result(TestResult::defaults_failed);
    }

    BindingKeyboardLayout keyboard{};
    if (layout_binding_keyboard(720, 240, 4, &keyboard) != SACCADE_OK || keyboard.key_count != binding_keys().size()) {
        return result(TestResult::keyboard_layout_failed);
    }
    for (uint32_t index = 0; index < keyboard.key_count; ++index) {
        const BindingKeyRect& key = keyboard.keys[index];
        if (key.usage == 0 || key.x < 0 || key.y < 0 || key.width <= 0 || key.height <= 0 || key.x + key.width > 720 ||
            key.y + key.height > 240) {
            return result(TestResult::keyboard_layout_failed);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            const BindingKeyRect& other = keyboard.keys[previous];
            const bool separated = key.x + key.width <= other.x || other.x + other.width <= key.x ||
                                   key.y + key.height <= other.y || other.y + other.height <= key.y;
            if (key.usage == other.usage || !separated) return result(TestResult::keyboard_layout_failed);
        }
    }

    std::array<uint16_t, saccade::interaction::maximum_hint_alphabet> translated{};
    for (uint32_t index = 0; index < settings.hints.alphabet_count; ++index)
        translated[index] = static_cast<uint16_t>(0x03b1 + index);
    HintSettings resolved{};
    if (resolve_hint_alphabet(settings.hints, translated.data(), &resolved) != SACCADE_OK ||
        resolved.alphabet[0] != translated[0] || resolved.physical_keys[0] != settings.hints.physical_keys[0]) {
        return result(TestResult::hint_mapping_failed);
    }
    InteractionProfile profile = make_interaction_profile(settings, resolved, 1, 2, 3, 4, 5);
    resolved.alphabet[0] = 'Z';
    if (profile.hints.alphabet[0] != translated[0] ||
        saccade::interaction::symbol_for_physical_key(profile.hints, settings.hints.physical_keys[0]) !=
            translated[0]) {
        return result(TestResult::hint_mapping_failed);
    }

    const SaccadeOverlayStyle dark = resolve_overlay_style(settings.appearance, settings.flags, true);
    const SaccadeOverlayStyle light = resolve_overlay_style(settings.appearance, settings.flags, false);
    AppearanceSettings custom = settings.appearance;
    custom.theme = Theme::custom;
    custom.font_size_q8 = 28U * 256U;
    custom.outline_width_q8 = 2U * 256U;
    custom.glow_radius_q8 = 4U * 256U;
    custom.label_rgba = 0x12345678;
    const SaccadeOverlayStyle large = resolve_overlay_style(custom, settings.flags | settings_reduced_motion, false);
    if (dark.label_background_rgba8 == light.label_background_rgba8 || dark.glyph_height_q3 != 14U * 8U ||
        dark.label_height_q3 != 18U * 8U || large.label_foreground_rgba8 != custom.label_rgba ||
        dark.flags != SACCADE_OVERLAY_STYLE_ANIMATED || large.glyph_height_q3 != 28U * 8U ||
        large.target_stroke_q3 != 2U * 8U || large.active_stroke_q3 != 6U * 8U ||
        large.glyph_advance_q3 <= large.glyph_width_q3 || large.flags != 0) {
        return result(TestResult::appearance_style_failed);
    }

    settings.source = TargetSource::fused;
    std::array<uint8_t, settings_encoded_capacity> first{};
    size_t first_size = 0;
    if (encode_settings(settings, {first.data(), first.size()}, &first_size) != SACCADE_OK || first_size == 0)
        return result(TestResult::encode_failed);

    constexpr size_t settings_header_bytes = 12;
    constexpr size_t body_prefix_bytes = 4 + 4 + 4 + 4 + 8;
    constexpr size_t binding_bytes = 4 + 4 + 4 + 4 + 2;
    constexpr size_t alphabet_prefix_bytes = 4 + saccade::interaction::maximum_hint_alphabet * sizeof(uint16_t);
    constexpr size_t physical_key_bytes = saccade::interaction::maximum_hint_alphabet * sizeof(uint32_t);
    const size_t physical_key_offset =
        settings_header_bytes + body_prefix_bytes + settings.binding_count * binding_bytes + alphabet_prefix_bytes;
    std::array<uint8_t, settings_encoded_capacity> legacy = first;
    std::memmove(legacy.data() + physical_key_offset, legacy.data() + physical_key_offset + physical_key_bytes,
                 first_size - physical_key_offset - physical_key_bytes);
    const size_t legacy_size = first_size - physical_key_bytes;
    legacy[4] = static_cast<uint8_t>(minimum_settings_version);
    legacy[5] = legacy[6] = legacy[7] = 0;
    legacy[8] = static_cast<uint8_t>(legacy_size);
    legacy[9] = static_cast<uint8_t>(legacy_size >> 8U);
    legacy[10] = static_cast<uint8_t>(legacy_size >> 16U);
    legacy[11] = static_cast<uint8_t>(legacy_size >> 24U);
    SettingsDocument migrated{};
    if (decode_settings({legacy.data(), legacy_size}, &migrated) != SACCADE_OK ||
        migrated.hints.physical_keys != settings.hints.physical_keys) {
        return result(TestResult::migration_failed);
    }
    SettingsDocument decoded{};
    if (decode_settings({first.data(), first_size}, &decoded) != SACCADE_OK) return result(TestResult::decode_failed);
    const HotkeyBinding* decoded_click = binding_for(decoded, Command::left_click);
    if (decoded.source != TargetSource::fused || decoded_click == nullptr || decoded_click->logical_symbol != ' ')
        return result(TestResult::logical_fallback_failed);
    std::array<uint8_t, settings_encoded_capacity> second{};
    size_t second_size = 0;
    if (encode_settings(decoded, {second.data(), second.size()}, &second_size) != SACCADE_OK ||
        second_size != first_size || std::memcmp(first.data(), second.data(), first_size) != 0) {
        return result(TestResult::roundtrip_failed);
    }
    decoded.bindings[1].physical_key = decoded.bindings[0].physical_key;
    decoded.bindings[1].modifiers = decoded.bindings[0].modifiers;
    decoded.bindings[1].logical_symbol = decoded.bindings[0].logical_symbol;
    if (validate_settings(decoded) != SACCADE_ERROR_ALREADY_EXISTS) return result(TestResult::conflict_failed);
    decoded = settings;
    decoded.bindings[0].logical_symbol = 0;
    if (validate_settings(decoded) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::logical_fallback_failed);
    if (reset_settings_page(SettingsPage::bindings, &decoded) != SACCADE_OK || validate_settings(decoded) != SACCADE_OK)
        return result(TestResult::reset_failed);

    constexpr std::array<uint16_t, 3> replacement_alphabet{'Q', 'W', 'E'};
    if (set_hint_alphabet(&decoded.hints, replacement_alphabet.data(),
                          static_cast<uint32_t>(replacement_alphabet.size())) != SACCADE_OK ||
        decoded.hints.alphabet_count != replacement_alphabet.size() || decoded.hints.physical_keys[0] != 0x14 ||
        decoded.hints.physical_keys[1] != 0x1a || decoded.hints.physical_keys[2] != 0x08 ||
        decoded.hints.physical_keys[3] != 0) {
        return result(TestResult::hint_mapping_failed);
    }
    first[0] = 0;
    if (decode_settings({first.data(), first_size}, &decoded) == SACCADE_OK)
        return result(TestResult::corruption_failed);
    uint32_t apply_count = 0;
    SettingsController controller;
    if (controller.initialize(settings, {&apply_count, apply}) != SACCADE_OK || controller.begin_edit() != SACCADE_OK) {
        return result(TestResult::controller_failed);
    }
    SettingsDocument staged = controller.staged();
    staged.source = TargetSource::fused;
    if (controller.stage(staged) != SACCADE_OK || controller.commit() != SACCADE_OK || controller.revision() != 2 ||
        controller.current().source != TargetSource::fused || apply_count != 2 ||
        controller.begin_edit() != SACCADE_OK || controller.reset_all() != SACCADE_OK ||
        controller.cancel() != SACCADE_OK || controller.current().source != TargetSource::fused ||
        controller.shutdown() != SACCADE_OK) {
        return result(TestResult::controller_failed);
    }
    SettingsDocument edited = settings;
    BindingConflict conflict{};
    uint32_t selected_binding = UINT32_MAX;
    if (set_binding(&edited, {Command::open_settings, 0x04, SACCADE_INPUT_MODIFIER_CONTROL, 0, 'A'}, &conflict) !=
            SACCADE_OK ||
        edited.binding_count != settings.binding_count + 1U ||
        set_binding(&edited, {Command::drag, 0x04, SACCADE_INPUT_MODIFIER_CONTROL, 0, 'A'}, &conflict) !=
            SACCADE_ERROR_ALREADY_EXISTS ||
        conflict.command != Command::open_settings || remove_binding(&edited, Command::open_settings) != SACCADE_OK ||
        edited.binding_count != settings.binding_count || command_name(Command::drag) == nullptr ||
        command_name(Command::source_fused) == nullptr || command_name(Command::target_position_9) == nullptr ||
        command_name(Command::scroll_up) == nullptr || command_name(Command::scroll_right_continuous) == nullptr ||
        command_name(Command::scope_toggle) == nullptr || default_logical_symbol(0x2f) != '[' ||
        default_logical_symbol(0x34) != '\'' || !command_targets_scene(Command::scroll_up) ||
        !command_targets_scene(Command::scroll_right_continuous) || command_targets_scene(Command::open_settings) ||
        find_binding(settings, Command::mode_single, &selected_binding) != SACCADE_OK ||
        settings.bindings[selected_binding].flags != hotkey_session_only ||
        binding_keys().size() != binding_key_count) {
        return result(TestResult::binding_editor_failed);
    }
    return result(TestResult::success);
}

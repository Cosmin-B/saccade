#ifndef SACCADE_APPLICATION_SETTINGS_HPP
#define SACCADE_APPLICATION_SETTINGS_HPP

#include "application/hotkeys.hpp"
#include "application/interaction_controller.hpp"
#include "interaction/hints.hpp"
#include "interaction/selection_reducer.hpp"

#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>
#include <saccade/saccade_overlay.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t settings_version = 4;
constexpr uint32_t minimum_settings_version = 2;
constexpr size_t settings_encoded_capacity = 4096;
constexpr uint32_t font_family_capacity = 64;
constexpr uint32_t language_tag_capacity = 16;

enum class TargetSource : uint32_t { pixel = 0, semantic = 1, grid = 2, fused = 3 };
enum class TargetScope : uint32_t { desktop = 0, active_window = 1, monitor = 2 };
enum class HintPlacement : uint32_t { automatic = 0, above = 1, below = 2, left = 3, right = 4 };
enum class HintSorting : uint32_t { sorted = 0, randomized = 1 };
enum class MergePolicy : uint32_t { balanced = 0, text_first = 1, controls_first = 2, disabled = 3 };
enum class FinalPointerPosition : uint32_t { target = 0, original = 1, anchor = 2 };
enum class Theme : uint32_t { system = 0, high_contrast = 1, light = 2, dark = 3, custom = 4 };
enum class ComputePolicy : uint32_t {
    automatic = 0,
    cpu_only = 1,
    cpu_and_gpu = 2,
    cpu_and_accelerator = 3,
    named_device = 4
};
enum class SettingsPage : uint32_t {
    bindings = 0,
    hints = 1,
    detector = 2,
    scope = 3,
    pointer = 4,
    appearance = 5,
    compute = 6
};

enum : uint32_t { settings_animate_overlay = UINT32_C(1) << 0, settings_reduced_motion = UINT32_C(1) << 1 };

struct DetectorSettings {
    uint16_t confidence_q16 = 1;
    uint16_t text_sensitivity_q16 = 1;
    uint16_t duplicate_iou_q16 = 32768;
    uint16_t minimum_width_q8 = 4U * 256U;
    uint16_t minimum_height_q8 = 4U * 256U;
    MergePolicy merge_policy = MergePolicy::balanced;
};

struct GridSettings {
    uint16_t rows = 8;
    uint16_t columns = 12;
    uint16_t margin_x_q8 = 8U * 256U;
    uint16_t margin_y_q8 = 8U * 256U;
};

struct HintSettings {
    std::array<uint16_t, interaction::maximum_hint_alphabet> alphabet{};
    std::array<uint32_t, interaction::maximum_hint_alphabet> physical_keys{};
    std::array<char, language_tag_capacity> language{};
    uint32_t alphabet_count = 0;
    interaction::HintPriority priority = interaction::HintPriority::scene_order;
    HintPlacement placement = HintPlacement::automatic;
    HintSorting sorting = HintSorting::sorted;
};

struct PointerSettings {
    FinalPointerPosition final_position = FinalPointerPosition::target;
    uint32_t movement_duration_ms = 0;
    int32_t anchor_x_q8 = 0;
    int32_t anchor_y_q8 = 0;
};

struct ActionSettings {
    interaction::SelectionMode initial_mode = interaction::SelectionMode::single;
    uint32_t timeout_ms = 2000;
    uint32_t hold_duration_ms = 0;
    uint32_t drag_duration_ms = 0;
    uint32_t scroll_duration_ms = 0;
    int32_t scroll_vertical_q8 = 3 * 256;
    int32_t scroll_horizontal_q8 = 3 * 256;
    uint32_t click_modifiers = 0;
};

struct AppearanceSettings {
    std::array<char, font_family_capacity> font_family{};
    Theme theme = Theme::system;
    HintPlacement placement = HintPlacement::automatic;
    uint32_t font_size_q8 = 14U * 256U;
    uint32_t font_weight = 600;
    uint32_t label_rgba = UINT32_C(0xffffffff);
    uint32_t background_rgba = UINT32_C(0x1b1b1bd9);
    uint32_t outline_rgba = UINT32_C(0x000000ff);
    uint32_t glow_rgba = UINT32_C(0x00000080);
    uint16_t outline_width_q8 = 256;
    uint16_t glow_radius_q8 = 2U * 256U;
};

struct ComputeSettings {
    ComputePolicy policy = ComputePolicy::automatic;
    uint64_t device_stable_id = 0;
};

struct SettingsDocument {
    std::array<HotkeyBinding, maximum_hotkey_bindings> bindings{};
    HintSettings hints{};
    DetectorSettings detector{};
    GridSettings grid{};
    PointerSettings pointer{};
    ActionSettings actions{};
    AppearanceSettings appearance{};
    ComputeSettings compute{};
    uint64_t monitor_stable_id = 0;
    uint32_t binding_count = 0;
    uint32_t flags = settings_animate_overlay;
    TargetSource source = TargetSource::pixel;
    TargetScope scope = TargetScope::desktop;
};

SettingsDocument default_settings() noexcept;
SaccadeOverlayStyle resolve_overlay_style(const AppearanceSettings&, uint32_t settings_flags,
                                          bool dark_system_theme) noexcept;
InteractionProfile make_interaction_profile(const SettingsDocument&, const HintSettings& resolved_hints,
                                            int32_t pointer_x_q8, int32_t pointer_y_q8, int32_t scope_center_x_q8,
                                            int32_t scope_center_y_q8, uint64_t random_seed) noexcept;
SaccadeResult resolve_hint_alphabet(const HintSettings&, const uint16_t* translated_symbols, HintSettings*) noexcept;
SaccadeResult set_hint_alphabet(HintSettings*, const uint16_t* symbols, uint32_t symbol_count) noexcept;
SaccadeResult validate_settings(const SettingsDocument&) noexcept;
SaccadeResult reset_settings_page(SettingsPage, SettingsDocument*) noexcept;
SaccadeResult encode_settings(const SettingsDocument&, SaccadeMutableSpanU8, size_t*) noexcept;
SaccadeResult decode_settings(SaccadeSpanU8, SettingsDocument*) noexcept;

} // namespace saccade::application

#endif

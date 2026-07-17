#include "application/settings.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace saccade::application {
namespace {

constexpr uint8_t settings_magic[4] = {'S', 'C', 'S', 'T'};
constexpr uint32_t settings_flag_mask = settings_animate_overlay | settings_reduced_motion;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;
constexpr uint32_t binding_flag_mask = hotkey_always_active | hotkey_session_only;
constexpr uint32_t hid_keyboard_first = 0x04;
constexpr uint32_t hid_keyboard_last = 0xe7;
constexpr uint32_t activation_modifiers = SACCADE_INPUT_MODIFIER_CONTROL | SACCADE_INPUT_MODIFIER_ALT;
constexpr uint32_t activation_shift_modifiers = activation_modifiers | SACCADE_INPUT_MODIFIER_SHIFT;

constexpr std::array<HotkeyBinding, 24> default_global_bindings{{
    {Command::pointer_move, 0x11, activation_modifiers, 0, 'N'},
    {Command::hover, 0x12, activation_modifiers, 0, 'O'},
    {Command::left_click, 0x2c, activation_modifiers, 0, ' '},
    {Command::right_click, 0x15, activation_modifiers, 0, 'R'},
    {Command::middle_click, 0x10, activation_modifiers, 0, 'M'},
    {Command::double_click, 0x07, activation_modifiers, 0, 'D'},
    {Command::hold, 0x0b, activation_modifiers, 0, 'H'},
    {Command::drag, 0x0a, activation_modifiers, 0, 'G'},
    {Command::scroll_vertical, 0x19, activation_modifiers, 0, 'V'},
    {Command::scroll_horizontal, 0x05, activation_modifiers, 0, 'B'},
    {Command::select_text, 0x17, activation_modifiers, 0, 'T'},
    {Command::repeat_action, 0x09, activation_modifiers, 0, 'F'},
    {Command::free_pointer, 0x13, activation_modifiers, 0, 'P'},
    {Command::window_activate, 0x1a, activation_modifiers, 0, 'W'},
    {Command::type_text, 0x1c, activation_modifiers, 0, 'Y'},
    {Command::scroll_up, 0x0c, activation_modifiers, 0, 'I'},
    {Command::scroll_down, 0x0e, activation_modifiers, 0, 'K'},
    {Command::scroll_left, 0x0d, activation_modifiers, 0, 'J'},
    {Command::scroll_right, 0x0f, activation_modifiers, 0, 'L'},
    {Command::scroll_up_continuous, 0x0c, activation_shift_modifiers, 0, 'I'},
    {Command::scroll_down_continuous, 0x0e, activation_shift_modifiers, 0, 'K'},
    {Command::scroll_left_continuous, 0x0d, activation_shift_modifiers, 0, 'J'},
    {Command::scroll_right_continuous, 0x0f, activation_shift_modifiers, 0, 'L'},
    {Command::suspend_toggle, 0x45, activation_modifiers, hotkey_always_active, 0},
}};

constexpr std::array<HotkeyBinding, 37> default_session_bindings{{
    {Command::window_cycle_forward, 0x08, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'E'},
    {Command::window_cycle_backward, 0x14, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'Q'},
    {Command::window_activate_behind, 0x1d, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'Z'},
    {Command::window_activate_left, 0x04, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'A'},
    {Command::window_activate_right, 0x07, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'D'},
    {Command::window_activate_up, 0x1a, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'W'},
    {Command::window_activate_down, 0x16, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 'S'},
    {Command::mode_single, 0x34, SACCADE_INPUT_MODIFIER_ALT, hotkey_session_only, '\''},
    {Command::mode_dual, 0x34, 0, hotkey_session_only, '\''},
    {Command::mode_multi, 0x34, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, '\''},
    {Command::mode_path, 0x34, SACCADE_INPUT_MODIFIER_CONTROL, hotkey_session_only, '\''},
    {Command::source_pixel, 0x30, 0, hotkey_session_only, ']'},
    {Command::source_semantic, 0x2f, 0, hotkey_session_only, '['},
    {Command::source_grid, 0x33, 0, hotkey_session_only, ';'},
    {Command::source_fused, 0x31, 0, hotkey_session_only, '\\'},
    {Command::scope_toggle, 0x2b, 0, hotkey_session_only, 0},
    {Command::target_position_next, 0x2c, 0, hotkey_session_only, ' '},
    {Command::target_position_1, 0x1e, 0, hotkey_session_only, '1'},
    {Command::target_position_2, 0x1f, 0, hotkey_session_only, '2'},
    {Command::target_position_3, 0x20, 0, hotkey_session_only, '3'},
    {Command::target_position_4, 0x21, 0, hotkey_session_only, '4'},
    {Command::target_position_5, 0x22, 0, hotkey_session_only, '5'},
    {Command::target_position_6, 0x23, 0, hotkey_session_only, '6'},
    {Command::target_position_7, 0x24, 0, hotkey_session_only, '7'},
    {Command::target_position_8, 0x25, 0, hotkey_session_only, '8'},
    {Command::target_position_9, 0x26, 0, hotkey_session_only, '9'},
    {Command::edge_snap_left, 0x50, 0, hotkey_session_only, 0},
    {Command::edge_snap_right, 0x4f, 0, hotkey_session_only, 0},
    {Command::edge_snap_up, 0x52, 0, hotkey_session_only, 0},
    {Command::edge_snap_down, 0x51, 0, hotkey_session_only, 0},
    {Command::nudge_left, 0x50, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 0},
    {Command::nudge_right, 0x4f, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 0},
    {Command::nudge_up, 0x52, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 0},
    {Command::nudge_down, 0x51, SACCADE_INPUT_MODIFIER_SHIFT, hotkey_session_only, 0},
    {Command::confirm, 0x28, 0, hotkey_session_only, 0},
    {Command::backspace, 0x2a, 0, hotkey_session_only, 0},
    {Command::cancel, 0x29, 0, hotkey_session_only, 0},
}};

static_assert(default_global_bindings.size() + default_session_bindings.size() <= maximum_hotkey_bindings);

struct OverlayPalette {
    uint32_t target_outline_rgba8;
    uint32_t label_background_rgba8;
    uint32_t label_foreground_rgba8;
    uint32_t active_fill_rgba8;
    uint32_t active_outline_rgba8;
};

constexpr OverlayPalette dark_palette{0x000000ff, 0x1b1b1bd9, 0xffffffff, 0x00000080, 0xffffffff};
constexpr OverlayPalette light_palette{0x202020e6, 0xfffffff2, 0x181818ff, 0x006bd666, 0x006bd6ff};
constexpr OverlayPalette high_contrast_palette{0xffff00ff, 0x000000ff, 0xffffffff, 0xffff004d, 0xffff00ff};

uint16_t q8_to_q3(uint32_t value) noexcept {
    return static_cast<uint16_t>((value + 16U) >> 5U);
}

OverlayPalette overlay_palette(const AppearanceSettings& appearance, bool dark_system_theme) noexcept {
    switch (appearance.theme) {
    case Theme::system:
        return dark_system_theme ? dark_palette : light_palette;
    case Theme::high_contrast:
        return high_contrast_palette;
    case Theme::light:
        return light_palette;
    case Theme::dark:
        return dark_palette;
    case Theme::custom:
        return {appearance.outline_rgba, appearance.background_rgba, appearance.label_rgba, appearance.glow_rgba,
                appearance.label_rgba};
    }
    return dark_palette;
}

template <typename Enum> bool enum_between(Enum value, Enum first, Enum last) noexcept {
    return value >= first && value <= last;
}

template <size_t Size> bool text_valid(const std::array<char, Size>& text, bool allow_empty) noexcept {
    size_t end = 0;
    while (end != text.size() && text[end] != '\0')
        ++end;
    if (end == text.size() || (!allow_empty && end == 0)) return false;
    for (size_t index = end + 1U; index < text.size(); ++index) {
        if (text[index] != '\0') return false;
    }
    return true;
}

uint16_t canonical_symbol(uint16_t symbol) noexcept {
    return symbol >= 'a' && symbol <= 'z' ? static_cast<uint16_t>(symbol - ('a' - 'A')) : symbol;
}

bool language_tag_valid(const std::array<char, language_tag_capacity>& language) noexcept {
    if (!text_valid(language, false)) return false;

    bool segment_start = true;
    for (char value : language) {
        if (value == '\0') break;
        const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (value == '-') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (!alpha && !digit) return false;
        segment_start = false;
    }
    return !segment_start;
}

bool hint_key_valid(uint32_t physical_key) noexcept {
    return (physical_key >= 0x04 && physical_key <= 0x27) || (physical_key >= 0x2c && physical_key <= 0x38);
}

uint32_t us_physical_key(uint16_t symbol) noexcept {
    const uint16_t upper = canonical_symbol(symbol);
    if (upper >= 'A' && upper <= 'Z') return 0x04U + upper - 'A';
    if (symbol >= '1' && symbol <= '9') return 0x1eU + symbol - '1';
    if (symbol == '0') return 0x27;

    switch (symbol) {
    case ' ':
        return 0x2c;
    case '-':
    case '_':
        return 0x2d;
    case '=':
    case '+':
        return 0x2e;
    case '[':
    case '{':
        return 0x2f;
    case ']':
    case '}':
        return 0x30;
    case '\\':
    case '|':
        return 0x31;
    case ';':
    case ':':
        return 0x33;
    case '\'':
    case '"':
        return 0x34;
    case '`':
    case '~':
        return 0x35;
    case ',':
    case '<':
        return 0x36;
    case '.':
    case '>':
        return 0x37;
    case '/':
    case '?':
        return 0x38;
    default:
        return 0;
    }
}

bool key_used(const HintSettings& hints, uint32_t count, uint32_t physical_key) noexcept {
    for (uint32_t index = 0; index < count; ++index) {
        if (hints.physical_keys[index] == physical_key) return true;
    }
    return false;
}

void migrate_hint_keys(HintSettings* hints) noexcept {
    for (uint32_t index = 0; index < hints->alphabet_count; ++index) {
        uint32_t physical_key = us_physical_key(hints->alphabet[index]);
        if (!hint_key_valid(physical_key) || key_used(*hints, index, physical_key)) {
            physical_key = 0;
            for (uint32_t candidate = 0x04; candidate <= 0x27; ++candidate) {
                if (!key_used(*hints, index, candidate)) {
                    physical_key = candidate;
                    break;
                }
            }
        }
        hints->physical_keys[index] = physical_key;
    }
}

bool binding_valid(const HotkeyBinding& binding) noexcept {
    const bool printable_key = (binding.physical_key >= 0x04 && binding.physical_key <= 0x27) ||
                               (binding.physical_key >= 0x2c && binding.physical_key <= 0x31) ||
                               (binding.physical_key >= 0x33 && binding.physical_key <= 0x38);
    const bool valid_symbol = binding.logical_symbol < 0xd800 || binding.logical_symbol > 0xdfff;
    return enum_between(binding.command, Command::pointer_move, last_command) &&
           binding.physical_key >= hid_keyboard_first && binding.physical_key <= hid_keyboard_last &&
           (binding.modifiers & ~modifier_mask) == 0 && (binding.flags & ~binding_flag_mask) == 0 && valid_symbol &&
           (binding.flags & (hotkey_always_active | hotkey_session_only)) !=
               (hotkey_always_active | hotkey_session_only) &&
           (!printable_key || binding.logical_symbol != 0);
}

bool hints_valid(const HintSettings& hints) noexcept {
    if (hints.alphabet_count < 2 || hints.alphabet_count > hints.alphabet.size() ||
        !enum_between(hints.priority, interaction::HintPriority::scene_order, interaction::HintPriority::randomized) ||
        !enum_between(hints.placement, HintPlacement::automatic, HintPlacement::right) ||
        !enum_between(hints.sorting, HintSorting::sorted, HintSorting::randomized) ||
        !language_tag_valid(hints.language))
        return false;
    for (uint32_t index = 0; index < hints.alphabet_count; ++index) {
        if (hints.alphabet[index] == 0 || (hints.alphabet[index] >= 0xd800 && hints.alphabet[index] <= 0xdfff) ||
            !hint_key_valid(hints.physical_keys[index])) {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (canonical_symbol(hints.alphabet[previous]) == canonical_symbol(hints.alphabet[index]) ||
                hints.physical_keys[previous] == hints.physical_keys[index]) {
                return false;
            }
        }
    }
    for (uint32_t index = hints.alphabet_count; index < hints.alphabet.size(); ++index) {
        if (hints.alphabet[index] != 0 || hints.physical_keys[index] != 0) return false;
    }
    return true;
}

struct Writer {
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
    size_t size_ = 0;
    bool failed_ = false;

    void bytes(const void* source, size_t size) noexcept {
        if (failed_ || size > capacity_ - size_) {
            failed_ = true;
            return;
        }
        std::memcpy(data_ + size_, source, size);
        size_ += size;
    }

    void u16(uint16_t value) noexcept {
        const uint8_t bytes_[2]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U)};
        bytes(bytes_, sizeof(bytes_));
    }

    void u32(uint32_t value) noexcept {
        const uint8_t bytes_[4]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U),
                                static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U)};
        bytes(bytes_, sizeof(bytes_));
    }

    void u64(uint64_t value) noexcept {
        u32(static_cast<uint32_t>(value));
        u32(static_cast<uint32_t>(value >> 32U));
    }
};

struct Reader {
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t position_ = 0;
    bool failed_ = false;

    void bytes(void* destination, size_t size) noexcept {
        if (failed_ || size > size_ - position_) {
            failed_ = true;
            return;
        }
        std::memcpy(destination, data_ + position_, size);
        position_ += size;
    }

    uint16_t u16() noexcept {
        uint8_t value[2]{};
        bytes(value, sizeof(value));
        return static_cast<uint16_t>(static_cast<uint16_t>(value[0]) | static_cast<uint16_t>(value[1]) << 8U);
    }

    uint32_t u32() noexcept {
        uint8_t value[4]{};
        bytes(value, sizeof(value));
        return static_cast<uint32_t>(value[0]) | static_cast<uint32_t>(value[1]) << 8U |
               static_cast<uint32_t>(value[2]) << 16U | static_cast<uint32_t>(value[3]) << 24U;
    }

    uint64_t u64() noexcept {
        const uint64_t low = u32();
        return low | static_cast<uint64_t>(u32()) << 32U;
    }
};

void encode_body(Writer& writer, const SettingsDocument& settings) noexcept {
    writer.u32(settings.flags);
    writer.u32(settings.binding_count);
    writer.u32(static_cast<uint32_t>(settings.source));
    writer.u32(static_cast<uint32_t>(settings.scope));
    writer.u64(settings.monitor_stable_id);
    for (uint32_t index = 0; index < settings.binding_count; ++index) {
        const HotkeyBinding& binding = settings.bindings[index];
        writer.u32(static_cast<uint32_t>(binding.command));
        writer.u32(binding.physical_key);
        writer.u32(binding.modifiers);
        writer.u32(binding.flags);
        writer.u16(binding.logical_symbol);
    }
    writer.u32(settings.hints.alphabet_count);
    for (uint16_t symbol : settings.hints.alphabet)
        writer.u16(symbol);
    for (uint32_t physical_key : settings.hints.physical_keys)
        writer.u32(physical_key);
    writer.bytes(settings.hints.language.data(), settings.hints.language.size());
    writer.u32(static_cast<uint32_t>(settings.hints.priority));
    writer.u32(static_cast<uint32_t>(settings.hints.placement));
    writer.u32(static_cast<uint32_t>(settings.hints.sorting));
    writer.u16(settings.detector.confidence_q16);
    writer.u16(settings.detector.text_sensitivity_q16);
    writer.u16(settings.detector.duplicate_iou_q16);
    writer.u16(settings.detector.minimum_width_q8);
    writer.u16(settings.detector.minimum_height_q8);
    writer.u16(0);
    writer.u32(static_cast<uint32_t>(settings.detector.merge_policy));
    writer.u16(settings.grid.rows);
    writer.u16(settings.grid.columns);
    writer.u16(settings.grid.margin_x_q8);
    writer.u16(settings.grid.margin_y_q8);
    writer.u32(static_cast<uint32_t>(settings.pointer.final_position));
    writer.u32(settings.pointer.movement_duration_ms);
    writer.u32(static_cast<uint32_t>(settings.pointer.anchor_x_q8));
    writer.u32(static_cast<uint32_t>(settings.pointer.anchor_y_q8));
    writer.u32(static_cast<uint32_t>(settings.actions.initial_mode));
    writer.u32(settings.actions.timeout_ms);
    writer.u32(settings.actions.hold_duration_ms);
    writer.u32(settings.actions.drag_duration_ms);
    writer.u32(settings.actions.scroll_duration_ms);
    writer.u32(static_cast<uint32_t>(settings.actions.scroll_vertical_q8));
    writer.u32(static_cast<uint32_t>(settings.actions.scroll_horizontal_q8));
    writer.u32(settings.actions.click_modifiers);
    writer.bytes(settings.appearance.font_family.data(), settings.appearance.font_family.size());
    writer.u32(static_cast<uint32_t>(settings.appearance.theme));
    writer.u32(static_cast<uint32_t>(settings.appearance.placement));
    writer.u32(settings.appearance.font_size_q8);
    writer.u32(settings.appearance.font_weight);
    writer.u32(settings.appearance.label_rgba);
    writer.u32(settings.appearance.background_rgba);
    writer.u32(settings.appearance.outline_rgba);
    writer.u32(settings.appearance.glow_rgba);
    writer.u16(settings.appearance.outline_width_q8);
    writer.u16(settings.appearance.glow_radius_q8);
    writer.u32(static_cast<uint32_t>(settings.compute.policy));
    writer.u64(settings.compute.device_stable_id);
}

void decode_body(Reader& reader, uint32_t version, SettingsDocument* settings) noexcept {
    settings->flags = reader.u32();
    settings->binding_count = reader.u32();
    settings->source = static_cast<TargetSource>(reader.u32());
    settings->scope = static_cast<TargetScope>(reader.u32());
    settings->monitor_stable_id = reader.u64();
    if (settings->binding_count > settings->bindings.size()) {
        reader.failed_ = true;
        return;
    }
    for (uint32_t index = 0; index < settings->binding_count; ++index) {
        HotkeyBinding& binding = settings->bindings[index];
        binding.command = static_cast<Command>(reader.u32());
        binding.physical_key = reader.u32();
        binding.modifiers = reader.u32();
        const uint32_t flags = reader.u32();
        if (flags > UINT16_MAX) reader.failed_ = true;
        binding.flags = static_cast<uint16_t>(flags);
        binding.logical_symbol = reader.u16();
    }
    settings->hints.alphabet_count = reader.u32();
    for (uint16_t& symbol : settings->hints.alphabet)
        symbol = reader.u16();
    if (settings->hints.alphabet_count > settings->hints.alphabet.size()) {
        reader.failed_ = true;
        return;
    }
    if (version >= 3) {
        for (uint32_t& physical_key : settings->hints.physical_keys)
            physical_key = reader.u32();
    } else {
        migrate_hint_keys(&settings->hints);
    }
    reader.bytes(settings->hints.language.data(), settings->hints.language.size());
    settings->hints.priority = static_cast<interaction::HintPriority>(reader.u32());
    settings->hints.placement = static_cast<HintPlacement>(reader.u32());
    settings->hints.sorting = static_cast<HintSorting>(reader.u32());
    settings->detector.confidence_q16 = reader.u16();
    settings->detector.text_sensitivity_q16 = reader.u16();
    settings->detector.duplicate_iou_q16 = reader.u16();
    settings->detector.minimum_width_q8 = reader.u16();
    settings->detector.minimum_height_q8 = reader.u16();
    if (reader.u16() != 0) reader.failed_ = true;
    settings->detector.merge_policy = static_cast<MergePolicy>(reader.u32());
    settings->grid.rows = reader.u16();
    settings->grid.columns = reader.u16();
    settings->grid.margin_x_q8 = reader.u16();
    settings->grid.margin_y_q8 = reader.u16();
    settings->pointer.final_position = static_cast<FinalPointerPosition>(reader.u32());
    settings->pointer.movement_duration_ms = reader.u32();
    settings->pointer.anchor_x_q8 = static_cast<int32_t>(reader.u32());
    settings->pointer.anchor_y_q8 = static_cast<int32_t>(reader.u32());
    settings->actions.initial_mode = static_cast<interaction::SelectionMode>(reader.u32());
    settings->actions.timeout_ms = reader.u32();
    settings->actions.hold_duration_ms = reader.u32();
    settings->actions.drag_duration_ms = reader.u32();
    settings->actions.scroll_duration_ms = reader.u32();
    settings->actions.scroll_vertical_q8 = static_cast<int32_t>(reader.u32());
    settings->actions.scroll_horizontal_q8 = static_cast<int32_t>(reader.u32());
    settings->actions.click_modifiers = reader.u32();
    reader.bytes(settings->appearance.font_family.data(), settings->appearance.font_family.size());
    settings->appearance.theme = static_cast<Theme>(reader.u32());
    settings->appearance.placement = static_cast<HintPlacement>(reader.u32());
    settings->appearance.font_size_q8 = reader.u32();
    settings->appearance.font_weight = reader.u32();
    settings->appearance.label_rgba = reader.u32();
    settings->appearance.background_rgba = reader.u32();
    settings->appearance.outline_rgba = reader.u32();
    settings->appearance.glow_rgba = reader.u32();
    settings->appearance.outline_width_q8 = reader.u16();
    settings->appearance.glow_radius_q8 = reader.u16();
    settings->compute.policy = static_cast<ComputePolicy>(reader.u32());
    settings->compute.device_stable_id = reader.u64();
}

} // namespace

SettingsDocument default_settings() noexcept {
    SettingsDocument settings{};
    constexpr uint16_t alphabet[] = {'A', 'S', 'D', 'F', 'J', 'K', 'L', 'G', 'H', 'E'};
    constexpr uint32_t physical_keys[] = {0x04, 0x16, 0x07, 0x09, 0x0d, 0x0e, 0x0f, 0x0a, 0x0b, 0x08};
    for (uint32_t index = 0; index < std::size(alphabet); ++index) {
        settings.hints.alphabet[index] = alphabet[index];
        settings.hints.physical_keys[index] = physical_keys[index];
    }
    settings.hints.alphabet_count = static_cast<uint32_t>(std::size(alphabet));
    constexpr char language[] = "und";
    constexpr char font[] = "system-ui";
    std::memcpy(settings.hints.language.data(), language, sizeof(language));
    std::memcpy(settings.appearance.font_family.data(), font, sizeof(font));
    std::copy(default_global_bindings.begin(), default_global_bindings.end(), settings.bindings.begin());
    std::copy(default_session_bindings.begin(), default_session_bindings.end(),
              settings.bindings.begin() + default_global_bindings.size());
    settings.binding_count = static_cast<uint32_t>(default_global_bindings.size() + default_session_bindings.size());
    return settings;
}

SaccadeOverlayStyle resolve_overlay_style(const AppearanceSettings& appearance, uint32_t settings_flags,
                                          bool dark_system_theme) noexcept {
    const OverlayPalette palette = overlay_palette(appearance, dark_system_theme);
    const uint16_t glyph_height_q3 = q8_to_q3(appearance.font_size_q8);
    const uint16_t glyph_width_q3 = static_cast<uint16_t>((static_cast<uint32_t>(glyph_height_q3) * 5U + 3U) / 7U);
    const uint16_t spacing_q3 = std::max<uint16_t>(8, static_cast<uint16_t>(glyph_height_q3 / 7U));
    const uint16_t padding_q3 = std::max<uint16_t>(16, static_cast<uint16_t>(glyph_height_q3 / 7U));
    const uint16_t stroke_q3 = std::max<uint16_t>(1, q8_to_q3(appearance.outline_width_q8));
    const uint16_t glow_q3 = q8_to_q3(appearance.glow_radius_q8);

    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = palette.target_outline_rgba8;
    style.label_background_rgba8 = palette.label_background_rgba8;
    style.label_foreground_rgba8 = palette.label_foreground_rgba8;
    style.active_fill_rgba8 = palette.active_fill_rgba8;
    style.active_outline_rgba8 = palette.active_outline_rgba8;
    style.target_stroke_q3 = stroke_q3;
    style.target_radius_q3 = std::max<uint16_t>(16, static_cast<uint16_t>(glyph_height_q3 / 4U));
    style.label_height_q3 = static_cast<uint16_t>(glyph_height_q3 + padding_q3 * 2U);
    style.label_radius_q3 = padding_q3;
    style.label_padding_x_q3 = padding_q3;
    style.glyph_width_q3 = glyph_width_q3;
    style.glyph_height_q3 = glyph_height_q3;
    style.glyph_advance_q3 = static_cast<uint16_t>(glyph_width_q3 + spacing_q3);
    style.active_stroke_q3 = static_cast<uint16_t>(stroke_q3 + glow_q3);
    if ((settings_flags & settings_animate_overlay) != 0 && (settings_flags & settings_reduced_motion) == 0)
        style.flags = SACCADE_OVERLAY_STYLE_ANIMATED;
    return style;
}

InteractionProfile make_interaction_profile(const SettingsDocument& settings, const HintSettings& resolved_hints,
                                            int32_t pointer_x_q8, int32_t pointer_y_q8, int32_t scope_center_x_q8,
                                            int32_t scope_center_y_q8, uint64_t random_seed) noexcept {
    constexpr uint64_t nanoseconds_per_millisecond = UINT64_C(1'000'000);
    InteractionProfile profile{};
    profile.hints.alphabet = resolved_hints.alphabet;
    profile.hints.physical_keys = resolved_hints.physical_keys;
    profile.hints.alphabet_count = resolved_hints.alphabet_count;
    profile.hints.priority = settings.hints.sorting == HintSorting::randomized ? interaction::HintPriority::randomized
                                                                               : settings.hints.priority;
    profile.hints.pointer_x_q8 = pointer_x_q8;
    profile.hints.pointer_y_q8 = pointer_y_q8;
    profile.hints.scope_center_x_q8 = scope_center_x_q8;
    profile.hints.scope_center_y_q8 = scope_center_y_q8;
    profile.hints.random_seed = random_seed;
    profile.timeout_ns = settings.actions.timeout_ms * nanoseconds_per_millisecond;
    profile.hold_duration_ns = settings.actions.hold_duration_ms * nanoseconds_per_millisecond;
    profile.drag_duration_ns = settings.actions.drag_duration_ms * nanoseconds_per_millisecond;
    profile.scroll_duration_ns = settings.actions.scroll_duration_ms * nanoseconds_per_millisecond;
    profile.scroll_vertical_q8 = settings.actions.scroll_vertical_q8;
    profile.scroll_horizontal_q8 = settings.actions.scroll_horizontal_q8;
    profile.click_modifiers = settings.actions.click_modifiers;
    profile.initial_mode = settings.actions.initial_mode;
    profile.final_pointer = static_cast<PointerFinalPosition>(settings.pointer.final_position);
    profile.pointer_duration_ms = settings.pointer.movement_duration_ms;
    profile.pointer_anchor = {settings.pointer.anchor_x_q8, settings.pointer.anchor_y_q8};
    return profile;
}

SaccadeResult resolve_hint_alphabet(const HintSettings& input, const uint16_t* translated_symbols,
                                    HintSettings* output) noexcept {
    if (translated_symbols == nullptr || output == nullptr || !hints_valid(input)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::array<bool, interaction::maximum_hint_alphabet> translated{};
    *output = input;
    for (uint32_t index = 0; index < input.alphabet_count; ++index) {
        const uint16_t symbol = translated_symbols[index];
        translated[index] = symbol != 0 && (symbol < 0xd800 || symbol > 0xdfff);
        output->alphabet[index] = translated[index] ? symbol : input.alphabet[index];
    }

    for (;;) {
        bool collision = false;
        for (uint32_t index = 0; index < input.alphabet_count && !collision; ++index) {
            for (uint32_t previous = 0; previous < index; ++previous) {
                if (canonical_symbol(output->alphabet[previous]) != canonical_symbol(output->alphabet[index])) {
                    continue;
                }

                if (!translated[previous] && !translated[index]) return SACCADE_ERROR_ALREADY_EXISTS;
                const uint32_t fallback = translated[index] ? index : previous;
                translated[fallback] = false;
                output->alphabet[fallback] = input.alphabet[fallback];
                collision = true;
                break;
            }
        }
        if (!collision) break;
    }

    return hints_valid(*output) ? SACCADE_OK : SACCADE_ERROR_INVALID_ARGUMENT;
}

SaccadeResult set_hint_alphabet(HintSettings* hints, const uint16_t* symbols, uint32_t symbol_count) noexcept {
    if (hints == nullptr || symbols == nullptr || symbol_count < 2 || symbol_count > hints->alphabet.size()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    HintSettings updated = *hints;
    updated.alphabet.fill(0);
    updated.physical_keys.fill(0);
    updated.alphabet_count = symbol_count;
    std::copy_n(symbols, symbol_count, updated.alphabet.begin());
    migrate_hint_keys(&updated);
    if (!hints_valid(updated)) return SACCADE_ERROR_INVALID_ARGUMENT;
    *hints = updated;
    return SACCADE_OK;
}

SaccadeResult validate_settings(const SettingsDocument& settings) noexcept {
    if (settings.binding_count > settings.bindings.size() || (settings.flags & ~settings_flag_mask) != 0 ||
        !enum_between(settings.source, TargetSource::pixel, TargetSource::fused) ||
        !enum_between(settings.scope, TargetScope::desktop, TargetScope::monitor) ||
        (settings.scope == TargetScope::monitor && settings.monitor_stable_id == 0) || !hints_valid(settings.hints))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < settings.binding_count; ++index) {
        if (!binding_valid(settings.bindings[index])) return SACCADE_ERROR_INVALID_ARGUMENT;
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (settings.bindings[previous].physical_key == settings.bindings[index].physical_key &&
                settings.bindings[previous].modifiers == settings.bindings[index].modifiers) {
                return SACCADE_ERROR_ALREADY_EXISTS;
            }
        }
    }
    for (uint32_t index = settings.binding_count; index < settings.bindings.size(); ++index) {
        const HotkeyBinding& binding = settings.bindings[index];
        if (binding.physical_key != 0 || binding.modifiers != 0 || binding.flags != 0 || binding.logical_symbol != 0)
            return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const DetectorSettings& detector = settings.detector;
    if (detector.confidence_q16 == 0 || detector.text_sensitivity_q16 == 0 || detector.duplicate_iou_q16 == 0 ||
        detector.minimum_width_q8 == 0 || detector.minimum_height_q8 == 0 ||
        !enum_between(detector.merge_policy, MergePolicy::balanced, MergePolicy::disabled) || settings.grid.rows == 0 ||
        settings.grid.columns == 0 ||
        static_cast<uint32_t>(settings.grid.rows) * settings.grid.columns > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        !enum_between(settings.pointer.final_position, FinalPointerPosition::target, FinalPointerPosition::anchor) ||
        settings.pointer.movement_duration_ms > 10'000 ||
        !enum_between(settings.actions.initial_mode, interaction::SelectionMode::single,
                      interaction::SelectionMode::path) ||
        settings.actions.timeout_ms < 100 || settings.actions.timeout_ms > 60'000 ||
        settings.actions.hold_duration_ms > 60'000 || settings.actions.drag_duration_ms > 60'000 ||
        settings.actions.scroll_duration_ms > 60'000 || settings.actions.scroll_vertical_q8 == 0 ||
        settings.actions.scroll_vertical_q8 == INT32_MIN || settings.actions.scroll_horizontal_q8 == 0 ||
        settings.actions.scroll_horizontal_q8 == INT32_MIN ||
        (settings.actions.click_modifiers & ~modifier_mask) != 0 ||
        !text_valid(settings.appearance.font_family, false) ||
        !enum_between(settings.appearance.theme, Theme::system, Theme::custom) ||
        !enum_between(settings.appearance.placement, HintPlacement::automatic, HintPlacement::right) ||
        settings.appearance.font_size_q8 == 0 || settings.appearance.font_size_q8 > 128U * 256U ||
        settings.appearance.font_weight < 100 || settings.appearance.font_weight > 1000 ||
        !enum_between(settings.compute.policy, ComputePolicy::automatic, ComputePolicy::named_device) ||
        (settings.compute.policy == ComputePolicy::named_device) != (settings.compute.device_stable_id != 0)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

SaccadeResult reset_settings_page(SettingsPage page, SettingsDocument* settings) noexcept {
    if (settings == nullptr || !enum_between(page, SettingsPage::bindings, SettingsPage::compute)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const SettingsDocument defaults = default_settings();
    switch (page) {
    case SettingsPage::bindings:
        settings->bindings = defaults.bindings;
        settings->binding_count = defaults.binding_count;
        break;
    case SettingsPage::hints:
        settings->hints = defaults.hints;
        break;
    case SettingsPage::detector:
        settings->detector = defaults.detector;
        settings->grid = defaults.grid;
        settings->source = defaults.source;
        break;
    case SettingsPage::scope:
        settings->scope = defaults.scope;
        settings->monitor_stable_id = defaults.monitor_stable_id;
        break;
    case SettingsPage::pointer:
        settings->pointer = defaults.pointer;
        settings->actions = defaults.actions;
        break;
    case SettingsPage::appearance:
        settings->appearance = defaults.appearance;
        settings->flags = defaults.flags;
        break;
    case SettingsPage::compute:
        settings->compute = defaults.compute;
        break;
    }
    return validate_settings(*settings);
}

SaccadeResult encode_settings(const SettingsDocument& settings, SaccadeMutableSpanU8 output,
                              size_t* output_size) noexcept {
    if (output_size == nullptr || output.data == nullptr || output.size > settings_encoded_capacity)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output_size = 0;
    const SaccadeResult valid = validate_settings(settings);
    if (valid != SACCADE_OK) return valid;
    Writer writer{output.data, output.size};
    writer.bytes(settings_magic, sizeof(settings_magic));
    writer.u32(settings_version);
    writer.u32(0);
    encode_body(writer, settings);
    if (writer.failed_) return SACCADE_ERROR_CAPACITY;
    const uint32_t total = static_cast<uint32_t>(writer.size_);
    output.data[8] = static_cast<uint8_t>(total);
    output.data[9] = static_cast<uint8_t>(total >> 8U);
    output.data[10] = static_cast<uint8_t>(total >> 16U);
    output.data[11] = static_cast<uint8_t>(total >> 24U);
    *output_size = writer.size_;
    return SACCADE_OK;
}

SaccadeResult decode_settings(SaccadeSpanU8 input, SettingsDocument* output) noexcept {
    if (output == nullptr || input.data == nullptr || input.size < 12 || input.size > settings_encoded_capacity)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    Reader reader{input.data, input.size};
    uint8_t magic[4]{};
    reader.bytes(magic, sizeof(magic));
    const uint32_t version = reader.u32();
    const uint32_t total = reader.u32();
    if (std::memcmp(magic, settings_magic, sizeof(magic)) != 0 || version < minimum_settings_version ||
        version > settings_version || total != input.size) {
        return SACCADE_ERROR_VERSION;
    }
    SettingsDocument decoded{};
    decode_body(reader, version, &decoded);
    if (reader.failed_ || reader.position_ != input.size) return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult valid = validate_settings(decoded);
    if (valid != SACCADE_OK) return valid;
    *output = decoded;
    return SACCADE_OK;
}

} // namespace saccade::application

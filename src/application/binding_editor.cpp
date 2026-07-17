#include "application/binding_editor.hpp"

#include <algorithm>

namespace saccade::application {
namespace {

constexpr std::array<const char*, binding_command_count> command_names{"Pointer move",
                                                                       "Hover",
                                                                       "Left click",
                                                                       "Right click",
                                                                       "Middle click",
                                                                       "Double click",
                                                                       "Hold",
                                                                       "Drag and drop",
                                                                       "Vertical scroll",
                                                                       "Horizontal scroll",
                                                                       "Select text",
                                                                       "Repeat action",
                                                                       "Free pointer",
                                                                       "Window hints",
                                                                       "Next overlapping window",
                                                                       "Previous overlapping window",
                                                                       "Window behind",
                                                                       "Window left",
                                                                       "Window right",
                                                                       "Window up",
                                                                       "Window down",
                                                                       "Single mode",
                                                                       "Dual mode",
                                                                       "Multi mode",
                                                                       "Path mode",
                                                                       "Pixel source",
                                                                       "Semantic source",
                                                                       "Grid source",
                                                                       "Desktop scope",
                                                                       "Active-window scope",
                                                                       "Monitor scope",
                                                                       "Next target position",
                                                                       "Snap left",
                                                                       "Snap right",
                                                                       "Snap up",
                                                                       "Snap down",
                                                                       "Nudge left",
                                                                       "Nudge right",
                                                                       "Nudge up",
                                                                       "Nudge down",
                                                                       "Confirm",
                                                                       "Backspace",
                                                                       "Cancel",
                                                                       "Suspend hotkeys",
                                                                       "Open settings",
                                                                       "Restart",
                                                                       "Quit",
                                                                       "Type clipboard text",
                                                                       "Fused source",
                                                                       "Target position 1",
                                                                       "Target position 2",
                                                                       "Target position 3",
                                                                       "Target position 4",
                                                                       "Target position 5",
                                                                       "Target position 6",
                                                                       "Target position 7",
                                                                       "Target position 8",
                                                                       "Target position 9",
                                                                       "Scroll up",
                                                                       "Scroll down",
                                                                       "Scroll left",
                                                                       "Scroll right",
                                                                       "Scroll up continuously",
                                                                       "Scroll down continuously",
                                                                       "Scroll left continuously",
                                                                       "Scroll right continuously",
                                                                       "Toggle desktop/window scope"};

constexpr std::array<BindingKey, binding_key_count> keys{
    {{0x04, "A"},     {0x05, "B"},      {0x06, "C"},         {0x07, "D"},   {0x08, "E"},     {0x09, "F"},
     {0x0a, "G"},     {0x0b, "H"},      {0x0c, "I"},         {0x0d, "J"},   {0x0e, "K"},     {0x0f, "L"},
     {0x10, "M"},     {0x11, "N"},      {0x12, "O"},         {0x13, "P"},   {0x14, "Q"},     {0x15, "R"},
     {0x16, "S"},     {0x17, "T"},      {0x18, "U"},         {0x19, "V"},   {0x1a, "W"},     {0x1b, "X"},
     {0x1c, "Y"},     {0x1d, "Z"},      {0x1e, "1"},         {0x1f, "2"},   {0x20, "3"},     {0x21, "4"},
     {0x22, "5"},     {0x23, "6"},      {0x24, "7"},         {0x25, "8"},   {0x26, "9"},     {0x27, "0"},
     {0x28, "Enter"}, {0x29, "Escape"}, {0x2a, "Backspace"}, {0x2b, "Tab"}, {0x2c, "Space"}, {0x2d, "-"},
     {0x2e, "="},     {0x2f, "["},      {0x30, "]"},         {0x31, "\\"},  {0x33, ";"},     {0x34, "'"},
     {0x35, "`"},     {0x36, ","},      {0x37, "."},         {0x38, "/"},   {0x3a, "F1"},    {0x3b, "F2"},
     {0x3c, "F3"},    {0x3d, "F4"},     {0x3e, "F5"},        {0x3f, "F6"},  {0x40, "F7"},    {0x41, "F8"},
     {0x42, "F9"},    {0x43, "F10"},    {0x44, "F11"},       {0x45, "F12"}, {0x4f, "Right"}, {0x50, "Left"},
     {0x51, "Down"},  {0x52, "Up"}}};

struct KeyPlacement {
    uint32_t usage;
    uint32_t x_units;
    uint32_t y_units;
    uint32_t width_units;
};

constexpr uint32_t keyboard_width_units = 32;
constexpr uint32_t keyboard_height_units = 6;

constexpr std::array<KeyPlacement, binding_key_count> key_placements{{
    {0x29, 0, 0, 2},  {0x3a, 4, 0, 2},  {0x3b, 6, 0, 2},  {0x3c, 8, 0, 2},  {0x3d, 10, 0, 2}, {0x3e, 13, 0, 2},
    {0x3f, 15, 0, 2}, {0x40, 17, 0, 2}, {0x41, 19, 0, 2}, {0x42, 22, 0, 2}, {0x43, 24, 0, 2}, {0x44, 26, 0, 2},
    {0x45, 28, 0, 2},

    {0x1e, 0, 1, 2},  {0x1f, 2, 1, 2},  {0x20, 4, 1, 2},  {0x21, 6, 1, 2},  {0x22, 8, 1, 2},  {0x23, 10, 1, 2},
    {0x24, 12, 1, 2}, {0x25, 14, 1, 2}, {0x26, 16, 1, 2}, {0x27, 18, 1, 2}, {0x2d, 20, 1, 2}, {0x2e, 22, 1, 2},
    {0x2a, 24, 1, 4},

    {0x2b, 0, 2, 3},  {0x14, 3, 2, 2},  {0x1a, 5, 2, 2},  {0x08, 7, 2, 2},  {0x15, 9, 2, 2},  {0x17, 11, 2, 2},
    {0x1c, 13, 2, 2}, {0x18, 15, 2, 2}, {0x0c, 17, 2, 2}, {0x12, 19, 2, 2}, {0x13, 21, 2, 2}, {0x2f, 23, 2, 2},
    {0x30, 25, 2, 2}, {0x31, 27, 2, 2},

    {0x04, 2, 3, 2},  {0x16, 4, 3, 2},  {0x07, 6, 3, 2},  {0x09, 8, 3, 2},  {0x0a, 10, 3, 2}, {0x0b, 12, 3, 2},
    {0x0d, 14, 3, 2}, {0x0e, 16, 3, 2}, {0x0f, 18, 3, 2}, {0x33, 20, 3, 2}, {0x34, 22, 3, 2}, {0x28, 24, 3, 4},

    {0x1d, 3, 4, 2},  {0x1b, 5, 4, 2},  {0x06, 7, 4, 2},  {0x19, 9, 4, 2},  {0x05, 11, 4, 2}, {0x11, 13, 4, 2},
    {0x10, 15, 4, 2}, {0x36, 17, 4, 2}, {0x37, 19, 4, 2}, {0x38, 21, 4, 2}, {0x52, 26, 4, 2},

    {0x35, 0, 5, 2},  {0x2c, 6, 5, 12}, {0x50, 24, 5, 2}, {0x51, 26, 5, 2}, {0x4f, 28, 5, 2},
}};

bool command_valid(Command command) noexcept {
    return command >= Command::pointer_move && command <= last_command;
}

} // namespace

const char* command_name(Command command) noexcept {
    return command_valid(command) ? command_names[static_cast<uint32_t>(command) - 1U] : nullptr;
}

const std::array<BindingKey, binding_key_count>& binding_keys() noexcept {
    return keys;
}

SaccadeResult layout_binding_keyboard(int32_t width, int32_t height, int32_t gap,
                                      BindingKeyboardLayout* output) noexcept {
    if (output == nullptr || width < static_cast<int32_t>(keyboard_width_units) ||
        height < static_cast<int32_t>(keyboard_height_units) || gap < 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *output = {};
    for (const KeyPlacement& placement : key_placements) {
        BindingKeyRect& key = output->keys[output->key_count++];
        const int32_t left =
            static_cast<int32_t>(placement.x_units * static_cast<uint32_t>(width) / keyboard_width_units);
        const int32_t right = static_cast<int32_t>((placement.x_units + placement.width_units) *
                                                   static_cast<uint32_t>(width) / keyboard_width_units);
        const int32_t top =
            static_cast<int32_t>(placement.y_units * static_cast<uint32_t>(height) / keyboard_height_units);
        const int32_t bottom =
            static_cast<int32_t>((placement.y_units + 1U) * static_cast<uint32_t>(height) / keyboard_height_units);
        key = {placement.usage, left, top, std::max<int32_t>(1, right - left - gap),
               std::max<int32_t>(1, bottom - top - gap)};
    }
    return SACCADE_OK;
}

uint16_t default_logical_symbol(uint32_t physical_key) noexcept {
    if (physical_key >= 0x04 && physical_key <= 0x1d) return static_cast<uint16_t>('A' + physical_key - 0x04);
    if (physical_key >= 0x1e && physical_key <= 0x26) return static_cast<uint16_t>('1' + physical_key - 0x1e);
    if (physical_key == 0x27) return static_cast<uint16_t>('0');
    switch (physical_key) {
    case 0x2c:
        return ' ';
    case 0x2d:
        return '-';
    case 0x2e:
        return '=';
    case 0x2f:
        return '[';
    case 0x30:
        return ']';
    case 0x31:
        return '\\';
    case 0x33:
        return ';';
    case 0x34:
        return '\'';
    case 0x35:
        return '`';
    case 0x36:
        return ',';
    case 0x37:
        return '.';
    case 0x38:
        return '/';
    default:
        return 0;
    }
}

SaccadeResult find_binding(const SettingsDocument& settings, Command command, uint32_t* index) noexcept {
    if (!command_valid(command) || index == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *index = UINT32_MAX;
    for (uint32_t candidate = 0; candidate < settings.binding_count; ++candidate) {
        if (settings.bindings[candidate].command == command) {
            *index = candidate;
            return SACCADE_OK;
        }
    }
    return SACCADE_ERROR_NOT_FOUND;
}

SaccadeResult set_binding(SettingsDocument* settings, HotkeyBinding binding, BindingConflict* conflict) noexcept {
    if (settings == nullptr || conflict == nullptr || !command_valid(binding.command) || binding.physical_key == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *conflict = {};
    conflict->index = UINT32_MAX;
    uint32_t replaced = UINT32_MAX;
    (void)find_binding(*settings, binding.command, &replaced);
    for (uint32_t index = 0; index < settings->binding_count; ++index) {
        if (index != replaced && settings->bindings[index].physical_key == binding.physical_key &&
            settings->bindings[index].modifiers == binding.modifiers) {
            conflict->command = settings->bindings[index].command;
            conflict->index = index;
            return SACCADE_ERROR_ALREADY_EXISTS;
        }
    }
    if (replaced == UINT32_MAX) {
        if (settings->binding_count == settings->bindings.size()) return SACCADE_ERROR_CAPACITY;
        replaced = settings->binding_count++;
    }
    settings->bindings[replaced] = binding;
    return validate_settings(*settings);
}

SaccadeResult remove_binding(SettingsDocument* settings, Command command) noexcept {
    if (settings == nullptr || !command_valid(command)) return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t index = UINT32_MAX;
    const SaccadeResult found = find_binding(*settings, command, &index);
    if (found != SACCADE_OK) return found;
    std::move(settings->bindings.begin() + index + 1U, settings->bindings.begin() + settings->binding_count,
              settings->bindings.begin() + index);
    --settings->binding_count;
    settings->bindings[settings->binding_count] = {};
    return validate_settings(*settings);
}

} // namespace saccade::application

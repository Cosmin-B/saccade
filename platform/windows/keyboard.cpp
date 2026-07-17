#include "platform/windows/keyboard.hpp"

#include "application/settings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstring>
#include <cwchar>

namespace saccade::platform::windows {
namespace {

constexpr uint32_t hid_keyboard_a = UINT32_C(0x04);
constexpr uint32_t hid_keyboard_z = UINT32_C(0x1d);
constexpr uint32_t hid_keyboard_1 = UINT32_C(0x1e);
constexpr uint32_t hid_keyboard_0 = UINT32_C(0x27);
constexpr uint32_t hid_keyboard_enter = UINT32_C(0x28);
constexpr uint32_t hid_keyboard_escape = UINT32_C(0x29);
constexpr uint32_t hid_keyboard_backspace = UINT32_C(0x2a);
constexpr uint32_t hid_keyboard_tab = UINT32_C(0x2b);
constexpr uint32_t hid_keyboard_space = UINT32_C(0x2c);
constexpr uint32_t hid_keyboard_f1 = UINT32_C(0x3a);
constexpr uint32_t hid_keyboard_f12 = UINT32_C(0x45);
constexpr uint32_t hid_keyboard_right = UINT32_C(0x4f);
constexpr uint32_t hid_keyboard_left = UINT32_C(0x50);
constexpr uint32_t hid_keyboard_down = UINT32_C(0x51);
constexpr uint32_t hid_keyboard_up = UINT32_C(0x52);
constexpr uint32_t hid_keyboard_left_control = UINT32_C(0xe0);
constexpr uint32_t hid_keyboard_left_shift = UINT32_C(0xe1);
constexpr uint32_t hid_keyboard_left_alt = UINT32_C(0xe2);
constexpr uint32_t hid_keyboard_left_gui = UINT32_C(0xe3);
constexpr uint32_t hid_keyboard_right_control = UINT32_C(0xe4);
constexpr uint32_t hid_keyboard_right_shift = UINT32_C(0xe5);
constexpr uint32_t hid_keyboard_right_alt = UINT32_C(0xe6);
constexpr uint32_t hid_keyboard_right_gui = UINT32_C(0xe7);
constexpr uint16_t scan_left_shift = UINT16_C(0x2a);
constexpr uint16_t scan_right_shift = UINT16_C(0x36);
constexpr uint16_t scan_control = UINT16_C(0x1d);
constexpr uint16_t scan_alt = UINT16_C(0x38);
constexpr uint16_t scan_left_gui = UINT16_C(0x5b);
constexpr uint16_t scan_right_gui = UINT16_C(0x5c);

constexpr std::array<uint8_t, 26> letter_scans{0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                               0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                               0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
constexpr std::array<uint8_t, 10> digit_scans{0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};

uint16_t symbol_from_layout(HKL layout, uint32_t usage) noexcept {
    KeyScan scan{};
    if (layout == nullptr || !scan_from_hid_usage(usage, &scan)) return 0;

    const UINT scan_code = scan.value | (scan.extended ? UINT32_C(0xe000) : 0);
    const UINT virtual_key = MapVirtualKeyExW(scan_code, MAPVK_VSC_TO_VK_EX, layout);
    if (virtual_key == 0 || virtual_key > UINT8_MAX) return 0;

    std::array<BYTE, 256> state{};
    std::array<wchar_t, 4> symbols{};
    constexpr UINT preserve_keyboard_state = 4;
    const int symbol_count = ToUnicodeEx(virtual_key, scan.value, state.data(), symbols.data(),
                                         static_cast<int>(symbols.size()), preserve_keyboard_state, layout);
    if (symbol_count != 1 || (symbols[0] >= 0xd800 && symbols[0] <= 0xdfff)) return 0;
    return static_cast<uint16_t>(symbols[0]);
}

size_t primary_language_size(const wchar_t* language) noexcept {
    const wchar_t* separator = std::wcschr(language, L'-');
    return separator == nullptr ? std::wcslen(language) : static_cast<size_t>(separator - language);
}

bool language_match(const wchar_t* requested, const wchar_t* candidate, bool exact) noexcept {
    const size_t requested_size = exact ? std::wcslen(requested) : primary_language_size(requested);
    const size_t candidate_size = exact ? std::wcslen(candidate) : primary_language_size(candidate);
    return requested_size == candidate_size &&
           CompareStringOrdinal(requested, static_cast<int>(requested_size), candidate,
                                static_cast<int>(candidate_size), TRUE) == CSTR_EQUAL;
}

HKL layout_for_language(const char* language) noexcept {
    if (std::strcmp(language, "und") == 0) return GetKeyboardLayout(0);

    std::array<wchar_t, application::language_tag_capacity> requested{};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, language, -1, requested.data(),
                            static_cast<int>(requested.size())) == 0) {
        return nullptr;
    }

    std::array<HKL, 64> layouts{};
    const int layout_count = GetKeyboardLayoutList(static_cast<int>(layouts.size()), layouts.data());
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const bool exact = pass == 0;
        for (int index = 0; index < layout_count; ++index) {
            const LANGID language_id = LOWORD(reinterpret_cast<ULONG_PTR>(layouts[index]));
            std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> candidate{};
            if (LCIDToLocaleName(MAKELCID(language_id, SORT_DEFAULT), candidate.data(),
                                 static_cast<int>(candidate.size()), 0) == 0 ||
                !language_match(requested.data(), candidate.data(), exact)) {
                continue;
            }
            return layouts[index];
        }
    }
    return nullptr;
}

constexpr std::array<uint8_t, 12> function_scans{0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
                                                 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};

bool same(KeyScan left, KeyScan right) noexcept {
    return left.value == right.value && left.extended == right.extended;
}

struct ScanUsage {
    KeyScan scan{};
    uint32_t usage = 0;
};

} // namespace

bool scan_from_hid_usage(uint32_t usage, KeyScan* output) noexcept {
    if (output == nullptr) return false;
    if (usage >= hid_keyboard_a && usage <= hid_keyboard_z) {
        *output = {letter_scans[usage - hid_keyboard_a], false};
        return true;
    }
    if (usage >= hid_keyboard_1 && usage <= hid_keyboard_0) {
        *output = {digit_scans[usage - hid_keyboard_1], false};
        return true;
    }
    if (usage >= hid_keyboard_f1 && usage <= hid_keyboard_f12) {
        *output = {function_scans[usage - hid_keyboard_f1], false};
        return true;
    }
    switch (usage) {
    case hid_keyboard_enter:
        *output = {0x1c, false};
        return true;
    case hid_keyboard_escape:
        *output = {0x01, false};
        return true;
    case hid_keyboard_backspace:
        *output = {0x0e, false};
        return true;
    case hid_keyboard_tab:
        *output = {0x0f, false};
        return true;
    case hid_keyboard_space:
        *output = {0x39, false};
        return true;
    case hid_keyboard_right:
        *output = {0x4d, true};
        return true;
    case hid_keyboard_left:
        *output = {0x4b, true};
        return true;
    case hid_keyboard_down:
        *output = {0x50, true};
        return true;
    case hid_keyboard_up:
        *output = {0x48, true};
        return true;
    case hid_keyboard_left_control:
        *output = {scan_control, false};
        return true;
    case hid_keyboard_left_shift:
        *output = {scan_left_shift, false};
        return true;
    case hid_keyboard_left_alt:
        *output = {scan_alt, false};
        return true;
    case hid_keyboard_left_gui:
        *output = {scan_left_gui, true};
        return true;
    case hid_keyboard_right_control:
        *output = {scan_control, true};
        return true;
    case hid_keyboard_right_shift:
        *output = {scan_right_shift, false};
        return true;
    case hid_keyboard_right_alt:
        *output = {scan_alt, true};
        return true;
    case hid_keyboard_right_gui:
        *output = {scan_right_gui, true};
        return true;
    default:
        return false;
    }
}

bool hid_usage_from_scan(KeyScan scan, uint32_t* output) noexcept {
    if (output == nullptr) return false;
    for (uint32_t index = 0; index < letter_scans.size(); ++index) {
        if (same(scan, {letter_scans[index], false})) {
            *output = hid_keyboard_a + index;
            return true;
        }
    }
    for (uint32_t index = 0; index < digit_scans.size(); ++index) {
        if (same(scan, {digit_scans[index], false})) {
            *output = hid_keyboard_1 + index;
            return true;
        }
    }
    for (uint32_t index = 0; index < function_scans.size(); ++index) {
        if (same(scan, {function_scans[index], false})) {
            *output = hid_keyboard_f1 + index;
            return true;
        }
    }
    constexpr std::array<ScanUsage, 17> remaining{{{{0x1c, false}, hid_keyboard_enter},
                                                   {{0x01, false}, hid_keyboard_escape},
                                                   {{0x0e, false}, hid_keyboard_backspace},
                                                   {{0x0f, false}, hid_keyboard_tab},
                                                   {{0x39, false}, hid_keyboard_space},
                                                   {{0x4d, true}, hid_keyboard_right},
                                                   {{0x4b, true}, hid_keyboard_left},
                                                   {{0x50, true}, hid_keyboard_down},
                                                   {{0x48, true}, hid_keyboard_up},
                                                   {{scan_control, false}, hid_keyboard_left_control},
                                                   {{scan_left_shift, false}, hid_keyboard_left_shift},
                                                   {{scan_alt, false}, hid_keyboard_left_alt},
                                                   {{scan_left_gui, true}, hid_keyboard_left_gui},
                                                   {{scan_control, true}, hid_keyboard_right_control},
                                                   {{scan_right_shift, false}, hid_keyboard_right_shift},
                                                   {{scan_alt, true}, hid_keyboard_right_alt},
                                                   {{scan_right_gui, true}, hid_keyboard_right_gui}}};
    for (const auto& value : remaining) {
        if (same(scan, value.scan)) {
            *output = value.usage;
            return true;
        }
    }
    return false;
}

bool scan_from_modifier(uint32_t modifier, KeyScan* output) noexcept {
    if (output == nullptr) return false;
    if (modifier == SACCADE_INPUT_MODIFIER_SHIFT)
        *output = {scan_left_shift, false};
    else if (modifier == SACCADE_INPUT_MODIFIER_CONTROL)
        *output = {scan_control, false};
    else if (modifier == SACCADE_INPUT_MODIFIER_ALT)
        *output = {scan_alt, false};
    else if (modifier == SACCADE_INPUT_MODIFIER_META)
        *output = {scan_left_gui, true};
    else
        return false;
    return true;
}

uint32_t modifier_from_scan(KeyScan scan) noexcept {
    if (scan.value == scan_left_shift || scan.value == scan_right_shift) return SACCADE_INPUT_MODIFIER_SHIFT;
    if (scan.value == scan_control) return SACCADE_INPUT_MODIFIER_CONTROL;
    if (scan.value == scan_alt) return SACCADE_INPUT_MODIFIER_ALT;
    return (scan.value == scan_left_gui || scan.value == scan_right_gui) && scan.extended ? SACCADE_INPUT_MODIFIER_META
                                                                                          : 0;
}

uint16_t logical_symbol_from_hid_usage(uint32_t usage) noexcept {
    return symbol_from_layout(GetKeyboardLayout(0), usage);
}

uint64_t active_keyboard_layout_token() noexcept {
    return static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
}

SaccadeResult resolve_hint_language(const application::HintSettings& input, application::HintSettings* output,
                                    uint64_t* layout_token) noexcept {
    if (output == nullptr || layout_token == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;

    const HKL layout = layout_for_language(input.language.data());
    if (layout == nullptr) return SACCADE_ERROR_NOT_FOUND;
    std::array<uint16_t, interaction::maximum_hint_alphabet> translated{};
    for (uint32_t index = 0; index < input.alphabet_count; ++index)
        translated[index] = symbol_from_layout(layout, input.physical_keys[index]);

    const SaccadeResult result = application::resolve_hint_alphabet(input, translated.data(), output);
    if (result == SACCADE_OK) *layout_token = static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(layout));
    return result;
}

} // namespace saccade::platform::windows

#include "platform/macos/keyboard.hpp"

#include "application/settings.hpp"

#include <Carbon/Carbon.h>
#include <IOKit/hid/IOHIDUsageTables.h>

#include <array>
#include <cstring>

namespace saccade::platform::macos {
namespace {

constexpr auto letter_keycodes = std::to_array<CGKeyCode>(
    {kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I,
     kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
     kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z});

constexpr auto digit_keycodes = std::to_array<CGKeyCode>({kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
                                                          kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9, kVK_ANSI_0});

constexpr auto function_keycodes = std::to_array<CGKeyCode>(
    {kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12});

uint16_t symbol_from_source(TISInputSourceRef source, uint32_t usage) noexcept {
    CGKeyCode keycode = 0;
    if (source == nullptr || !keycode_from_hid_usage(usage, &keycode)) return 0;

    const auto* data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    const auto* layout = data == nullptr ? nullptr : reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
    UInt32 dead_key_state = 0;
    UniChar symbols[4]{};
    UniCharCount symbol_count = 0;
    const OSStatus translated =
        layout == nullptr
            ? paramErr
            : UCKeyTranslate(layout, keycode, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit,
                             &dead_key_state, std::size(symbols), &symbol_count, symbols);
    if (translated != noErr || symbol_count != 1 || (symbols[0] >= 0xd800 && symbols[0] <= 0xdfff)) return 0;
    return symbols[0];
}

bool language_match(const char* requested, CFStringRef candidate, bool exact) noexcept {
    std::array<char, 64> candidate_bytes{};
    if (candidate == nullptr ||
        !CFStringGetCString(candidate, candidate_bytes.data(), candidate_bytes.size(), kCFStringEncodingUTF8)) {
        return false;
    }

    size_t requested_size = std::strlen(requested);
    size_t candidate_size = std::strlen(candidate_bytes.data());
    if (!exact) {
        const char* requested_separator = std::strchr(requested, '-');
        const char* candidate_separator = std::strchr(candidate_bytes.data(), '-');
        requested_size =
            requested_separator == nullptr ? requested_size : static_cast<size_t>(requested_separator - requested);
        candidate_size = candidate_separator == nullptr
                             ? candidate_size
                             : static_cast<size_t>(candidate_separator - candidate_bytes.data());
    }
    if (requested_size != candidate_size) return false;
    for (size_t index = 0; index < requested_size; ++index) {
        const char left =
            requested[index] >= 'A' && requested[index] <= 'Z' ? requested[index] + ('a' - 'A') : requested[index];
        const char right = candidate_bytes[index] >= 'A' && candidate_bytes[index] <= 'Z'
                               ? candidate_bytes[index] + ('a' - 'A')
                               : candidate_bytes[index];
        if (left != right) return false;
    }
    return true;
}

TISInputSourceRef source_for_language(const char* language) noexcept {
    if (std::strcmp(language, "und") == 0) return TISCopyCurrentKeyboardLayoutInputSource();

    CFArrayRef sources = TISCreateInputSourceList(nullptr, true);
    if (sources == nullptr) return nullptr;
    TISInputSourceRef selected = nullptr;
    for (uint32_t pass = 0; pass < 2 && selected == nullptr; ++pass) {
        const bool exact = pass == 0;
        const CFIndex source_count = CFArrayGetCount(sources);
        for (CFIndex source_index = 0; source_index < source_count && selected == nullptr; ++source_index) {
            const auto source =
                static_cast<TISInputSourceRef>(const_cast<void*>(CFArrayGetValueAtIndex(sources, source_index)));
            if (TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) == nullptr) continue;
            const auto languages =
                static_cast<CFArrayRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages));
            if (languages == nullptr) continue;
            const CFIndex language_count = CFArrayGetCount(languages);
            for (CFIndex language_index = 0; language_index < language_count; ++language_index) {
                const auto candidate = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, language_index));
                if (!language_match(language, candidate, exact)) continue;
                CFRetain(source);
                selected = source;
                break;
            }
        }
    }
    CFRelease(sources);
    return selected;
}

uint64_t source_token(TISInputSourceRef source) noexcept {
    const auto identifier =
        source == nullptr ? nullptr
                          : static_cast<CFStringRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
    std::array<char, 256> bytes{};
    if (identifier == nullptr || !CFStringGetCString(identifier, bytes.data(), bytes.size(), kCFStringEncodingUTF8)) {
        return 0;
    }

    uint64_t token = UINT64_C(1469598103934665603);
    for (const char* cursor = bytes.data(); *cursor != '\0'; ++cursor) {
        token ^= static_cast<uint8_t>(*cursor);
        token *= UINT64_C(1099511628211);
    }
    return token;
}

} // namespace

bool keycode_from_hid_usage(uint32_t usage, CGKeyCode* output) noexcept {
    if (output == nullptr) return false;
    if (usage >= kHIDUsage_KeyboardA && usage <= kHIDUsage_KeyboardZ) {
        *output = letter_keycodes[usage - kHIDUsage_KeyboardA];
        return true;
    }
    if (usage >= kHIDUsage_Keyboard1 && usage <= kHIDUsage_Keyboard0) {
        *output = digit_keycodes[usage - kHIDUsage_Keyboard1];
        return true;
    }
    if (usage >= kHIDUsage_KeyboardF1 && usage <= kHIDUsage_KeyboardF12) {
        *output = function_keycodes[usage - kHIDUsage_KeyboardF1];
        return true;
    }
    switch (usage) {
    case kHIDUsage_KeyboardReturnOrEnter:
        *output = kVK_Return;
        return true;
    case kHIDUsage_KeyboardEscape:
        *output = kVK_Escape;
        return true;
    case kHIDUsage_KeyboardDeleteOrBackspace:
        *output = kVK_Delete;
        return true;
    case kHIDUsage_KeyboardTab:
        *output = kVK_Tab;
        return true;
    case kHIDUsage_KeyboardSpacebar:
        *output = kVK_Space;
        return true;
    case kHIDUsage_KeyboardHyphen:
        *output = kVK_ANSI_Minus;
        return true;
    case kHIDUsage_KeyboardEqualSign:
        *output = kVK_ANSI_Equal;
        return true;
    case kHIDUsage_KeyboardOpenBracket:
        *output = kVK_ANSI_LeftBracket;
        return true;
    case kHIDUsage_KeyboardCloseBracket:
        *output = kVK_ANSI_RightBracket;
        return true;
    case kHIDUsage_KeyboardBackslash:
        *output = kVK_ANSI_Backslash;
        return true;
    case kHIDUsage_KeyboardSemicolon:
        *output = kVK_ANSI_Semicolon;
        return true;
    case kHIDUsage_KeyboardQuote:
        *output = kVK_ANSI_Quote;
        return true;
    case kHIDUsage_KeyboardGraveAccentAndTilde:
        *output = kVK_ANSI_Grave;
        return true;
    case kHIDUsage_KeyboardComma:
        *output = kVK_ANSI_Comma;
        return true;
    case kHIDUsage_KeyboardPeriod:
        *output = kVK_ANSI_Period;
        return true;
    case kHIDUsage_KeyboardSlash:
        *output = kVK_ANSI_Slash;
        return true;
    case kHIDUsage_KeyboardCapsLock:
        *output = kVK_CapsLock;
        return true;
    case kHIDUsage_KeyboardPrintScreen:
        *output = kVK_F13;
        return true;
    case kHIDUsage_KeyboardScrollLock:
        *output = kVK_F14;
        return true;
    case kHIDUsage_KeyboardPause:
        *output = kVK_F15;
        return true;
    case kHIDUsage_KeyboardInsert:
        *output = kVK_Help;
        return true;
    case kHIDUsage_KeyboardHome:
        *output = kVK_Home;
        return true;
    case kHIDUsage_KeyboardPageUp:
        *output = kVK_PageUp;
        return true;
    case kHIDUsage_KeyboardDeleteForward:
        *output = kVK_ForwardDelete;
        return true;
    case kHIDUsage_KeyboardEnd:
        *output = kVK_End;
        return true;
    case kHIDUsage_KeyboardPageDown:
        *output = kVK_PageDown;
        return true;
    case kHIDUsage_KeyboardRightArrow:
        *output = kVK_RightArrow;
        return true;
    case kHIDUsage_KeyboardLeftArrow:
        *output = kVK_LeftArrow;
        return true;
    case kHIDUsage_KeyboardDownArrow:
        *output = kVK_DownArrow;
        return true;
    case kHIDUsage_KeyboardUpArrow:
        *output = kVK_UpArrow;
        return true;
    case kHIDUsage_KeyboardLeftControl:
        *output = kVK_Control;
        return true;
    case kHIDUsage_KeyboardLeftShift:
        *output = kVK_Shift;
        return true;
    case kHIDUsage_KeyboardLeftAlt:
        *output = kVK_Option;
        return true;
    case kHIDUsage_KeyboardLeftGUI:
        *output = kVK_Command;
        return true;
    case kHIDUsage_KeyboardRightControl:
        *output = kVK_RightControl;
        return true;
    case kHIDUsage_KeyboardRightShift:
        *output = kVK_RightShift;
        return true;
    case kHIDUsage_KeyboardRightAlt:
        *output = kVK_RightOption;
        return true;
    case kHIDUsage_KeyboardRightGUI:
        *output = kVK_RightCommand;
        return true;
    default:
        return false;
    }
}

bool hid_usage_from_keycode(CGKeyCode keycode, uint32_t* output) noexcept {
    if (output == nullptr) return false;
    for (uint32_t index = 0; index < letter_keycodes.size(); ++index) {
        if (letter_keycodes[index] == keycode) {
            *output = kHIDUsage_KeyboardA + index;
            return true;
        }
    }
    for (uint32_t index = 0; index < digit_keycodes.size(); ++index) {
        if (digit_keycodes[index] == keycode) {
            *output = kHIDUsage_Keyboard1 + index;
            return true;
        }
    }
    for (uint32_t index = 0; index < function_keycodes.size(); ++index) {
        if (function_keycodes[index] == keycode) {
            *output = kHIDUsage_KeyboardF1 + index;
            return true;
        }
    }

    struct KeyUsage {
        CGKeyCode keycode_;
        uint32_t usage_;
    };

    constexpr std::array<KeyUsage, 36> remaining{{{kVK_Return, kHIDUsage_KeyboardReturnOrEnter},
                                                  {kVK_Escape, kHIDUsage_KeyboardEscape},
                                                  {kVK_Delete, kHIDUsage_KeyboardDeleteOrBackspace},
                                                  {kVK_Tab, kHIDUsage_KeyboardTab},
                                                  {kVK_Space, kHIDUsage_KeyboardSpacebar},
                                                  {kVK_ANSI_Minus, kHIDUsage_KeyboardHyphen},
                                                  {kVK_ANSI_Equal, kHIDUsage_KeyboardEqualSign},
                                                  {kVK_ANSI_LeftBracket, kHIDUsage_KeyboardOpenBracket},
                                                  {kVK_ANSI_RightBracket, kHIDUsage_KeyboardCloseBracket},
                                                  {kVK_ANSI_Backslash, kHIDUsage_KeyboardBackslash},
                                                  {kVK_ANSI_Semicolon, kHIDUsage_KeyboardSemicolon},
                                                  {kVK_ANSI_Quote, kHIDUsage_KeyboardQuote},
                                                  {kVK_ANSI_Grave, kHIDUsage_KeyboardGraveAccentAndTilde},
                                                  {kVK_ANSI_Comma, kHIDUsage_KeyboardComma},
                                                  {kVK_ANSI_Period, kHIDUsage_KeyboardPeriod},
                                                  {kVK_ANSI_Slash, kHIDUsage_KeyboardSlash},
                                                  {kVK_CapsLock, kHIDUsage_KeyboardCapsLock},
                                                  {kVK_F13, kHIDUsage_KeyboardPrintScreen},
                                                  {kVK_F14, kHIDUsage_KeyboardScrollLock},
                                                  {kVK_F15, kHIDUsage_KeyboardPause},
                                                  {kVK_Help, kHIDUsage_KeyboardInsert},
                                                  {kVK_Home, kHIDUsage_KeyboardHome},
                                                  {kVK_PageUp, kHIDUsage_KeyboardPageUp},
                                                  {kVK_ForwardDelete, kHIDUsage_KeyboardDeleteForward},
                                                  {kVK_End, kHIDUsage_KeyboardEnd},
                                                  {kVK_PageDown, kHIDUsage_KeyboardPageDown},
                                                  {kVK_RightArrow, kHIDUsage_KeyboardRightArrow},
                                                  {kVK_LeftArrow, kHIDUsage_KeyboardLeftArrow},
                                                  {kVK_DownArrow, kHIDUsage_KeyboardDownArrow},
                                                  {kVK_UpArrow, kHIDUsage_KeyboardUpArrow},
                                                  {kVK_Control, kHIDUsage_KeyboardLeftControl},
                                                  {kVK_Shift, kHIDUsage_KeyboardLeftShift},
                                                  {kVK_Option, kHIDUsage_KeyboardLeftAlt},
                                                  {kVK_Command, kHIDUsage_KeyboardLeftGUI},
                                                  {kVK_RightControl, kHIDUsage_KeyboardRightControl},
                                                  {kVK_RightShift, kHIDUsage_KeyboardRightShift}}};
    for (const KeyUsage& value : remaining) {
        if (value.keycode_ == keycode) {
            *output = value.usage_;
            return true;
        }
    }
    if (keycode == kVK_RightOption) {
        *output = kHIDUsage_KeyboardRightAlt;
        return true;
    }
    if (keycode == kVK_RightCommand) {
        *output = kHIDUsage_KeyboardRightGUI;
        return true;
    }
    return false;
}

uint16_t logical_symbol_from_hid_usage(uint32_t usage) noexcept {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr) return 0;
    const uint16_t symbol = symbol_from_source(source, usage);
    CFRelease(source);
    return symbol;
}

uint64_t active_keyboard_layout_token() noexcept {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    const uint64_t token = source_token(source);
    if (source != nullptr) CFRelease(source);
    return token;
}

SaccadeResult resolve_hint_language(const application::HintSettings& input, application::HintSettings* output,
                                    uint64_t* layout_token) noexcept {
    if (output == nullptr || layout_token == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;

    TISInputSourceRef source = source_for_language(input.language.data());
    if (source == nullptr) return SACCADE_ERROR_NOT_FOUND;
    std::array<uint16_t, interaction::maximum_hint_alphabet> translated{};
    for (uint32_t index = 0; index < input.alphabet_count; ++index)
        translated[index] = symbol_from_source(source, input.physical_keys[index]);
    const uint64_t token = source_token(source);
    CFRelease(source);
    if (token == 0) return SACCADE_ERROR_BACKEND;

    const SaccadeResult result = application::resolve_hint_alphabet(input, translated.data(), output);
    if (result == SACCADE_OK) *layout_token = token;
    return result;
}

} // namespace saccade::platform::macos

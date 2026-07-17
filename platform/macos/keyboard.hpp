#ifndef SACCADE_PLATFORM_MACOS_KEYBOARD_HPP
#define SACCADE_PLATFORM_MACOS_KEYBOARD_HPP

#include <saccade/saccade.h>

#include <CoreGraphics/CoreGraphics.h>

#include <cstdint>

namespace saccade::application {
struct HintSettings;
}

namespace saccade::platform::macos {

bool keycode_from_hid_usage(uint32_t usage, CGKeyCode* output) noexcept;
bool hid_usage_from_keycode(CGKeyCode keycode, uint32_t* output) noexcept;
uint16_t logical_symbol_from_hid_usage(uint32_t usage) noexcept;
uint64_t active_keyboard_layout_token() noexcept;
SaccadeResult resolve_hint_language(const application::HintSettings&, application::HintSettings*,
                                    uint64_t* layout_token) noexcept;

} // namespace saccade::platform::macos

#endif

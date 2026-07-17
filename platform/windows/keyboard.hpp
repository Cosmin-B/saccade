#ifndef SACCADE_PLATFORM_WINDOWS_KEYBOARD_HPP
#define SACCADE_PLATFORM_WINDOWS_KEYBOARD_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_input.h>

#include <cstdint>

namespace saccade::application {
struct HintSettings;
}

namespace saccade::platform::windows {

struct KeyScan {
    uint16_t value = 0;
    bool extended = false;
};

bool scan_from_hid_usage(uint32_t usage, KeyScan* output) noexcept;
bool hid_usage_from_scan(KeyScan scan, uint32_t* output) noexcept;
bool scan_from_modifier(uint32_t modifier, KeyScan* output) noexcept;
uint32_t modifier_from_scan(KeyScan scan) noexcept;
uint16_t logical_symbol_from_hid_usage(uint32_t usage) noexcept;
uint64_t active_keyboard_layout_token() noexcept;
SaccadeResult resolve_hint_language(const application::HintSettings&, application::HintSettings*,
                                    uint64_t* layout_token) noexcept;

} // namespace saccade::platform::windows

#endif

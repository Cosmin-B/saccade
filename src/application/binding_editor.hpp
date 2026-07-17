#ifndef SACCADE_APPLICATION_BINDING_EDITOR_HPP
#define SACCADE_APPLICATION_BINDING_EDITOR_HPP

#include "application/settings.hpp"

#include <array>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t binding_key_count = 68;

struct BindingKey {
    uint32_t usage = 0;
    const char* name = nullptr;
};

struct BindingConflict {
    Command command = Command::pointer_move;
    uint32_t index = UINT32_MAX;
};

struct BindingKeyRect {
    uint32_t usage = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct BindingKeyboardLayout {
    std::array<BindingKeyRect, binding_key_count> keys{};
    uint32_t key_count = 0;
};

constexpr uint32_t binding_command_count = static_cast<uint32_t>(last_command);

const char* command_name(Command) noexcept;
const std::array<BindingKey, binding_key_count>& binding_keys() noexcept;
SaccadeResult layout_binding_keyboard(int32_t width, int32_t height, int32_t gap, BindingKeyboardLayout*) noexcept;
uint16_t default_logical_symbol(uint32_t physical_key) noexcept;
SaccadeResult find_binding(const SettingsDocument&, Command, uint32_t* index) noexcept;
SaccadeResult set_binding(SettingsDocument*, HotkeyBinding, BindingConflict*) noexcept;
SaccadeResult remove_binding(SettingsDocument*, Command) noexcept;

} // namespace saccade::application

#endif

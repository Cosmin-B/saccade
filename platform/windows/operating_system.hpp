#pragma once

#include <cstdint>

namespace saccade::platform::windows {

constexpr uint32_t minimum_supported_windows_build = 26100;

uint32_t operating_system_build() noexcept;
bool operating_system_supported() noexcept;

} // namespace saccade::platform::windows

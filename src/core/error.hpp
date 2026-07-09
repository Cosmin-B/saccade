#ifndef SACCADE_CORE_ERROR_HPP
#define SACCADE_CORE_ERROR_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace saccade::core {

inline constexpr size_t kErrorCapacity = 512;

class ErrorScope final {
public:
    ErrorScope() noexcept;
    ~ErrorScope() noexcept;

    ErrorScope(const ErrorScope&) = delete;
    ErrorScope& operator=(const ErrorScope&) = delete;
    ErrorScope(ErrorScope&&) = delete;
    ErrorScope& operator=(ErrorScope&&) = delete;

private:
    std::array<char, kErrorCapacity> saved_{};
    size_t saved_size_ = 0;
    bool restore_ = false;
};

void set_last_error(const char* message) noexcept;
void set_last_error(std::string_view message) noexcept;
void set_last_errorf(const char* format, ...) noexcept;
void clear_last_error() noexcept;
SaccadeSpanU8 last_error() noexcept;

}  // namespace saccade::core

#endif

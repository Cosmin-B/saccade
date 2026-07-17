#include "core/error.hpp"

#include <algorithm>
#include <cstring>

namespace saccade::core {
namespace {

struct ErrorState {
    std::array<char, kErrorCapacity> text{};
    size_t size = 0;
    uint32_t depth = 0;
};

thread_local ErrorState error_state;

bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

size_t sequence_size(unsigned char lead) noexcept {
    if ((lead & 0x80U) == 0) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

size_t trim_incomplete_utf8(const char* text, size_t size) noexcept {
    if (size == 0) {
        return 0;
    }

    size_t start = size - 1;
    while (start > 0 && is_continuation(static_cast<unsigned char>(text[start]))) {
        --start;
    }

    const size_t expected = sequence_size(static_cast<unsigned char>(text[start]));
    return start + expected > size ? start : size;
}

void commit_size(size_t size) noexcept {
    error_state.size = trim_incomplete_utf8(error_state.text.data(), size);
    error_state.text[error_state.size] = '\0';
}

} // namespace

ErrorScope::ErrorScope() noexcept {
    restore_ = error_state.depth != 0;
    if (restore_) {
        saved_size_ = error_state.size;
        std::memcpy(saved_.data(), error_state.text.data(), saved_size_ + 1);
    }
    ++error_state.depth;
    clear_last_error();
}

ErrorScope::~ErrorScope() noexcept {
    if (error_state.depth != 0) {
        --error_state.depth;
    }
    if (restore_) {
        std::memcpy(error_state.text.data(), saved_.data(), saved_size_ + 1);
        error_state.size = saved_size_;
    }
}

void set_last_error(const char* message) noexcept {
    if (message == nullptr) {
        clear_last_error();
        return;
    }
    set_last_error(std::string_view(message));
}

void set_last_error(std::string_view message) noexcept {
    const size_t size = std::min(message.size(), kErrorCapacity - 1);
    if (size != 0) {
        std::memcpy(error_state.text.data(), message.data(), size);
    }
    commit_size(size);
}

void clear_last_error() noexcept {
    error_state.size = 0;
    error_state.text[0] = '\0';
}

SaccadeSpanU8 last_error() noexcept {
    return {reinterpret_cast<const uint8_t*>(error_state.text.data()), error_state.size};
}

} // namespace saccade::core

extern "C" SaccadeSpanU8 SACCADE_CALL saccade_last_error(void) {
    return saccade::core::last_error();
}

#ifndef SACCADE_INPUT_UTF8_HPP
#define SACCADE_INPUT_UTF8_HPP

#include <cstddef>
#include <cstdint>

namespace saccade::input {

inline bool decode_utf8(const uint8_t** cursor, const uint8_t* end, uint32_t* codepoint) noexcept {
    if (cursor == nullptr || *cursor == nullptr || codepoint == nullptr || *cursor >= end) {
        return false;
    }
    const uint8_t first = *(*cursor)++;
    if (first < UINT8_C(0x80)) {
        *codepoint = first;
        return true;
    }
    uint32_t value = 0;
    uint32_t continuation_count = 0;
    uint32_t minimum_codepoint = 0;
    if ((first & UINT8_C(0xe0)) == UINT8_C(0xc0)) {
        value = first & UINT8_C(0x1f);
        continuation_count = 1;
        minimum_codepoint = UINT32_C(0x80);
    } else if ((first & UINT8_C(0xf0)) == UINT8_C(0xe0)) {
        value = first & UINT8_C(0x0f);
        continuation_count = 2;
        minimum_codepoint = UINT32_C(0x800);
    } else if ((first & UINT8_C(0xf8)) == UINT8_C(0xf0)) {
        value = first & UINT8_C(0x07);
        continuation_count = 3;
        minimum_codepoint = UINT32_C(0x10000);
    } else {
        return false;
    }
    if (static_cast<size_t>(end - *cursor) < continuation_count) {
        return false;
    }
    for (uint32_t index = 0; index < continuation_count; ++index) {
        const uint8_t byte = *(*cursor)++;
        if ((byte & UINT8_C(0xc0)) != UINT8_C(0x80)) {
            return false;
        }
        value = (value << 6U) | (byte & UINT8_C(0x3f));
    }
    constexpr uint32_t unicode_maximum = UINT32_C(0x10ffff);
    constexpr uint32_t surrogate_first = UINT32_C(0xd800);
    constexpr uint32_t surrogate_last = UINT32_C(0xdfff);
    if (value < minimum_codepoint || value > unicode_maximum || (value >= surrogate_first && value <= surrogate_last)) {
        return false;
    }
    *codepoint = value;
    return true;
}

} // namespace saccade::input

#endif

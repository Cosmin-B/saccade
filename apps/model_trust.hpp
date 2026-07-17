#ifndef SACCADE_APPS_MODEL_TRUST_HPP
#define SACCADE_APPS_MODEL_TRUST_HPP

#include "model/p256_verifier.hpp"

#include <cstddef>
#include <cstdint>

#ifndef SACCADE_MODEL_PUBLIC_KEY_XY
#define SACCADE_MODEL_PUBLIC_KEY_XY ""
#endif

namespace saccade::apps::model_trust {
namespace detail {

consteval uint8_t hex_digit(char value) {
    if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<uint8_t>(value - 'A' + 10);
    return UINT8_MAX;
}

template <size_t Size> consteval model::P256PublicKey parse(const char (&text)[Size]) {
    static_assert(Size == 1 || Size == 129, "SACCADE_MODEL_PUBLIC_KEY_XY must contain 128 hex digits");
    model::P256PublicKey key{};
    if constexpr (Size == 129) {
        for (size_t index = 0; index < key.xy.size(); ++index) {
            const uint8_t high = hex_digit(text[index * 2U]);
            const uint8_t low = hex_digit(text[index * 2U + 1U]);
            if (high == UINT8_MAX || low == UINT8_MAX) throw "invalid P-256 key";
            key.xy[index] = static_cast<uint8_t>((high << 4U) | low);
        }
    }
    return key;
}

} // namespace detail

inline constexpr char public_key_text[] = SACCADE_MODEL_PUBLIC_KEY_XY;
inline constexpr bool configured = sizeof(public_key_text) == 129;
inline constexpr model::P256PublicKey public_key = detail::parse(public_key_text);

} // namespace saccade::apps::model_trust

#endif

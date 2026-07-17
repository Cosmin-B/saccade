#ifndef SACCADE_CORE_CACHE_LINE_HPP
#define SACCADE_CORE_CACHE_LINE_HPP

#include <cstddef>

namespace saccade::core {

#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr size_t destructive_interference_size = 128;
#else
inline constexpr size_t destructive_interference_size = 64;
#endif

static_assert((destructive_interference_size & (destructive_interference_size - 1U)) == 0);

} // namespace saccade::core

#endif

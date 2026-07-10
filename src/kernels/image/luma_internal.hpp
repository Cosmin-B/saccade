#ifndef SACCADE_KERNELS_IMAGE_LUMA_INTERNAL_HPP
#define SACCADE_KERNELS_IMAGE_LUMA_INTERNAL_HPP

#include "kernels/image/luma.hpp"

#include <saccade/saccade_backend.h>

#include <cstdint>

namespace saccade::kernels::image::detail {

inline uint8_t luma(uint8_t red, uint8_t green, uint8_t blue) noexcept {
    const uint32_t weighted = UINT32_C(77) * red + UINT32_C(150) * green + UINT32_C(29) * blue;
    return static_cast<uint8_t>((weighted + UINT32_C(128)) >> 8U);
}

void convert_scalar(const InterleavedU8View& source, const PlaneU8View& destination) noexcept;

#if defined(SACCADE_BUILD_LUMA_NEON)
void convert_neon(const InterleavedU8View& source, const PlaneU8View& destination) noexcept;
#endif

#if defined(SACCADE_BUILD_LUMA_AVX2)
void convert_avx2(const InterleavedU8View& source, const PlaneU8View& destination) noexcept;
[[nodiscard]] bool avx2_cpu_available() noexcept;
#endif

} // namespace saccade::kernels::image::detail

#endif

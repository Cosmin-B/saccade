#include "kernels/image/luma_internal.hpp"

#if defined(_MSC_VER) && defined(_M_ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include <cstddef>
#include <cstdint>

namespace saccade::kernels::image::detail {
namespace {

template <bool Rgba>
void convert_neon_format(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    constexpr uint32_t vector_width = 16;
    const uint8x8_t red_weight = vdup_n_u8(UINT8_C(77));
    const uint8x8_t green_weight = vdup_n_u8(UINT8_C(150));
    const uint8x8_t blue_weight = vdup_n_u8(UINT8_C(29));
    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* source_row = source.data + static_cast<size_t>(y) * source.row_stride_bytes;
        uint8_t* destination_row =
            destination.data + static_cast<size_t>(y) * destination.row_stride_bytes;

        uint32_t x = 0;
        for (; source.width - x >= vector_width; x += vector_width) {
            const uint8x16x4_t pixels = vld4q_u8(source_row + static_cast<size_t>(x) * 4U);
            const uint8x16_t red = pixels.val[Rgba ? 0 : 2];
            const uint8x16_t green = pixels.val[1];
            const uint8x16_t blue = pixels.val[Rgba ? 2 : 0];

            uint16x8_t low = vmull_u8(vget_low_u8(red), red_weight);
            low = vmlal_u8(low, vget_low_u8(green), green_weight);
            low = vmlal_u8(low, vget_low_u8(blue), blue_weight);
            low = vaddq_u16(low, vdupq_n_u16(UINT16_C(128)));

            uint16x8_t high = vmull_u8(vget_high_u8(red), red_weight);
            high = vmlal_u8(high, vget_high_u8(green), green_weight);
            high = vmlal_u8(high, vget_high_u8(blue), blue_weight);
            high = vaddq_u16(high, vdupq_n_u16(UINT16_C(128)));

            vst1q_u8(destination_row + x, vcombine_u8(vshrn_n_u16(low, 8), vshrn_n_u16(high, 8)));
        }

        for (; x < source.width; ++x) {
            const uint8_t* pixel = source_row + static_cast<size_t>(x) * 4U;
            const uint8_t red = Rgba ? pixel[0] : pixel[2];
            const uint8_t blue = Rgba ? pixel[2] : pixel[0];
            destination_row[x] = luma(red, pixel[1], blue);
        }
    }
}

} // namespace

void convert_neon(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    if (source.pixel_format == SACCADE_FORMAT_RGBA8) {
        convert_neon_format<true>(source, destination);
        return;
    }
    convert_neon_format<false>(source, destination);
}

} // namespace saccade::kernels::image::detail

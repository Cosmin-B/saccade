#include "kernels/image/luma_internal.hpp"

#include <immintrin.h>
#include <smmintrin.h>

#include <cstddef>
#include <cstdint>

namespace saccade::kernels::image::detail {
namespace {

template <bool Rgba>
void convert_avx2_format(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    const __m256i coefficients =
        Rgba ? _mm256_setr_epi16(77, 150, 29, 0, 77, 150, 29, 0, 77, 150, 29, 0, 77, 150, 29, 0)
             : _mm256_setr_epi16(29, 150, 77, 0, 29, 150, 77, 0, 29, 150, 77, 0, 29, 150, 77, 0);
    const __m256i reorder = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    const __m256i rounding = _mm256_set1_epi32(128);

    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* source_row = source.data + static_cast<size_t>(y) * source.row_stride_bytes;
        uint8_t* destination_row = destination.data + static_cast<size_t>(y) * destination.row_stride_bytes;

        uint32_t x = 0;
        for (; source.width - x >= 8U; x += 8U) {
            const __m256i bytes =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source_row + static_cast<size_t>(x) * 4U));
            const __m256i low = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(bytes));
            const __m256i high = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(bytes, 1));
            const __m256i low_pairs = _mm256_madd_epi16(low, coefficients);
            const __m256i high_pairs = _mm256_madd_epi16(high, coefficients);
            __m256i values = _mm256_hadd_epi32(low_pairs, high_pairs);
            values = _mm256_permutevar8x32_epi32(values, reorder);
            values = _mm256_srli_epi32(_mm256_add_epi32(values, rounding), 8);

            const __m128i low_values = _mm256_castsi256_si128(values);
            const __m128i high_values = _mm256_extracti128_si256(values, 1);
            const __m128i packed16 = _mm_packus_epi32(low_values, high_values);
            const __m128i packed8 = _mm_packus_epi16(packed16, _mm_setzero_si128());
            _mm_storel_epi64(reinterpret_cast<__m128i*>(destination_row + x), packed8);
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

void convert_avx2(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    if (source.pixel_format == SACCADE_FORMAT_RGBA8) {
        convert_avx2_format<true>(source, destination);
        return;
    }
    convert_avx2_format<false>(source, destination);
}

} // namespace saccade::kernels::image::detail

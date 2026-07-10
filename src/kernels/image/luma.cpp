#include "kernels/image/luma_internal.hpp"

#include <saccade/saccade_backend.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(SACCADE_BUILD_LUMA_AVX2) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace saccade::kernels::image {
namespace {

struct ValidatedLayout {
    size_t source_row_bytes = 0;
    size_t source_required_bytes = 0;
    size_t destination_required_bytes = 0;
};

bool required_bytes(uint32_t height, uint32_t row_stride_bytes, size_t row_bytes,
                    size_t* out_required) noexcept {
    if (height == 0 || out_required == nullptr || row_stride_bytes < row_bytes) {
        return false;
    }
    const size_t rows_before_last = static_cast<size_t>(height - 1U);
    const size_t stride = row_stride_bytes;
    if (rows_before_last > (std::numeric_limits<size_t>::max() - row_bytes) / stride) {
        return false;
    }
    *out_required = rows_before_last * stride + row_bytes;
    return true;
}

bool overlap(const void* left, size_t left_size, const void* right, size_t right_size) noexcept {
    const uintptr_t left_begin = reinterpret_cast<uintptr_t>(left);
    const uintptr_t right_begin = reinterpret_cast<uintptr_t>(right);
    if (left_begin > std::numeric_limits<uintptr_t>::max() - left_size ||
        right_begin > std::numeric_limits<uintptr_t>::max() - right_size) {
        return true;
    }
    const uintptr_t left_end = left_begin + left_size;
    const uintptr_t right_end = right_begin + right_size;
    return left_begin < right_end && right_begin < left_end;
}

bool validate_layout(const InterleavedU8View& source, const PlaneU8View& destination,
                     ValidatedLayout* out_layout) noexcept {
    if (source.data == nullptr || destination.data == nullptr || out_layout == nullptr ||
        source.width == 0 || source.height == 0) {
        return false;
    }

    size_t pixel_size = 0;
    switch (source.pixel_format) {
    case SACCADE_FORMAT_R8:
        pixel_size = 1;
        break;
    case SACCADE_FORMAT_BGRA8:
    case SACCADE_FORMAT_RGBA8:
    case SACCADE_FORMAT_BGRX8:
        pixel_size = 4;
        break;
    default:
        return false;
    }

    if (static_cast<size_t>(source.width) > std::numeric_limits<size_t>::max() / pixel_size) {
        return false;
    }
    ValidatedLayout layout{};
    layout.source_row_bytes = static_cast<size_t>(source.width) * pixel_size;
    if (!required_bytes(source.height, source.row_stride_bytes, layout.source_row_bytes,
                        &layout.source_required_bytes) ||
        !required_bytes(source.height, destination.row_stride_bytes, source.width,
                        &layout.destination_required_bytes) ||
        source.size < layout.source_required_bytes ||
        destination.size < layout.destination_required_bytes) {
        return false;
    }

    const bool exact_r8_alias = source.pixel_format == SACCADE_FORMAT_R8 &&
                                source.data == destination.data &&
                                source.row_stride_bytes == destination.row_stride_bytes;
    if (!exact_r8_alias && overlap(source.data, layout.source_required_bytes, destination.data,
                                   layout.destination_required_bytes)) {
        return false;
    }

    *out_layout = layout;
    return true;
}

void copy_r8(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    if (source.data == destination.data &&
        source.row_stride_bytes == destination.row_stride_bytes) {
        return;
    }
    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* source_row = source.data + static_cast<size_t>(y) * source.row_stride_bytes;
        uint8_t* destination_row =
            destination.data + static_cast<size_t>(y) * destination.row_stride_bytes;
        std::memcpy(destination_row, source_row, source.width);
    }
}

} // namespace

namespace detail {

#if defined(SACCADE_BUILD_LUMA_AVX2)
bool avx2_cpu_available() noexcept {
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuid(registers, 1);
    constexpr int avx_mask = 1 << 28;
    constexpr int osxsave_mask = 1 << 27;
    if ((registers[2] & (avx_mask | osxsave_mask)) != (avx_mask | osxsave_mask)) {
        return false;
    }
    constexpr uint64_t xmm_ymm_mask = UINT64_C(0x6);
    if ((_xgetbv(0) & xmm_ymm_mask) != xmm_ymm_mask) {
        return false;
    }
    __cpuidex(registers, 7, 0);
    constexpr int avx2_mask = 1 << 5;
    return (registers[1] & avx2_mask) != 0;
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}
#endif

void convert_scalar(const InterleavedU8View& source, const PlaneU8View& destination) noexcept {
    const bool rgba = source.pixel_format == SACCADE_FORMAT_RGBA8;
    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* source_row = source.data + static_cast<size_t>(y) * source.row_stride_bytes;
        uint8_t* destination_row =
            destination.data + static_cast<size_t>(y) * destination.row_stride_bytes;
        for (uint32_t x = 0; x < source.width; ++x) {
            const uint8_t* pixel = source_row + static_cast<size_t>(x) * 4U;
            const uint8_t red = rgba ? pixel[0] : pixel[2];
            const uint8_t blue = rgba ? pixel[2] : pixel[0];
            destination_row[x] = luma(red, pixel[1], blue);
        }
    }
}

} // namespace detail

bool luma_path_compiled(LumaPath path) noexcept {
    switch (path) {
    case LumaPath::automatic:
    case LumaPath::scalar:
        return true;
    case LumaPath::neon:
#if defined(SACCADE_BUILD_LUMA_NEON)
        return true;
#else
        return false;
#endif
    case LumaPath::avx2:
#if defined(SACCADE_BUILD_LUMA_AVX2)
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

bool luma_path_available(LumaPath path) noexcept {
    if (!luma_path_compiled(path)) {
        return false;
    }
    if (path == LumaPath::avx2) {
        return selected_luma_path() == LumaPath::avx2;
    }
    return true;
}

LumaPath selected_luma_path() noexcept {
#if defined(SACCADE_BUILD_LUMA_NEON)
    return LumaPath::neon;
#elif defined(SACCADE_BUILD_LUMA_AVX2)
    thread_local const LumaPath selected =
        detail::avx2_cpu_available() ? LumaPath::avx2 : LumaPath::scalar;
    return selected;
#else
    return LumaPath::scalar;
#endif
}

SaccadeResult convert_to_luma(const InterleavedU8View& source, const PlaneU8View& destination,
                              LumaPath path) noexcept {
    ValidatedLayout layout{};
    if (!validate_layout(source, destination, &layout)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    (void)layout;

    if (!luma_path_available(path)) {
        switch (path) {
        case LumaPath::neon:
        case LumaPath::avx2:
            return SACCADE_ERROR_UNSUPPORTED;
        default:
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }

    if (source.pixel_format == SACCADE_FORMAT_R8) {
        copy_r8(source, destination);
        return SACCADE_OK;
    }

    const LumaPath selected = path == LumaPath::automatic ? selected_luma_path() : path;
    switch (selected) {
    case LumaPath::scalar:
        detail::convert_scalar(source, destination);
        return SACCADE_OK;
    case LumaPath::neon:
#if defined(SACCADE_BUILD_LUMA_NEON)
        detail::convert_neon(source, destination);
        return SACCADE_OK;
#else
        return SACCADE_ERROR_UNSUPPORTED;
#endif
    case LumaPath::avx2:
#if defined(SACCADE_BUILD_LUMA_AVX2)
        detail::convert_avx2(source, destination);
        return SACCADE_OK;
#else
        return SACCADE_ERROR_UNSUPPORTED;
#endif
    default:
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
}

} // namespace saccade::kernels::image

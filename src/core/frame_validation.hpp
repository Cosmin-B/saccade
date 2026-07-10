#ifndef SACCADE_CORE_FRAME_VALIDATION_HPP
#define SACCADE_CORE_FRAME_VALIDATION_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace saccade::core {

inline constexpr size_t host_frame_bytes_per_pixel(uint32_t pixel_format) noexcept {
    switch (pixel_format) {
        case SACCADE_FORMAT_BGRA8:
        case SACCADE_FORMAT_RGBA8:
        case SACCADE_FORMAT_BGRX8:
            return 4;
        case SACCADE_FORMAT_R8:
            return 1;
        case SACCADE_FORMAT_RGB_F16:
            return 6;
        default:
            return 0;
    }
}

inline bool valid_host_frame(const SaccadeHostFrameDesc& desc) noexcept {
    if (desc.data.data == nullptr || desc.width == 0 || desc.height == 0) {
        return false;
    }

    const size_t bytes_per_pixel = host_frame_bytes_per_pixel(desc.pixel_format);
    const size_t width = static_cast<size_t>(desc.width);
    const size_t maximum = std::numeric_limits<size_t>::max();
    if (bytes_per_pixel == 0 || width > maximum / bytes_per_pixel) {
        return false;
    }

    const size_t row_bytes = width * bytes_per_pixel;
    const size_t row_stride = static_cast<size_t>(desc.row_stride_bytes);
    if (row_stride < row_bytes) {
        return false;
    }

    const size_t rows_before_last = static_cast<size_t>(desc.height - 1U);
    if (rows_before_last > (maximum - row_bytes) / row_stride) {
        return false;
    }
    const size_t required_bytes = rows_before_last * row_stride + row_bytes;
    return desc.data.size >= required_bytes;
}

}  // namespace saccade::core

#endif

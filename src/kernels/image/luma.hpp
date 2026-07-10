#ifndef SACCADE_KERNELS_IMAGE_LUMA_HPP
#define SACCADE_KERNELS_IMAGE_LUMA_HPP

#include <saccade/saccade.h>

#include <cstddef>
#include <cstdint>

namespace saccade::kernels::image {

enum class LumaPath : uint8_t { automatic = 0, scalar = 1, neon = 2, avx2 = 3 };

struct InterleavedU8View {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_stride_bytes = 0;
    uint32_t pixel_format = 0;
};

struct PlaneU8View {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t row_stride_bytes = 0;
};

[[nodiscard]] bool luma_path_available(LumaPath path) noexcept;
[[nodiscard]] bool luma_path_compiled(LumaPath path) noexcept;
[[nodiscard]] LumaPath selected_luma_path() noexcept;

SaccadeResult convert_to_luma(const InterleavedU8View& source, const PlaneU8View& destination,
                              LumaPath path = LumaPath::automatic) noexcept;

} // namespace saccade::kernels::image

#endif

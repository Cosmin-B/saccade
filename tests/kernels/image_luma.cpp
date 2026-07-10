#include "../support/allocation_tracker.hpp"
#include "kernels/image/luma.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace {

constexpr size_t maximum_width = 67;
constexpr size_t maximum_height = 19;
constexpr size_t source_stride = maximum_width * 4 + 13;
constexpr size_t destination_stride = maximum_width + 11;

std::array<uint8_t, source_stride * maximum_height> source_pixels{};
std::array<uint8_t, destination_stride * maximum_height> scalar_pixels{};
std::array<uint8_t, destination_stride * maximum_height> selected_pixels{};

uint8_t reference_luma(uint8_t red, uint8_t green, uint8_t blue) noexcept {
    const uint32_t weighted = UINT32_C(77) * red + UINT32_C(150) * green + UINT32_C(29) * blue;
    return static_cast<uint8_t>((weighted + UINT32_C(128)) >> 8U);
}

void fill_source(uint32_t width, uint32_t height, uint32_t format) noexcept {
    source_pixels.fill(UINT8_C(0xA5));
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = source_pixels.data() + static_cast<size_t>(y) * source_stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t red = static_cast<uint8_t>((x * 17U + y * 31U + 3U) & 0xFFU);
            const uint8_t green = static_cast<uint8_t>((x * 43U + y * 7U + 91U) & 0xFFU);
            const uint8_t blue = static_cast<uint8_t>((x * 5U + y * 59U + 201U) & 0xFFU);
            uint8_t* pixel = row + static_cast<size_t>(x) * 4U;
            if (format == SACCADE_FORMAT_RGBA8) {
                pixel[0] = red;
                pixel[1] = green;
                pixel[2] = blue;
            } else {
                pixel[0] = blue;
                pixel[1] = green;
                pixel[2] = red;
            }
            pixel[3] = static_cast<uint8_t>(x + y);
        }
    }
}

saccade::kernels::image::InterleavedU8View source_view(uint32_t width, uint32_t height,
                                                       uint32_t format) noexcept {
    return {source_pixels.data(),
            source_pixels.size(),
            width,
            height,
            static_cast<uint32_t>(source_stride),
            format};
}

saccade::kernels::image::PlaneU8View
destination_view(std::array<uint8_t, destination_stride * maximum_height>& pixels) noexcept {
    return {pixels.data(), pixels.size(), static_cast<uint32_t>(destination_stride)};
}

bool verify_reference(const std::array<uint8_t, destination_stride * maximum_height>& pixels,
                      uint32_t width, uint32_t height, uint32_t format) noexcept {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = source_pixels.data() + static_cast<size_t>(y) * source_stride;
        const uint8_t* output = pixels.data() + static_cast<size_t>(y) * destination_stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = source + static_cast<size_t>(x) * 4U;
            const uint8_t red = format == SACCADE_FORMAT_RGBA8 ? pixel[0] : pixel[2];
            const uint8_t green = pixel[1];
            const uint8_t blue = format == SACCADE_FORMAT_RGBA8 ? pixel[2] : pixel[0];
            if (output[x] != reference_luma(red, green, blue)) {
                return false;
            }
        }
        for (size_t x = width; x < destination_stride; ++x) {
            if (output[x] != UINT8_C(0xCD)) {
                return false;
            }
        }
    }
    return true;
}

bool equal_active_pixels(const std::array<uint8_t, destination_stride * maximum_height>& left,
                         const std::array<uint8_t, destination_stride * maximum_height>& right,
                         uint32_t width, uint32_t height) noexcept {
    for (uint32_t y = 0; y < height; ++y) {
        const size_t offset = static_cast<size_t>(y) * destination_stride;
        for (uint32_t x = 0; x < width; ++x) {
            if (left[offset + x] != right[offset + x]) {
                return false;
            }
        }
    }
    return true;
}

bool padding_is_unchanged(const std::array<uint8_t, destination_stride * maximum_height>& pixels,
                          uint32_t width, uint32_t height) noexcept {
    for (uint32_t y = 0; y < height; ++y) {
        const size_t offset = static_cast<size_t>(y) * destination_stride;
        for (size_t x = width; x < destination_stride; ++x) {
            if (pixels[offset + x] != UINT8_C(0xCD)) {
                return false;
            }
        }
    }
    return true;
}

template <size_t Width>
bool exact_tail_matches(saccade::kernels::image::LumaPath path, uint32_t format) noexcept {
    static_assert(Width > 0 && Width <= UINT32_MAX);
    std::array<uint8_t, Width * 4> source{};
    std::array<uint8_t, Width> scalar{};
    std::array<uint8_t, Width> selected{};
    for (size_t x = 0; x < Width; ++x) {
        uint8_t* pixel = source.data() + x * 4U;
        const uint8_t red = static_cast<uint8_t>((x * 17U + 3U) & 0xFFU);
        const uint8_t green = static_cast<uint8_t>((x * 43U + 91U) & 0xFFU);
        const uint8_t blue = static_cast<uint8_t>((x * 5U + 201U) & 0xFFU);
        if (format == SACCADE_FORMAT_RGBA8) {
            pixel[0] = red;
            pixel[1] = green;
            pixel[2] = blue;
        } else {
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
        }
        pixel[3] = static_cast<uint8_t>(x);
    }

    const auto width = static_cast<uint32_t>(Width);
    const auto source_size = static_cast<uint32_t>(Width * 4U);
    const saccade::kernels::image::InterleavedU8View input{source.data(), source.size(), width, 1,
                                                           source_size,   format};
    const saccade::kernels::image::PlaneU8View scalar_output{scalar.data(), scalar.size(), width};
    const saccade::kernels::image::PlaneU8View selected_output{selected.data(), selected.size(),
                                                               width};
    return saccade::kernels::image::convert_to_luma(
               input, scalar_output, saccade::kernels::image::LumaPath::scalar) == SACCADE_OK &&
           saccade::kernels::image::convert_to_luma(input, selected_output, path) == SACCADE_OK &&
           scalar == selected;
}

bool exact_tails_match(saccade::kernels::image::LumaPath path, uint32_t format) noexcept {
    return exact_tail_matches<1>(path, format) && exact_tail_matches<7>(path, format) &&
           exact_tail_matches<8>(path, format) && exact_tail_matches<9>(path, format) &&
           exact_tail_matches<15>(path, format) && exact_tail_matches<16>(path, format) &&
           exact_tail_matches<17>(path, format) && exact_tail_matches<31>(path, format) &&
           exact_tail_matches<32>(path, format) && exact_tail_matches<33>(path, format) &&
           exact_tail_matches<63>(path, format) && exact_tail_matches<64>(path, format) &&
           exact_tail_matches<65>(path, format);
}

} // namespace

int main() {
    using saccade::kernels::image::convert_to_luma;
    using saccade::kernels::image::InterleavedU8View;
    using saccade::kernels::image::luma_path_available;
    using saccade::kernels::image::luma_path_compiled;
    using saccade::kernels::image::LumaPath;
    using saccade::kernels::image::PlaneU8View;
    using saccade::kernels::image::selected_luma_path;

    static_assert(std::is_trivially_copyable_v<InterleavedU8View>);
    static_assert(std::is_trivially_copyable_v<PlaneU8View>);

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }
    if (!luma_path_compiled(LumaPath::automatic) || !luma_path_compiled(LumaPath::scalar) ||
        !luma_path_available(LumaPath::automatic) || !luma_path_available(LumaPath::scalar) ||
        selected_luma_path() == LumaPath::automatic || !luma_path_available(selected_luma_path()) ||
        luma_path_compiled(static_cast<LumaPath>(UINT8_C(0xFF))) ||
        luma_path_available(static_cast<LumaPath>(UINT8_C(0xFF)))) {
        return 2;
    }
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!luma_path_compiled(LumaPath::neon) || luma_path_compiled(LumaPath::avx2) ||
        selected_luma_path() != LumaPath::neon) {
        return 19;
    }
#elif defined(__x86_64__) || defined(_M_X64)
    if (!luma_path_compiled(LumaPath::avx2) || luma_path_compiled(LumaPath::neon)) {
        return 19;
    }
#endif

    constexpr std::array<uint32_t, 3> formats{SACCADE_FORMAT_BGRA8, SACCADE_FORMAT_BGRX8,
                                              SACCADE_FORMAT_RGBA8};
    constexpr std::array<uint32_t, 13> widths{1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 63, 67};
    for (uint32_t format : formats) {
        for (uint32_t width : widths) {
            constexpr uint32_t height = 19;
            fill_source(width, height, format);
            scalar_pixels.fill(UINT8_C(0xCD));
            selected_pixels.fill(UINT8_C(0xCD));
            const InterleavedU8View source = source_view(width, height, format);
            if (convert_to_luma(source, destination_view(scalar_pixels), LumaPath::scalar) !=
                    SACCADE_OK ||
                !verify_reference(scalar_pixels, width, height, format) ||
                convert_to_luma(source, destination_view(selected_pixels), LumaPath::automatic) !=
                    SACCADE_OK ||
                !equal_active_pixels(scalar_pixels, selected_pixels, width, height) ||
                !padding_is_unchanged(selected_pixels, width, height)) {
                return 3;
            }

            for (LumaPath path : {LumaPath::neon, LumaPath::avx2}) {
                selected_pixels.fill(UINT8_C(0xCD));
                const SaccadeResult result =
                    convert_to_luma(source, destination_view(selected_pixels), path);
                if (luma_path_available(path)) {
                    if (result != SACCADE_OK ||
                        !equal_active_pixels(scalar_pixels, selected_pixels, width, height) ||
                        !padding_is_unchanged(selected_pixels, width, height)) {
                        return 4;
                    }
                } else if (result != SACCADE_ERROR_UNSUPPORTED) {
                    return 5;
                }
            }
        }
    }

    for (uint32_t format : formats) {
        if (!exact_tails_match(selected_luma_path(), format)) {
            return 20;
        }
    }

    std::array<uint8_t, 16> primaries{0, 0, 255, 9, 0, 255, 0, 8, 255, 0, 0, 7, 255, 255, 255, 6};
    std::array<uint8_t, 4> primary_luma{};
    const InterleavedU8View primary_source{primaries.data(),    primaries.size(), 4, 1, 16,
                                           SACCADE_FORMAT_BGRA8};
    const PlaneU8View primary_destination{primary_luma.data(), primary_luma.size(), 4};
    if (convert_to_luma(primary_source, primary_destination, LumaPath::scalar) != SACCADE_OK ||
        primary_luma != std::array<uint8_t, 4>{77, 149, 29, 255}) {
        return 6;
    }

    std::array<uint8_t, 16> r8{1, 2, 3, 4, 0xEE, 0xEE, 0xEE, 0xEE,
                               5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE};
    std::array<uint8_t, 12> copied{};
    const InterleavedU8View r8_source{r8.data(), r8.size(), 4, 2, 8, SACCADE_FORMAT_R8};
    const PlaneU8View r8_destination{copied.data(), copied.size(), 6};
    if (convert_to_luma(r8_source, r8_destination, LumaPath::automatic) != SACCADE_OK ||
        copied != std::array<uint8_t, 12>{1, 2, 3, 4, 0, 0, 5, 6, 7, 8, 0, 0}) {
        return 7;
    }
    const PlaneU8View in_place{r8.data(), r8.size(), 8};
    if (convert_to_luma(r8_source, in_place, LumaPath::automatic) != SACCADE_OK) {
        return 8;
    }

    InterleavedU8View invalid_source = primary_source;
    PlaneU8View invalid_destination = primary_destination;
    invalid_source.data = nullptr;
    for (LumaPath path : {LumaPath::automatic, LumaPath::scalar, LumaPath::neon, LumaPath::avx2}) {
        if (convert_to_luma(invalid_source, primary_destination, path) !=
            SACCADE_ERROR_INVALID_ARGUMENT) {
            return 9;
        }
    }
    invalid_source = primary_source;
    invalid_source.size = 15;
    if (convert_to_luma(invalid_source, primary_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 10;
    }
    invalid_source = primary_source;
    invalid_source.row_stride_bytes = 15;
    if (convert_to_luma(invalid_source, primary_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 11;
    }
    invalid_source = primary_source;
    invalid_source.pixel_format = SACCADE_FORMAT_RGB_F16;
    if (convert_to_luma(invalid_source, primary_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 12;
    }
    invalid_destination.data = nullptr;
    if (convert_to_luma(primary_source, invalid_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 13;
    }
    invalid_destination = primary_destination;
    invalid_destination.size = 3;
    if (convert_to_luma(primary_source, invalid_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 14;
    }
    invalid_destination = primary_destination;
    invalid_destination.row_stride_bytes = 3;
    if (convert_to_luma(primary_source, invalid_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 15;
    }
    invalid_destination = {const_cast<uint8_t*>(primary_source.data), primary_source.size,
                           primary_source.width};
    if (convert_to_luma(primary_source, invalid_destination, LumaPath::scalar) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 16;
    }
    if (convert_to_luma(primary_source, primary_destination,
                        static_cast<LumaPath>(UINT8_C(0xFF))) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 17;
    }

    fill_source(67, 19, SACCADE_FORMAT_BGRA8);
    const InterleavedU8View measured_source = source_view(67, 19, SACCADE_FORMAT_BGRA8);
    const PlaneU8View measured_destination = destination_view(selected_pixels);

    std::atomic<uint32_t> first_use_gate{0};
    SaccadeResult first_use_result = SACCADE_ERROR_STATE;
    std::thread first_use_worker([&]() noexcept {
        while (first_use_gate.load(std::memory_order_acquire) == 0) {
        }
        first_use_result =
            convert_to_luma(measured_source, measured_destination, LumaPath::automatic);
        first_use_gate.store(2, std::memory_order_release);
    });
    saccade::test::begin_allocation_tracking();
    first_use_gate.store(1, std::memory_order_release);
    while (first_use_gate.load(std::memory_order_acquire) != 2) {
    }
    const size_t first_use_allocations = saccade::test::end_allocation_tracking();
    first_use_worker.join();
    if (first_use_result != SACCADE_OK || first_use_allocations != 0) {
        return 21;
    }

    saccade::test::begin_allocation_tracking();
    SaccadeResult measured_result = SACCADE_OK;
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        measured_result =
            convert_to_luma(measured_source, measured_destination, LumaPath::automatic);
        if (measured_result != SACCADE_OK) {
            break;
        }
    }
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (measured_result != SACCADE_OK || allocations != 0) {
        return 18;
    }

    return 0;
}

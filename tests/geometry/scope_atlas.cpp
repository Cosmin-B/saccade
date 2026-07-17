#include "geometry/scope_atlas.hpp"

#include <array>
#include <cstdint>

namespace {

constexpr int32_t q8(int32_t value) noexcept {
    return value << saccade::geometry::coordinate_fraction_bits;
}

bool rect_is(const saccade::geometry::PixelRect& rect, uint32_t x, uint32_t y, uint32_t width,
             uint32_t height) noexcept {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

} // namespace

int main() {
    using namespace saccade::geometry;

    const std::array<AtlasSurface, 2> side_by_side = {
        AtlasSurface{{q8(-1920), 0, q8(1920), q8(1080)}, 1920, 1080},
        AtlasSurface{{0, 0, q8(1920), q8(1080)}, 1920, 1080},
    };
    ScopeAtlasLayout layout{};
    if (make_scope_atlas_layout({q8(-1920), 0, q8(3840), q8(1080)}, 1280, 768, side_by_side.data(),
                                static_cast<uint32_t>(side_by_side.size()), &layout) != SACCADE_OK ||
        layout.count != 2 || !rect_is(layout.content, 0, 204, 1280, 360) ||
        !rect_is(layout.placements[0].source, 0, 0, 1920, 1080) ||
        !rect_is(layout.placements[0].destination, 0, 204, 640, 360) ||
        !rect_is(layout.placements[1].source, 0, 0, 1920, 1080) ||
        !rect_is(layout.placements[1].destination, 640, 204, 640, 360)) {
        return 1;
    }

    const AtlasSurface retina{{0, 0, q8(1920), q8(1080)}, 3840, 2160};
    if (make_scope_atlas_layout({q8(480), q8(270), q8(960), q8(540)}, 1280, 768, &retina, 1, &layout) != SACCADE_OK ||
        layout.count != 1 || !rect_is(layout.content, 0, 24, 1280, 720) ||
        !rect_is(layout.placements[0].source, 960, 540, 1920, 1080) ||
        !rect_is(layout.placements[0].destination, 0, 24, 1280, 720)) {
        return 2;
    }

    const std::array<AtlasSurface, 2> crossing = {
        AtlasSurface{{0, 0, q8(1920), q8(1080)}, 1920, 1080},
        AtlasSurface{{q8(1920), 0, q8(2560), q8(1440)}, 2560, 1440},
    };
    if (make_scope_atlas_layout({q8(1600), q8(100), q8(640), q8(400)}, 1280, 768, crossing.data(),
                                static_cast<uint32_t>(crossing.size()), &layout) != SACCADE_OK ||
        layout.count != 2 || !rect_is(layout.content, 26, 0, 1228, 768) ||
        !rect_is(layout.placements[0].source, 1600, 100, 320, 400) ||
        !rect_is(layout.placements[0].destination, 26, 0, 614, 768) ||
        !rect_is(layout.placements[1].source, 0, 100, 320, 400) ||
        !rect_is(layout.placements[1].destination, 640, 0, 614, 768)) {
        return 3;
    }

    const AtlasSurface outside{{q8(3000), 0, q8(1000), q8(1000)}, 1000, 1000};
    if (make_scope_atlas_layout({0, 0, q8(1000), q8(1000)}, 640, 640, &outside, 1, &layout) !=
        SACCADE_ERROR_NOT_FOUND) {
        return 4;
    }

    if (make_scope_atlas_layout({}, 1280, 768, side_by_side.data(), static_cast<uint32_t>(side_by_side.size()),
                                &layout) != SACCADE_ERROR_INVALID_ARGUMENT ||
        make_scope_atlas_layout({0, 0, q8(100), q8(100)}, 1280, 768, nullptr, 0, &layout) !=
            SACCADE_ERROR_INVALID_ARGUMENT) {
        return 5;
    }

    return 0;
}

#ifndef SACCADE_GEOMETRY_SCOPE_ATLAS_HPP
#define SACCADE_GEOMETRY_SCOPE_ATLAS_HPP

#include "geometry/coordinate_transform.hpp"
#include "geometry/display_catalog.hpp"

#include <array>
#include <cstdint>

namespace saccade::geometry {

struct PixelRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct AtlasSurface {
    RectQ8 desktop_bounds{};
    uint32_t image_width = 0;
    uint32_t image_height = 0;
};

struct AtlasPlacement {
    PixelRect source{};
    PixelRect destination{};
    uint32_t surface_index = 0;
    uint32_t reserved = 0;
};

struct ScopeAtlasLayout {
    PixelRect content{};
    uint32_t count = 0;
    uint32_t reserved = 0;
    std::array<AtlasPlacement, display_capacity> placements{};
};

static_assert(sizeof(PixelRect) == 16);
static_assert(sizeof(AtlasSurface) == 24);
static_assert(sizeof(AtlasPlacement) == 40);
static_assert(sizeof(ScopeAtlasLayout) == 664);

SaccadeResult make_scope_atlas_layout(const RectQ8& scope, uint32_t output_width, uint32_t output_height,
                                      const AtlasSurface* surfaces, uint32_t surface_count,
                                      ScopeAtlasLayout* output) noexcept;

} // namespace saccade::geometry

#endif

#include "geometry/scope_atlas.hpp"

#include <algorithm>
#include <cstdint>

namespace saccade::geometry {
namespace {

uint32_t scaled_floor(uint64_t value, uint32_t destination_extent, uint32_t source_extent) noexcept {
    return static_cast<uint32_t>((value * destination_extent) / source_extent);
}

uint32_t scaled_ceil(uint64_t value, uint32_t destination_extent, uint32_t source_extent) noexcept {
    const uint64_t product = value * destination_extent;
    return static_cast<uint32_t>((product + source_extent - 1U) / source_extent);
}

PixelRect content_rect(const RectQ8& scope, uint32_t output_width, uint32_t output_height) noexcept {
    const uint64_t scope_width = static_cast<uint32_t>(scope.width);
    const uint64_t scope_height = static_cast<uint32_t>(scope.height);
    uint32_t content_width = output_width;
    uint32_t content_height = output_height;

    if (scope_width * output_height > scope_height * output_width) {
        content_height = std::max<uint32_t>(1, static_cast<uint32_t>((scope_height * output_width) / scope_width));
    } else {
        content_width = std::max<uint32_t>(1, static_cast<uint32_t>((scope_width * output_height) / scope_height));
    }

    return {(output_width - content_width) / 2U, (output_height - content_height) / 2U, content_width, content_height};
}

bool intersection(const RectQ8& left, const RectQ8& right, RectQ8* output) noexcept {
    const int64_t left_right = static_cast<int64_t>(left.x) + left.width;
    const int64_t left_bottom = static_cast<int64_t>(left.y) + left.height;
    const int64_t right_right = static_cast<int64_t>(right.x) + right.width;
    const int64_t right_bottom = static_cast<int64_t>(right.y) + right.height;
    const int64_t x = std::max<int64_t>(left.x, right.x);
    const int64_t y = std::max<int64_t>(left.y, right.y);
    const int64_t edge_x = std::min(left_right, right_right);
    const int64_t edge_y = std::min(left_bottom, right_bottom);

    if (x >= edge_x || y >= edge_y) return false;
    *output = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(edge_x - x),
               static_cast<int32_t>(edge_y - y)};
    return true;
}

PixelRect map_rect(const RectQ8& rect, const RectQ8& source, const PixelRect& destination) noexcept {
    const uint64_t relative_left = static_cast<uint64_t>(static_cast<int64_t>(rect.x) - source.x);
    const uint64_t relative_top = static_cast<uint64_t>(static_cast<int64_t>(rect.y) - source.y);
    const uint64_t relative_right = relative_left + static_cast<uint32_t>(rect.width);
    const uint64_t relative_bottom = relative_top + static_cast<uint32_t>(rect.height);
    const uint32_t source_width = static_cast<uint32_t>(source.width);
    const uint32_t source_height = static_cast<uint32_t>(source.height);
    const uint32_t left = scaled_floor(relative_left, destination.width, source_width);
    const uint32_t top = scaled_floor(relative_top, destination.height, source_height);
    const uint32_t right = scaled_ceil(relative_right, destination.width, source_width);
    const uint32_t bottom = scaled_ceil(relative_bottom, destination.height, source_height);

    return {destination.x + left, destination.y + top, right - left, bottom - top};
}

} // namespace

SaccadeResult make_scope_atlas_layout(const RectQ8& scope, uint32_t output_width, uint32_t output_height,
                                      const AtlasSurface* surfaces, uint32_t surface_count,
                                      ScopeAtlasLayout* output) noexcept {
    if (!rect_valid(scope) || output_width == 0 || output_height == 0 ||
        output_width > (static_cast<uint32_t>(INT32_MAX) >> coordinate_fraction_bits) ||
        output_height > (static_cast<uint32_t>(INT32_MAX) >> coordinate_fraction_bits) || surfaces == nullptr ||
        surface_count == 0 || surface_count > display_capacity || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    ScopeAtlasLayout result{};
    result.content = content_rect(scope, output_width, output_height);

    for (uint32_t index = 0; index < surface_count; ++index) {
        const AtlasSurface& surface = surfaces[index];
        if (!rect_valid(surface.desktop_bounds) || surface.image_width == 0 || surface.image_height == 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        RectQ8 visible{};
        if (!intersection(scope, surface.desktop_bounds, &visible)) continue;

        const PixelRect source_bounds{0, 0, surface.image_width, surface.image_height};
        AtlasPlacement& placement = result.placements[result.count++];
        placement.source = map_rect(visible, surface.desktop_bounds, source_bounds);
        placement.destination = map_rect(visible, scope, result.content);
        placement.surface_index = index;
    }

    if (result.count == 0) return SACCADE_ERROR_NOT_FOUND;
    *output = result;
    return SACCADE_OK;
}

} // namespace saccade::geometry

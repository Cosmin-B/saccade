#include "geometry/coordinate_transform.hpp"

#include <algorithm>
#include <cstdint>

namespace saccade::geometry {
namespace {

constexpr uint32_t scale_fraction_bits = 32;
constexpr uint64_t scale_fraction_mask = UINT64_C(0xFFFFFFFF);
constexpr uint64_t scale_half = UINT64_C(1) << 31;

bool space_valid(CoordinateSpace space) noexcept {
    return space == CoordinateSpace::capture || space == CoordinateSpace::desktop ||
           space == CoordinateSpace::surface || space == CoordinateSpace::window;
}

bool rotation_valid(QuarterTurn rotation) noexcept {
    return rotation == QuarterTurn::clockwise_0 || rotation == QuarterTurn::clockwise_90 ||
           rotation == QuarterTurn::clockwise_180 || rotation == QuarterTurn::clockwise_270;
}

bool rect_edges(const RectQ8& rect, int64_t* right, int64_t* bottom) noexcept {
    if (rect.width <= 0 || rect.height <= 0 || right == nullptr || bottom == nullptr) {
        return false;
    }
    *right = static_cast<int64_t>(rect.x) + rect.width;
    *bottom = static_cast<int64_t>(rect.y) + rect.height;
    return *right <= INT32_MAX && *right >= INT32_MIN && *bottom <= INT32_MAX && *bottom >= INT32_MIN;
}

void rotate_point(QuarterTurn rotation, uint32_t source_width, uint32_t source_height, uint32_t x, uint32_t y,
                  uint32_t* rotated_x, uint32_t* rotated_y) noexcept {
    switch (rotation) {
    case QuarterTurn::clockwise_0:
        *rotated_x = x;
        *rotated_y = y;
        return;
    case QuarterTurn::clockwise_90:
        *rotated_x = source_height - y;
        *rotated_y = x;
        return;
    case QuarterTurn::clockwise_180:
        *rotated_x = source_width - x;
        *rotated_y = source_height - y;
        return;
    case QuarterTurn::clockwise_270:
        *rotated_x = y;
        *rotated_y = source_width - x;
        return;
    }
}

void rotate_rect(QuarterTurn rotation, uint32_t source_width, uint32_t source_height, uint32_t left, uint32_t top,
                 uint32_t right, uint32_t bottom, uint32_t* rotated_left, uint32_t* rotated_top,
                 uint32_t* rotated_right, uint32_t* rotated_bottom) noexcept {
    switch (rotation) {
    case QuarterTurn::clockwise_0:
        *rotated_left = left;
        *rotated_top = top;
        *rotated_right = right;
        *rotated_bottom = bottom;
        return;
    case QuarterTurn::clockwise_90:
        *rotated_left = source_height - bottom;
        *rotated_top = left;
        *rotated_right = source_height - top;
        *rotated_bottom = right;
        return;
    case QuarterTurn::clockwise_180:
        *rotated_left = source_width - right;
        *rotated_top = source_height - bottom;
        *rotated_right = source_width - left;
        *rotated_bottom = source_height - top;
        return;
    case QuarterTurn::clockwise_270:
        *rotated_left = top;
        *rotated_top = source_width - right;
        *rotated_right = bottom;
        *rotated_bottom = source_width - left;
        return;
    }
}

} // namespace

CoordinateTransform::Scale CoordinateTransform::make_scale(int32_t source_extent, int32_t destination_extent) noexcept {
    const uint64_t source = static_cast<uint64_t>(source_extent);
    const uint64_t numerator = static_cast<uint64_t>(destination_extent) << scale_fraction_bits;
    return {(numerator / source), ((numerator + source - 1U) / source), ((numerator + source / 2U) / source),
            source_extent, destination_extent};
}

uint32_t CoordinateTransform::scaled_floor(uint32_t value, const Scale& scale) noexcept {
    if (value == 0) {
        return 0;
    }
    if (value >= static_cast<uint32_t>(scale.source_extent_)) {
        return static_cast<uint32_t>(scale.destination_extent_);
    }
    return static_cast<uint32_t>((static_cast<uint64_t>(value) * scale.floor_q32_) >> scale_fraction_bits);
}

uint32_t CoordinateTransform::scaled_ceil(uint32_t value, const Scale& scale) noexcept {
    if (value == 0) {
        return 0;
    }
    if (value >= static_cast<uint32_t>(scale.source_extent_)) {
        return static_cast<uint32_t>(scale.destination_extent_);
    }
    const uint64_t product = static_cast<uint64_t>(value) * scale.ceil_q32_;
    const uint64_t rounded = (product + scale_fraction_mask) >> scale_fraction_bits;
    return static_cast<uint32_t>(std::min<uint64_t>(rounded, static_cast<uint64_t>(scale.destination_extent_)));
}

uint32_t CoordinateTransform::scaled_nearest(uint32_t value, const Scale& scale) noexcept {
    if (value == 0) {
        return 0;
    }
    if (value >= static_cast<uint32_t>(scale.source_extent_)) {
        return static_cast<uint32_t>(scale.destination_extent_);
    }
    const uint64_t product = static_cast<uint64_t>(value) * scale.nearest_q32_;
    const uint64_t rounded = (product + scale_half) >> scale_fraction_bits;
    return static_cast<uint32_t>(std::min<uint64_t>(rounded, static_cast<uint64_t>(scale.destination_extent_)));
}

bool rect_valid(const RectQ8& rect) noexcept {
    int64_t right = 0;
    int64_t bottom = 0;
    return rect_edges(rect, &right, &bottom);
}

bool rect_contains(const RectQ8& outer, const RectQ8& inner) noexcept {
    int64_t outer_right = 0;
    int64_t outer_bottom = 0;
    int64_t inner_right = 0;
    int64_t inner_bottom = 0;
    return rect_edges(outer, &outer_right, &outer_bottom) && rect_edges(inner, &inner_right, &inner_bottom) &&
           inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right && inner_bottom <= outer_bottom;
}

SaccadeResult CoordinateTransform::initialize(const TransformDesc& desc) noexcept {
    valid_ = false;
    desc_ = {};
    x_scale_ = {};
    y_scale_ = {};
    if (!rect_valid(desc.source) || !rect_valid(desc.destination) || !space_valid(desc.source_space) ||
        !space_valid(desc.destination_space) || !rotation_valid(desc.rotation) || desc.flags != 0 ||
        desc.reserved != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const bool swaps_axes = desc.rotation == QuarterTurn::clockwise_90 || desc.rotation == QuarterTurn::clockwise_270;
    const int32_t rotated_width = swaps_axes ? desc.source.height : desc.source.width;
    const int32_t rotated_height = swaps_axes ? desc.source.width : desc.source.height;
    desc_ = desc;
    x_scale_ = make_scale(rotated_width, desc.destination.width);
    y_scale_ = make_scale(rotated_height, desc.destination.height);
    valid_ = true;
    return SACCADE_OK;
}

SaccadeResult CoordinateTransform::map_point(PointQ8 source, PointQ8* destination) const noexcept {
    if (!valid_ || destination == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const int64_t local_x = static_cast<int64_t>(source.x) - desc_.source.x;
    const int64_t local_y = static_cast<int64_t>(source.y) - desc_.source.y;
    if (local_x < 0 || local_y < 0 || local_x > desc_.source.width || local_y > desc_.source.height) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t rotated_x = 0;
    uint32_t rotated_y = 0;
    rotate_point(desc_.rotation, static_cast<uint32_t>(desc_.source.width), static_cast<uint32_t>(desc_.source.height),
                 static_cast<uint32_t>(local_x), static_cast<uint32_t>(local_y), &rotated_x, &rotated_y);
    const int64_t output_x = static_cast<int64_t>(desc_.destination.x) + scaled_nearest(rotated_x, x_scale_);
    const int64_t output_y = static_cast<int64_t>(desc_.destination.y) + scaled_nearest(rotated_y, y_scale_);
    if (output_x < INT32_MIN || output_x > INT32_MAX || output_y < INT32_MIN || output_y > INT32_MAX) {
        return SACCADE_ERROR_STATE;
    }
    *destination = {static_cast<int32_t>(output_x), static_cast<int32_t>(output_y)};
    return SACCADE_OK;
}

SaccadeResult CoordinateTransform::map_rect_clipped(const RectQ8& source, RectQ8* destination) const noexcept {
    if (!valid_ || destination == nullptr || !rect_valid(source)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const int64_t source_right = static_cast<int64_t>(source.x) + source.width;
    const int64_t source_bottom = static_cast<int64_t>(source.y) + source.height;
    const int64_t transform_right = static_cast<int64_t>(desc_.source.x) + desc_.source.width;
    const int64_t transform_bottom = static_cast<int64_t>(desc_.source.y) + desc_.source.height;
    const int64_t clipped_left = std::max<int64_t>(source.x, desc_.source.x);
    const int64_t clipped_top = std::max<int64_t>(source.y, desc_.source.y);
    const int64_t clipped_right = std::min(source_right, transform_right);
    const int64_t clipped_bottom = std::min(source_bottom, transform_bottom);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return SACCADE_ERROR_NOT_FOUND;
    }

    const uint32_t left = static_cast<uint32_t>(clipped_left - desc_.source.x);
    const uint32_t top = static_cast<uint32_t>(clipped_top - desc_.source.y);
    const uint32_t right = static_cast<uint32_t>(clipped_right - desc_.source.x);
    const uint32_t bottom = static_cast<uint32_t>(clipped_bottom - desc_.source.y);
    uint32_t rotated_left = 0;
    uint32_t rotated_top = 0;
    uint32_t rotated_right = 0;
    uint32_t rotated_bottom = 0;
    rotate_rect(desc_.rotation, static_cast<uint32_t>(desc_.source.width), static_cast<uint32_t>(desc_.source.height),
                left, top, right, bottom, &rotated_left, &rotated_top, &rotated_right, &rotated_bottom);

    const uint32_t output_left = scaled_floor(rotated_left, x_scale_);
    const uint32_t output_top = scaled_floor(rotated_top, y_scale_);
    const uint32_t output_right = scaled_ceil(rotated_right, x_scale_);
    const uint32_t output_bottom = scaled_ceil(rotated_bottom, y_scale_);
    const int64_t x = static_cast<int64_t>(desc_.destination.x) + output_left;
    const int64_t y = static_cast<int64_t>(desc_.destination.y) + output_top;
    const int64_t width = static_cast<int64_t>(output_right) - output_left;
    const int64_t height = static_cast<int64_t>(output_bottom) - output_top;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || width <= 0 || width > INT32_MAX ||
        height <= 0 || height > INT32_MAX) {
        return SACCADE_ERROR_STATE;
    }
    *destination = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(width),
                    static_cast<int32_t>(height)};
    return SACCADE_OK;
}

SaccadeResult CoordinateTransform::inverse(CoordinateTransform* destination) const noexcept {
    if (!valid_ || destination == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t rotation = static_cast<uint32_t>(desc_.rotation);
    TransformDesc inverse_desc{};
    inverse_desc.source = desc_.destination;
    inverse_desc.destination = desc_.source;
    inverse_desc.epoch = desc_.epoch;
    inverse_desc.source_space = desc_.destination_space;
    inverse_desc.destination_space = desc_.source_space;
    inverse_desc.rotation = static_cast<QuarterTurn>((4U - rotation) & 3U);
    return destination->initialize(inverse_desc);
}

} // namespace saccade::geometry

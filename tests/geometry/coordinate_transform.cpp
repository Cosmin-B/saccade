#include "../support/allocation_tracker.hpp"
#include "geometry/coordinate_transform.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr int32_t q8(int32_t value) noexcept {
    return value * saccade::geometry::coordinate_one;
}

bool point_is(const saccade::geometry::PointQ8& point, int32_t x, int32_t y) noexcept {
    return point.x == x && point.y == y;
}

bool maps_corner(saccade::geometry::CoordinateTransform* transform, saccade::geometry::PointQ8 source, int32_t x,
                 int32_t y) noexcept {
    saccade::geometry::PointQ8 mapped{};
    return transform->map_point(source, &mapped) == SACCADE_OK && point_is(mapped, x, y);
}

void rotate_edges(saccade::geometry::QuarterTurn rotation, int32_t source_width, int32_t source_height, int32_t left,
                  int32_t top, int32_t right, int32_t bottom, int32_t* rotated_left, int32_t* rotated_top,
                  int32_t* rotated_right, int32_t* rotated_bottom) noexcept {
    switch (rotation) {
    case saccade::geometry::QuarterTurn::clockwise_0:
        *rotated_left = left;
        *rotated_top = top;
        *rotated_right = right;
        *rotated_bottom = bottom;
        return;
    case saccade::geometry::QuarterTurn::clockwise_90:
        *rotated_left = source_height - bottom;
        *rotated_top = left;
        *rotated_right = source_height - top;
        *rotated_bottom = right;
        return;
    case saccade::geometry::QuarterTurn::clockwise_180:
        *rotated_left = source_width - right;
        *rotated_top = source_height - bottom;
        *rotated_right = source_width - left;
        *rotated_bottom = source_height - top;
        return;
    case saccade::geometry::QuarterTurn::clockwise_270:
        *rotated_left = top;
        *rotated_top = source_width - right;
        *rotated_right = bottom;
        *rotated_bottom = source_width - left;
        return;
    }
}

} // namespace

int main() {
    using namespace saccade::geometry;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    CoordinateTransform transform;
    TransformDesc invalid{};
    invalid.source = {0, 0, q8(100), q8(100)};
    invalid.destination = {0, 0, q8(100), q8(100)};
    invalid.flags = 1;
    if (transform.initialize(invalid) != SACCADE_ERROR_INVALID_ARGUMENT || transform.valid()) {
        return 2;
    }
    invalid.flags = 0;
    invalid.source.width = 0;
    if (transform.initialize(invalid) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 3;
    }
    invalid.source.width = q8(100);
    invalid.source_space = static_cast<CoordinateSpace>(99);
    if (transform.initialize(invalid) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 3;
    }
    invalid.source_space = CoordinateSpace::capture;
    invalid.rotation = static_cast<QuarterTurn>(99);
    if (transform.initialize(invalid) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 3;
    }
    invalid.rotation = QuarterTurn::clockwise_0;
    invalid.reserved = 1;
    if (transform.initialize(invalid) != SACCADE_ERROR_INVALID_ARGUMENT || rect_valid({INT32_MAX - 1, 0, 2, 1})) {
        return 3;
    }

    TransformDesc identity{};
    identity.source = {q8(-300), q8(-120), q8(600), q8(240)};
    identity.destination = identity.source;
    identity.epoch = 9;
    identity.source_space = CoordinateSpace::desktop;
    identity.destination_space = CoordinateSpace::surface;
    if (transform.initialize(identity) != SACCADE_OK || !transform.valid() ||
        !maps_corner(&transform, {q8(-300), q8(-120)}, q8(-300), q8(-120)) ||
        !maps_corner(&transform, {q8(300), q8(120)}, q8(300), q8(120))) {
        return 4;
    }

    PointQ8 rejected{};
    if (transform.map_point({q8(301), 0}, &rejected) != SACCADE_ERROR_INVALID_ARGUMENT ||
        transform.map_point({}, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 5;
    }

    TransformDesc rotated{};
    rotated.source = {0, 0, q8(100), q8(200)};
    rotated.destination = {q8(-50), q8(25), q8(400), q8(100)};
    rotated.epoch = 17;
    rotated.source_space = CoordinateSpace::capture;
    rotated.destination_space = CoordinateSpace::desktop;
    rotated.rotation = QuarterTurn::clockwise_90;
    if (transform.initialize(rotated) != SACCADE_OK || !maps_corner(&transform, {0, 0}, q8(350), q8(25)) ||
        !maps_corner(&transform, {q8(100), 0}, q8(350), q8(125)) ||
        !maps_corner(&transform, {0, q8(200)}, q8(-50), q8(25)) ||
        !maps_corner(&transform, {q8(100), q8(200)}, q8(-50), q8(125))) {
        return 6;
    }

    RectQ8 mapped{};
    if (transform.map_rect_clipped({q8(-10), q8(50), q8(30), q8(40)}, &mapped) != SACCADE_OK || !rect_valid(mapped) ||
        !rect_contains(rotated.destination, mapped)) {
        return 7;
    }
    if (transform.map_rect_clipped({q8(-20), q8(-20), q8(10), q8(10)}, &mapped) != SACCADE_ERROR_NOT_FOUND ||
        transform.map_rect_clipped({}, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 8;
    }

    TransformDesc fractional{};
    fractional.source = {0, 0, q8(3), q8(7)};
    fractional.destination = {0, 0, q8(10), q8(5)};
    fractional.epoch = 21;
    fractional.source_space = CoordinateSpace::capture;
    fractional.destination_space = CoordinateSpace::surface;
    if (transform.initialize(fractional) != SACCADE_OK ||
        transform.map_rect_clipped({q8(1), q8(2), q8(1), q8(3)}, &mapped) != SACCADE_OK) {
        return 9;
    }
    const int64_t left_exact_numerator = static_cast<int64_t>(q8(1)) * q8(10);
    const int64_t right_exact_numerator = static_cast<int64_t>(q8(2)) * q8(10);
    if (static_cast<int64_t>(mapped.x) * q8(3) > left_exact_numerator ||
        static_cast<int64_t>(mapped.x + mapped.width) * q8(3) < right_exact_numerator) {
        return 10;
    }

    CoordinateTransform inverse;
    if (transform.inverse(&inverse) != SACCADE_OK || inverse.descriptor().epoch != fractional.epoch ||
        inverse.descriptor().rotation != QuarterTurn::clockwise_0 ||
        inverse.descriptor().source_space != fractional.destination_space ||
        inverse.descriptor().destination_space != fractional.source_space) {
        return 11;
    }
    for (int32_t y = 0; y <= q8(7); y += 19) {
        for (int32_t x = 0; x <= q8(3); x += 13) {
            PointQ8 forward{};
            PointQ8 back{};
            if (transform.map_point({x, y}, &forward) != SACCADE_OK ||
                inverse.map_point(forward, &back) != SACCADE_OK || std::abs(back.x - x) > 2 ||
                std::abs(back.y - y) > 3) {
                return 12;
            }
        }
    }

    constexpr std::array<QuarterTurn, 4> rotations{QuarterTurn::clockwise_0, QuarterTurn::clockwise_90,
                                                   QuarterTurn::clockwise_180, QuarterTurn::clockwise_270};
    for (QuarterTurn rotation : rotations) {
        TransformDesc desc{};
        desc.source = {q8(-40), q8(10), q8(80), q8(60)};
        desc.destination = {q8(100), q8(-200), q8(120), q8(160)};
        desc.epoch = 31;
        desc.source_space = CoordinateSpace::desktop;
        desc.destination_space = CoordinateSpace::window;
        desc.rotation = rotation;
        CoordinateTransform forward;
        CoordinateTransform back;
        PointQ8 middle{};
        PointQ8 restored{};
        if (forward.initialize(desc) != SACCADE_OK || forward.inverse(&back) != SACCADE_OK ||
            forward.map_point({0, q8(40)}, &middle) != SACCADE_OK || back.map_point(middle, &restored) != SACCADE_OK ||
            std::abs(restored.x) > 1 || std::abs(restored.y - q8(40)) > 1) {
            return 13;
        }
    }

    constexpr std::array<int32_t, 5> source_extents{1, 3, 255, 768, 1792};
    constexpr std::array<int32_t, 5> destination_extents{1, 5, 511, 1280, 2560};
    for (QuarterTurn rotation : rotations) {
        for (int32_t source_width : source_extents) {
            for (int32_t source_height : source_extents) {
                for (int32_t destination_width : destination_extents) {
                    const int32_t destination_height =
                        destination_extents[static_cast<size_t>(source_width) % destination_extents.size()];
                    TransformDesc desc{};
                    desc.source = {-1000, -2000, source_width, source_height};
                    desc.destination = {500, -300, destination_width, destination_height};
                    desc.epoch = 33;
                    desc.source_space = CoordinateSpace::capture;
                    desc.destination_space = CoordinateSpace::surface;
                    desc.rotation = rotation;
                    CoordinateTransform checked;
                    if (checked.initialize(desc) != SACCADE_OK) {
                        return 14;
                    }
                    const int32_t x_step = std::max(1, source_width / 7);
                    const int32_t y_step = std::max(1, source_height / 7);
                    for (int32_t top = 0; top < source_height; top += y_step) {
                        for (int32_t left = 0; left < source_width; left += x_step) {
                            const int32_t right = std::min(source_width, left + x_step + 1);
                            const int32_t bottom = std::min(source_height, top + y_step + 1);
                            RectQ8 output{};
                            if (checked.map_rect_clipped(
                                    {desc.source.x + left, desc.source.y + top, right - left, bottom - top}, &output) !=
                                    SACCADE_OK ||
                                !rect_contains(desc.destination, output)) {
                                return 15;
                            }
                            int32_t rotated_left = 0;
                            int32_t rotated_top = 0;
                            int32_t rotated_right = 0;
                            int32_t rotated_bottom = 0;
                            rotate_edges(rotation, source_width, source_height, left, top, right, bottom, &rotated_left,
                                         &rotated_top, &rotated_right, &rotated_bottom);
                            const bool swaps =
                                rotation == QuarterTurn::clockwise_90 || rotation == QuarterTurn::clockwise_270;
                            const int32_t rotated_width = swaps ? source_height : source_width;
                            const int32_t rotated_height = swaps ? source_width : source_height;
                            const int32_t output_left = output.x - desc.destination.x;
                            const int32_t output_top = output.y - desc.destination.y;
                            const int32_t output_right = output_left + output.width;
                            const int32_t output_bottom = output_top + output.height;
                            if (static_cast<int64_t>(output_left) * rotated_width >
                                    static_cast<int64_t>(rotated_left) * destination_width ||
                                static_cast<int64_t>(output_right) * rotated_width <
                                    static_cast<int64_t>(rotated_right) * destination_width ||
                                static_cast<int64_t>(output_top) * rotated_height >
                                    static_cast<int64_t>(rotated_top) * destination_height ||
                                static_cast<int64_t>(output_bottom) * rotated_height <
                                    static_cast<int64_t>(rotated_bottom) * destination_height) {
                                return 16;
                            }
                        }
                    }
                }
            }
        }
    }

    TransformDesc measured{};
    measured.source = {q8(-2000), q8(-1000), q8(8000), q8(4500)};
    measured.destination = {0, 0, q8(7680), q8(4320)};
    measured.epoch = 44;
    measured.source_space = CoordinateSpace::desktop;
    measured.destination_space = CoordinateSpace::surface;
    if (transform.initialize(measured) != SACCADE_OK) {
        return 17;
    }
    RectQ8 output{};
    saccade::test::begin_allocation_tracking();
    for (uint32_t index = 0; index < 10000; ++index) {
        const RectQ8 input{q8(-2000) + static_cast<int32_t>(index % 100U) * q8(40),
                           q8(-1000) + static_cast<int32_t>(index / 100U) * q8(20), q8(12), q8(8)};
        if (transform.map_rect_clipped(input, &output) != SACCADE_OK) {
            return 18;
        }
    }
    return saccade::test::end_allocation_tracking() == 0 ? 0 : 19;
}

#include "../support/allocation_tracker.hpp"
#include "geometry/display_catalog.hpp"

#include <array>
#include <cstdint>

namespace {

constexpr int32_t q8(int32_t value) noexcept {
    return value * saccade::geometry::coordinate_one;
}

saccade::geometry::DisplaySurface display(uint64_t id, int32_t x, uint32_t width, uint32_t height) noexcept {
    saccade::geometry::DisplaySurface result{};
    result.display_id = id;
    result.desktop_bounds = {q8(x), 0, q8(static_cast<int32_t>(width)), q8(static_cast<int32_t>(height))};
    result.work_bounds = result.desktop_bounds;
    result.backing_width = width * 2U;
    result.backing_height = height * 2U;
    result.maximum_fps = 120;
    result.flags = saccade::geometry::display_surface_active;
    return result;
}

} // namespace

int main() {
    using namespace saccade::geometry;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    DisplayCatalog catalog;
    std::array<DisplaySurface, 2> displays{display(9, 1512, 1920, 1080), display(3, 0, 1512, 982)};
    displays[1].flags |= display_surface_main | display_surface_builtin;
    if (catalog.publish(displays.data(), static_cast<uint32_t>(displays.size()), display_topology_separate_spaces) !=
        SACCADE_OK) {
        return 2;
    }
    const DisplaySnapshot& first = catalog.snapshot();
    if (first.epoch != 1 || first.count != displays.size() || first.flags != display_topology_separate_spaces ||
        first.displays[0].display_id != 3 || first.displays[1].display_id != 9 ||
        catalog.find(3) != &first.displays[0] || catalog.find(99) != nullptr) {
        return 3;
    }

    const uint64_t unchanged_epoch = first.epoch;
    std::array<DisplaySurface, 2> reordered{displays[1], displays[0]};
    if (catalog.publish(reordered.data(), static_cast<uint32_t>(reordered.size()), display_topology_separate_spaces) !=
            SACCADE_OK ||
        catalog.snapshot().epoch != unchanged_epoch) {
        return 4;
    }

    reordered[0].work_bounds.y += q8(32);
    reordered[0].work_bounds.height -= q8(32);
    if (catalog.publish(reordered.data(), static_cast<uint32_t>(reordered.size()), display_topology_separate_spaces) !=
            SACCADE_OK ||
        catalog.snapshot().epoch != unchanged_epoch + 1U) {
        return 5;
    }

    CoordinateTransform transform;
    const DisplaySurface* main = catalog.find(3);
    if (main == nullptr ||
        make_desktop_to_surface_transform(*main, catalog.snapshot().epoch, &transform) != SACCADE_OK ||
        transform.descriptor().source_space != CoordinateSpace::desktop ||
        transform.descriptor().destination_space != CoordinateSpace::surface ||
        transform.descriptor().destination.width !=
            static_cast<int32_t>(main->backing_width << coordinate_fraction_bits)) {
        return 6;
    }

    PointQ8 bottom_right{};
    const int32_t source_right = main->desktop_bounds.x + main->desktop_bounds.width;
    const int32_t source_bottom = main->desktop_bounds.y + main->desktop_bounds.height;
    if (transform.map_point({source_right, source_bottom}, &bottom_right) != SACCADE_OK ||
        bottom_right.x != static_cast<int32_t>(main->backing_width << coordinate_fraction_bits) ||
        bottom_right.y != static_cast<int32_t>(main->backing_height << coordinate_fraction_bits)) {
        return 7;
    }

    DisplaySurface rotated = *main;
    rotated.rotation = QuarterTurn::clockwise_90;
    CoordinateTransform presented_transform;
    if (make_desktop_to_surface_transform(rotated, catalog.snapshot().epoch, &presented_transform) != SACCADE_OK ||
        presented_transform.descriptor().rotation != QuarterTurn::clockwise_0) {
        return 13;
    }

    const DisplaySnapshot before_invalid = catalog.snapshot();
    reordered[1].display_id = reordered[0].display_id;
    if (catalog.publish(reordered.data(), static_cast<uint32_t>(reordered.size()), 0) !=
            SACCADE_ERROR_INVALID_ARGUMENT ||
        catalog.snapshot().epoch != before_invalid.epoch || catalog.snapshot().count != before_invalid.count) {
        return 8;
    }

    DisplaySurface invalid_surface = display(3, 0, 1512, 982);
    invalid_surface.flags |= display_surface_main;
    invalid_surface.safe_insets.left = invalid_surface.desktop_bounds.width;
    invalid_surface.safe_insets.right = 1;
    if (catalog.publish(&invalid_surface, 1, 0) != SACCADE_ERROR_INVALID_ARGUMENT ||
        catalog.publish(&invalid_surface, 1, UINT32_C(0x80000000)) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 9;
    }

    std::array<DisplaySurface, display_capacity + 1> too_many{};
    if (catalog.publish(too_many.data(), static_cast<uint32_t>(too_many.size()), 0) != SACCADE_ERROR_CAPACITY ||
        catalog.publish(nullptr, 1, 0) != SACCADE_ERROR_INVALID_ARGUMENT ||
        catalog.publish(nullptr, 0, 0) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 10;
    }

    DisplaySurface removed = display(3, 0, 1512, 982);
    removed.flags |= display_surface_main | display_surface_builtin;
    removed.flags |= display_surface_asleep;
    saccade::test::begin_allocation_tracking();
    if (catalog.publish(&removed, 1, 0) != SACCADE_OK || catalog.snapshot().count != 1 ||
        catalog.snapshot().epoch != 3) {
        return 11;
    }
    return saccade::test::end_allocation_tracking() == 0 ? 0 : 12;
}

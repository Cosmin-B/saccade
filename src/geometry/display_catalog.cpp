#include "geometry/display_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace saccade::geometry {
namespace {

constexpr uint32_t surface_flag_mask = display_surface_main | display_surface_builtin | display_surface_active |
                                       display_surface_asleep | display_surface_mirrored;
constexpr uint32_t topology_flag_mask = display_topology_separate_spaces;
constexpr uint32_t maximum_backing_extent = static_cast<uint32_t>(INT32_MAX) >> coordinate_fraction_bits;

bool rotation_valid(QuarterTurn rotation) noexcept {
    return rotation == QuarterTurn::clockwise_0 || rotation == QuarterTurn::clockwise_90 ||
           rotation == QuarterTurn::clockwise_180 || rotation == QuarterTurn::clockwise_270;
}

bool surface_valid(const DisplaySurface& surface) noexcept {
    return surface.display_id != 0 && rect_valid(surface.desktop_bounds) && rect_valid(surface.work_bounds) &&
           rect_contains(surface.desktop_bounds, surface.work_bounds) && surface.safe_insets.top >= 0 &&
           surface.safe_insets.left >= 0 && surface.safe_insets.bottom >= 0 && surface.safe_insets.right >= 0 &&
           static_cast<int64_t>(surface.safe_insets.left) + surface.safe_insets.right <= surface.desktop_bounds.width &&
           static_cast<int64_t>(surface.safe_insets.top) + surface.safe_insets.bottom <=
               surface.desktop_bounds.height &&
           surface.backing_width != 0 && surface.backing_height != 0 &&
           surface.backing_width <= maximum_backing_extent && surface.backing_height <= maximum_backing_extent &&
           surface.maximum_fps != 0 && rotation_valid(surface.rotation) && (surface.flags & ~surface_flag_mask) == 0 &&
           surface.reserved == 0;
}

void sort_displays(DisplaySurface* displays, uint32_t count) noexcept {
    for (uint32_t index = 1; index < count; ++index) {
        const DisplaySurface value = displays[index];
        uint32_t destination = index;
        while (destination != 0 && displays[destination - 1U].display_id > value.display_id) {
            displays[destination] = displays[destination - 1U];
            --destination;
        }
        displays[destination] = value;
    }
}

} // namespace

SaccadeResult DisplayCatalog::publish(const DisplaySurface* displays, uint32_t count, uint32_t flags) noexcept {
    if (count > display_capacity) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (displays == nullptr || count == 0 || (flags & ~topology_flag_mask) != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    DisplaySnapshot candidate{};
    candidate.count = count;
    candidate.flags = flags;
    uint32_t main_count = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (!surface_valid(displays[index])) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        candidate.displays[index] = displays[index];
        main_count += (displays[index].flags & display_surface_main) != 0 ? 1U : 0U;
    }
    if (main_count != 1) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    sort_displays(candidate.displays.data(), count);
    for (uint32_t index = 1; index < count; ++index) {
        if (candidate.displays[index - 1U].display_id == candidate.displays[index].display_id) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }

    const bool unchanged =
        snapshot_.count == candidate.count && snapshot_.flags == candidate.flags &&
        std::memcmp(snapshot_.displays.data(), candidate.displays.data(), sizeof(DisplaySurface) * count) == 0;
    if (unchanged) {
        return SACCADE_OK;
    }
    if (next_epoch_ == 0) {
        return SACCADE_ERROR_STATE;
    }
    candidate.epoch = next_epoch_++;
    snapshot_ = candidate;
    return SACCADE_OK;
}

const DisplaySurface* DisplayCatalog::find(uint64_t display_id) const noexcept {
    uint32_t first = 0;
    uint32_t count = snapshot_.count;
    while (count != 0) {
        const uint32_t step = count / 2U;
        const uint32_t index = first + step;
        const uint64_t candidate = snapshot_.displays[index].display_id;
        if (candidate < display_id) {
            first = index + 1U;
            count -= step + 1U;
        } else if (candidate > display_id) {
            count = step;
        } else {
            return &snapshot_.displays[index];
        }
    }
    return nullptr;
}

SaccadeResult make_desktop_to_surface_transform(const DisplaySurface& display, uint64_t epoch,
                                                CoordinateTransform* output) noexcept {
    if (!surface_valid(display) || epoch == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    TransformDesc desc{};
    desc.source = display.desktop_bounds;
    desc.destination = {0, 0, static_cast<int32_t>(display.backing_width << coordinate_fraction_bits),
                        static_cast<int32_t>(display.backing_height << coordinate_fraction_bits)};
    desc.epoch = epoch;
    desc.source_space = CoordinateSpace::desktop;
    desc.destination_space = CoordinateSpace::surface;
    // AppKit drawable dimensions are already in the display's presented orientation.
    // Physical rotation remains topology metadata for capture correlation.
    desc.rotation = QuarterTurn::clockwise_0;
    return output->initialize(desc);
}

} // namespace saccade::geometry

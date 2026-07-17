#ifndef SACCADE_GEOMETRY_DISPLAY_CATALOG_HPP
#define SACCADE_GEOMETRY_DISPLAY_CATALOG_HPP

#include "geometry/coordinate_transform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::geometry {

constexpr uint32_t display_capacity = 16;

enum : uint32_t {
    display_surface_main = UINT32_C(0x00000001),
    display_surface_builtin = UINT32_C(0x00000002),
    display_surface_active = UINT32_C(0x00000004),
    display_surface_asleep = UINT32_C(0x00000008),
    display_surface_mirrored = UINT32_C(0x00000010)
};

enum : uint32_t { display_topology_separate_spaces = UINT32_C(0x00000001) };

struct InsetsQ8 {
    int32_t top = 0;
    int32_t left = 0;
    int32_t bottom = 0;
    int32_t right = 0;
};

static_assert(sizeof(InsetsQ8) == 16);

struct DisplaySurface {
    uint64_t display_id = 0;
    RectQ8 desktop_bounds{};
    RectQ8 work_bounds{};
    InsetsQ8 safe_insets{};
    uint32_t backing_width = 0;
    uint32_t backing_height = 0;
    uint32_t maximum_fps = 0;
    QuarterTurn rotation = QuarterTurn::clockwise_0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct DisplaySnapshot {
    uint64_t epoch = 0;
    uint32_t count = 0;
    uint32_t flags = 0;
    std::array<DisplaySurface, display_capacity> displays{};
};

static_assert(sizeof(DisplaySurface) == 80);
static_assert(sizeof(DisplaySnapshot) == 1296);

class DisplayCatalog final {
  public:
    SaccadeResult publish(const DisplaySurface* displays, uint32_t count, uint32_t flags) noexcept;

    [[nodiscard]] const DisplaySnapshot& snapshot() const noexcept { return snapshot_; }

    [[nodiscard]] const DisplaySurface* find(uint64_t display_id) const noexcept;

  private:
    DisplaySnapshot snapshot_{};
    uint64_t next_epoch_ = 1;
};

SaccadeResult make_desktop_to_surface_transform(const DisplaySurface& display, uint64_t epoch,
                                                CoordinateTransform* output) noexcept;

} // namespace saccade::geometry

#endif

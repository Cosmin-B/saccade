#ifndef SACCADE_GEOMETRY_COORDINATE_TRANSFORM_HPP
#define SACCADE_GEOMETRY_COORDINATE_TRANSFORM_HPP

#include <saccade/saccade.h>

#include <cstdint>

namespace saccade::geometry {

constexpr uint32_t coordinate_fraction_bits = 8;
constexpr int32_t coordinate_one = INT32_C(1) << coordinate_fraction_bits;

enum class CoordinateSpace : uint32_t { capture = 1, desktop = 2, surface = 3, window = 4 };

enum class QuarterTurn : uint32_t { clockwise_0 = 0, clockwise_90 = 1, clockwise_180 = 2, clockwise_270 = 3 };

struct PointQ8 {
    int32_t x = 0;
    int32_t y = 0;
};

struct RectQ8 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct TransformDesc {
    RectQ8 source{};
    RectQ8 destination{};
    uint64_t epoch = 0;
    CoordinateSpace source_space = CoordinateSpace::capture;
    CoordinateSpace destination_space = CoordinateSpace::desktop;
    QuarterTurn rotation = QuarterTurn::clockwise_0;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

static_assert(sizeof(PointQ8) == 8);
static_assert(sizeof(RectQ8) == 16);
static_assert(sizeof(TransformDesc) == 64);

[[nodiscard]] bool rect_valid(const RectQ8&) noexcept;
[[nodiscard]] bool rect_contains(const RectQ8& outer, const RectQ8& inner) noexcept;

class CoordinateTransform final {
  public:
    CoordinateTransform() noexcept = default;

    SaccadeResult initialize(const TransformDesc&) noexcept;
    SaccadeResult map_point(PointQ8 source, PointQ8* destination) const noexcept;
    SaccadeResult map_rect_clipped(const RectQ8& source, RectQ8* destination) const noexcept;
    SaccadeResult inverse(CoordinateTransform* destination) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] const TransformDesc& descriptor() const noexcept { return desc_; }

  private:
    struct Scale {
        uint64_t floor_q32_ = 0;
        uint64_t ceil_q32_ = 0;
        uint64_t nearest_q32_ = 0;
        int32_t source_extent_ = 0;
        int32_t destination_extent_ = 0;
    };

    static Scale make_scale(int32_t source_extent, int32_t destination_extent) noexcept;
    static uint32_t scaled_floor(uint32_t value, const Scale&) noexcept;
    static uint32_t scaled_ceil(uint32_t value, const Scale&) noexcept;
    static uint32_t scaled_nearest(uint32_t value, const Scale&) noexcept;

    TransformDesc desc_{};
    Scale x_scale_{};
    Scale y_scale_{};
    bool valid_ = false;
};

} // namespace saccade::geometry

#endif

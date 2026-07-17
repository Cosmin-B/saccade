#ifndef SACCADE_SCENE_GRID_HPP
#define SACCADE_SCENE_GRID_HPP

#include "geometry/coordinate_transform.hpp"
#include "scene/packet.hpp"

#include <cstddef>
#include <cstdint>

namespace saccade::scene {

struct GridSceneConfig {
    geometry::RectQ8 scope{};
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
    uint64_t display_id = 0;
    uint16_t rows = 0;
    uint16_t columns = 0;
    uint16_t reserved0 = 0;
    uint16_t reserved1 = 0;
    int32_t margin_x_q8 = 0;
    int32_t margin_y_q8 = 0;
};

SaccadeResult build_grid_scene(const GridSceneConfig&, SaccadeMutableSpanU8, size_t*) noexcept;

static_assert(sizeof(GridSceneConfig) == 96);

} // namespace saccade::scene

#endif

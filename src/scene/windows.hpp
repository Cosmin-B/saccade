#ifndef SACCADE_SCENE_WINDOWS_HPP
#define SACCADE_SCENE_WINDOWS_HPP

#include <saccade/saccade_backend.h>

#include <cstddef>
#include <cstdint>

namespace saccade::scene {

struct WindowSceneConfig {
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
};

SaccadeResult build_window_scene(const WindowSceneConfig&, const SaccadeWindowInfo*, uint32_t count,
                                 SaccadeMutableSpanU8, size_t*) noexcept;

static_assert(sizeof(WindowSceneConfig) == 56);

} // namespace saccade::scene

#endif

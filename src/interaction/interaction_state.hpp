#ifndef SACCADE_INTERACTION_INTERACTION_STATE_HPP
#define SACCADE_INTERACTION_INTERACTION_STATE_HPP

#include "geometry/coordinate_transform.hpp"

#include <saccade/saccade.h>

#include <cstdint>

namespace saccade::interaction {

/* Read-only interaction snapshot shared by the desktop host and the agent
   service. Both depend on this header so the agent module never includes the
   application layer. */
struct InteractionState {
    uint64_t scene_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t permission_epoch = 0;
    uint64_t focus_id = 0;
    uint32_t permissions = 0;
    uint32_t expected_buttons = 0;
    int32_t pointer_x_q8 = 0;
    int32_t pointer_y_q8 = 0;
    uint64_t window_id = 0;
    uint64_t display_id = 0;
    geometry::RectQ8 window_bounds{};
};

using ReadInteractionStateFn = SaccadeResult (*)(void*, InteractionState*) noexcept;

} // namespace saccade::interaction

#endif

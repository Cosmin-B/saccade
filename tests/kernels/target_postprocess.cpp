#include "kernels/targets/postprocess.hpp"
#include "scene/packet.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstdint>

int main() {
    using namespace saccade::kernels::targets;
    const std::array<DenseCandidate, 7> candidates{
        {{0, 0, 80, 80, 65000, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
         {8, 8, 16, 16, 64000, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
         {4, 4, 80, 80, 63000, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
         {160, 0, 32, 32, 62000, SACCADE_TARGET_ROLE_LINK, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
         {240, 0, 32, 32, 61000, SACCADE_TARGET_ROLE_TEXT_FIELD, SACCADE_TARGET_SOURCE_NEURAL,
          SACCADE_TARGET_ACTIONABLE, 0},
         {320, 0, 32, 32, 1000, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
         {400, 0, 32, 32, 65535, SACCADE_TARGET_ROLE_UNKNOWN, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
          0}}};
    PostprocessConfig config{};
    config.maximum_targets = 5;
    config.minimum_confidence_q16 = 2000;
    config.iou_threshold_q16 = 32768;
    PostprocessEpochs epochs{1, 2, 3, 4, 5, 6};
    static PostprocessWorkspace workspace;
    alignas(8) std::array<uint8_t, 4096> output{};
    size_t required = 0;
    PostprocessStats stats{};
    saccade::test::begin_allocation_tracking();
    const SaccadeResult result = postprocess(candidates.data(), static_cast<uint32_t>(candidates.size()), config,
                                             epochs, &workspace, {output.data(), output.size()}, &required, &stats);
    const size_t allocations = saccade::test::end_allocation_tracking();
    saccade::scene::PacketView packet{};
    if (result != SACCADE_OK || allocations != 0 ||
        saccade::scene::validate_packet({output.data(), required}, &packet) != SACCADE_OK ||
        packet.header->target_count != 3 || packet.targets[0].confidence_q16 != 65535 ||
        packet.targets[0].capability_bits !=
            (SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
             SACCADE_TARGET_CAPABILITY_SCROLL | SACCADE_TARGET_CAPABILITY_DRAG_SOURCE |
             SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
             SACCADE_TARGET_CAPABILITY_TEXT_SELECT) ||
        packet.targets[1].confidence_q16 != 65000 || packet.targets[2].confidence_q16 != 62000 ||
        packet.targets[2].role != SACCADE_TARGET_ROLE_LINK ||
        packet.targets[2].capability_bits != (SACCADE_TARGET_CAPABILITY_POINTER_MOVE |
                                              SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_INVOKE) ||
        stats.candidates_read != candidates.size() || stats.candidates_above_threshold != 6 ||
        stats.heap_replacements != 1 || stats.containment_suppressed != 1 || stats.targets_written != 3) {
        return 1;
    }

    size_t small_required = 0;
    if (postprocess(candidates.data(), static_cast<uint32_t>(candidates.size()), config, epochs, &workspace,
                    {output.data(), 8}, &small_required, &stats) != SACCADE_ERROR_CAPACITY ||
        small_required != sizeof(SaccadeTargetPacketHeader) + 5U * sizeof(SaccadeTargetRecord)) {
        return 2;
    }

    const std::array<DenseCandidate, 4> band_candidates{{
        {0, 160, 80, 80, 1500, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE, 0},
        {160, 160, 128, 160, 1500, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
         0},
        {400, 160, 200, 240, 1500, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
         0},
        {720, 160, 80, 80, 2500, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
         0},
    }};
    config.maximum_targets = static_cast<uint32_t>(band_candidates.size());
    config.band_minimum_confidence_q16 = 1000;
    config.band_min_short_side_q3 = 96;
    config.band_max_short_side_q3 = 192;
    if (postprocess(band_candidates.data(), static_cast<uint32_t>(band_candidates.size()), config, epochs, &workspace,
                    {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        saccade::scene::validate_packet({output.data(), required}, &packet) != SACCADE_OK ||
        packet.header->target_count != 2 || packet.targets[0].confidence_q16 != 2500 ||
        packet.targets[1].confidence_q16 != 1500 || stats.candidates_above_threshold != 2) {
        return 3;
    }
    config.band_max_short_side_q3 = 0;
    if (postprocess(band_candidates.data(), static_cast<uint32_t>(band_candidates.size()), config, epochs, &workspace,
                    {output.data(), output.size()}, &required, &stats) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 4;
    }

    const DenseCandidate tiny{
        880, 160, 16, 16, 65535, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    config.band_minimum_confidence_q16 = 0;
    config.band_min_short_side_q3 = 0;
    config.maximum_targets = 1;
    if (postprocess(&tiny, 1, config, epochs, &workspace, {output.data(), output.size()}, &required, &stats) !=
            SACCADE_OK ||
        saccade::scene::validate_packet({output.data(), required}, &packet) != SACCADE_OK ||
        packet.header->target_count != 1 || packet.targets[0].safe_x_q8 != (888 << 5) ||
        packet.targets[0].safe_y_q8 != (168 << 5) || packet.targets[0].capability_bits != 0 ||
        (packet.targets[0].flags & SACCADE_TARGET_ACTIONABLE) != 0 || has_safe_interior(tiny)) {
        return 5;
    }
    return 0;
}

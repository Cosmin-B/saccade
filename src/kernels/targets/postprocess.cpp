#include "kernels/targets/postprocess.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace saccade::kernels::targets {
namespace {

constexpr uint32_t generic_visual_capabilities =
    SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_SCROLL |
    SACCADE_TARGET_CAPABILITY_DRAG_SOURCE | SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT |
    SACCADE_TARGET_CAPABILITY_TEXT_SELECT;

constexpr int32_t q8(uint16_t value) noexcept {
    return static_cast<int32_t>(value) << 5;
}

bool better(const DenseCandidate* candidates, uint32_t left, uint32_t right) noexcept {
    const DenseCandidate& a = candidates[left];
    const DenseCandidate& b = candidates[right];
    if (a.confidence_q16 != b.confidence_q16) {
        return a.confidence_q16 > b.confidence_q16;
    }
    if (a.y_q3 != b.y_q3) {
        return a.y_q3 < b.y_q3;
    }
    if (a.x_q3 != b.x_q3) {
        return a.x_q3 < b.x_q3;
    }
    return left < right;
}

void sift_up(const DenseCandidate* candidates, uint32_t* heap, uint32_t index) noexcept {
    while (index != 0) {
        const uint32_t parent = (index - 1U) / 2U;
        if (!better(candidates, heap[parent], heap[index])) {
            break;
        }
        std::swap(heap[parent], heap[index]);
        index = parent;
    }
}

void sift_down(const DenseCandidate* candidates, uint32_t* heap, uint32_t count, uint32_t index) noexcept {
    for (;;) {
        const uint32_t left = index * 2U + 1U;
        if (left >= count) {
            return;
        }
        const uint32_t right = left + 1U;
        uint32_t worse = left;
        if (right < count && better(candidates, heap[left], heap[right])) {
            worse = right;
        }
        if (!better(candidates, heap[index], heap[worse])) {
            return;
        }
        std::swap(heap[index], heap[worse]);
        index = worse;
    }
}

uint64_t target_id(uint64_t source_id, const DenseCandidate& candidate) noexcept {
    uint64_t hash = UINT64_C(14695981039346656037);
    const std::array<uint64_t, 7> values{source_id,
                                         static_cast<uint32_t>(q8(candidate.x_q3)),
                                         static_cast<uint32_t>(q8(candidate.y_q3)),
                                         static_cast<uint32_t>(q8(candidate.width_q3)),
                                         static_cast<uint32_t>(q8(candidate.height_q3)),
                                         candidate.role,
                                         candidate.source_bits};
    for (uint64_t value : values) {
        for (uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8U));
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash == 0 ? 1 : hash;
}

uint64_t area(const DenseCandidate& value) noexcept {
    return static_cast<uint64_t>(value.width_q3) * static_cast<uint64_t>(value.height_q3);
}

uint32_t capabilities_for_role(SaccadeTargetRole role) noexcept {
    switch (role) {
    case SACCADE_TARGET_ROLE_BUTTON:
    case SACCADE_TARGET_ROLE_LINK:
    case SACCADE_TARGET_ROLE_CHECKBOX:
    case SACCADE_TARGET_ROLE_RADIO:
    case SACCADE_TARGET_ROLE_MENU_ITEM:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
               SACCADE_TARGET_CAPABILITY_INVOKE;
    case SACCADE_TARGET_ROLE_TEXT:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    case SACCADE_TARGET_ROLE_TEXT_FIELD:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
               SACCADE_TARGET_CAPABILITY_TEXT | SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    case SACCADE_TARGET_ROLE_SLIDER:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
               SACCADE_TARGET_CAPABILITY_SCROLL;
    case SACCADE_TARGET_ROLE_WINDOW:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
    case SACCADE_TARGET_ROLE_IMAGE:
        return SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    case SACCADE_TARGET_ROLE_UNKNOWN:
        return generic_visual_capabilities;
    }
    return 0;
}

uint64_t intersection(const DenseCandidate& a, const DenseCandidate& b) noexcept {
    const int64_t left = std::max<int64_t>(a.x_q3, b.x_q3);
    const int64_t top = std::max<int64_t>(a.y_q3, b.y_q3);
    const int64_t right =
        std::min<int64_t>(static_cast<int64_t>(a.x_q3) + a.width_q3, static_cast<int64_t>(b.x_q3) + b.width_q3);
    const int64_t bottom =
        std::min<int64_t>(static_cast<int64_t>(a.y_q3) + a.height_q3, static_cast<int64_t>(b.y_q3) + b.height_q3);
    if (left >= right || top >= bottom) {
        return 0;
    }
    return static_cast<uint64_t>(right - left) * static_cast<uint64_t>(bottom - top);
}

} // namespace

SaccadeResult postprocess(const DenseCandidate* candidates, uint32_t candidate_count, const PostprocessConfig& config,
                          const PostprocessEpochs& epochs, PostprocessWorkspace* workspace, SaccadeMutableSpanU8 output,
                          size_t* required, PostprocessStats* stats) noexcept {
    if ((candidate_count != 0 && candidates == nullptr) || workspace == nullptr || required == nullptr ||
        stats == nullptr || candidate_count > maximum_candidates || config.maximum_targets == 0 ||
        config.maximum_targets > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        config.coordinate_space == SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        (config.coordinate_space != SACCADE_COORDINATE_SPACE_MODEL_Q8 &&
         config.coordinate_space != SACCADE_COORDINATE_SPACE_SOURCE_Q8) ||
        !confidence_band_valid(config) || config.reserved != 0 || epochs.frame_id == 0 || epochs.model_epoch == 0 ||
        epochs.session_epoch == 0 || epochs.transform_epoch == 0 || epochs.topology_epoch == 0 ||
        epochs.source_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *stats = {};
    stats->candidates_read = candidate_count;
    uint32_t heap_size = 0;
    for (uint32_t index = 0; index < candidate_count; ++index) {
        if (!above_confidence_threshold(candidates[index], config)) {
            continue;
        }
        ++stats->candidates_above_threshold;
        if (heap_size < config.maximum_targets) {
            workspace->indices[heap_size] = index;
            sift_up(candidates, workspace->indices.data(), heap_size++);
        } else if (better(candidates, index, workspace->indices[0])) {
            workspace->indices[0] = index;
            sift_down(candidates, workspace->indices.data(), heap_size, 0);
            ++stats->heap_replacements;
        }
    }
    for (uint32_t count = heap_size; count > 1; --count) {
        std::swap(workspace->indices[0], workspace->indices[count - 1U]);
        sift_down(candidates, workspace->indices.data(), count - 1U, 0);
    }

    const size_t maximum_required =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(heap_size) * sizeof(SaccadeTargetRecord);
    *required = maximum_required;
    if (output.data == nullptr || output.size < maximum_required) {
        return SACCADE_ERROR_CAPACITY;
    }
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = config.coordinate_space;
    header.frame_id = epochs.frame_id;
    header.model_epoch = epochs.model_epoch;
    header.session_epoch = epochs.session_epoch;
    header.transform_epoch = epochs.transform_epoch;
    header.topology_epoch = epochs.topology_epoch;
    header.source_id = epochs.source_id;
    header.targets_offset = sizeof(header);
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(output.data + sizeof(header));

    for (uint32_t ordered = 0; ordered < heap_size; ++ordered) {
        const DenseCandidate& candidate = candidates[workspace->indices[ordered]];
        bool suppressed = false;
        const uint64_t candidate_area = area(candidate);
        for (uint32_t accepted = 0; accepted < header.target_count; ++accepted) {
            ++stats->overlap_tests;
            const SaccadeTargetRecord& target = targets[accepted];
            const DenseCandidate existing{static_cast<uint16_t>(target.x_q8 >> 5),
                                          static_cast<uint16_t>(target.y_q8 >> 5),
                                          static_cast<uint16_t>(target.width_q8 >> 5),
                                          static_cast<uint16_t>(target.height_q8 >> 5),
                                          static_cast<uint16_t>(target.confidence_q16),
                                          static_cast<uint8_t>(target.role),
                                          static_cast<uint8_t>(target.source_bits),
                                          static_cast<uint16_t>(target.flags),
                                          0};
            const uint64_t overlap = intersection(candidate, existing);
            if (overlap == candidate_area) {
                ++stats->containment_suppressed;
                suppressed = true;
                break;
            }
            if (overlap == 0) {
                continue;
            }
            const uint64_t union_area = candidate_area + area(existing) - overlap;
            if (overlap * UINT16_MAX >= union_area * config.iou_threshold_q16) {
                ++stats->iou_suppressed;
                suppressed = true;
                break;
            }
        }
        if (suppressed) {
            continue;
        }
        SaccadeTargetRecord target{};
        target.target_id = target_id(epochs.source_id, candidate);
        target.x_q8 = q8(candidate.x_q3);
        target.y_q8 = q8(candidate.y_q3);
        target.width_q8 = q8(candidate.width_q3);
        target.height_q8 = q8(candidate.height_q3);
        target.safe_x_q8 = target.x_q8 + target.width_q8 / 2;
        target.safe_y_q8 = target.y_q8 + target.height_q8 / 2;
        target.confidence_q16 = candidate.confidence_q16;
        target.role = candidate.role;
        target.source_bits = candidate.source_bits;
        target.flags = candidate.flags & ~static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE);
        if ((candidate.flags & SACCADE_TARGET_ACTIONABLE) != 0 && has_safe_interior(candidate) &&
            (target.flags & (SACCADE_TARGET_DISABLED | SACCADE_TARGET_SECURE)) == 0) {
            target.capability_bits = capabilities_for_role(target.role);
            if (target.capability_bits != 0) {
                target.flags |= SACCADE_TARGET_ACTIONABLE;
            }
        }
        target.order = header.target_count;
        targets[header.target_count++] = target;
    }
    header.total_size = sizeof(header) + static_cast<uint64_t>(header.target_count) * sizeof(SaccadeTargetRecord);
    std::memcpy(output.data, &header, sizeof(header));
    *required = static_cast<size_t>(header.total_size);
    stats->targets_written = header.target_count;
    return SACCADE_OK;
}

} // namespace saccade::kernels::targets

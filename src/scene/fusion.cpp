#include "scene/fusion.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace saccade::scene {
namespace {

constexpr uint32_t empty_node = UINT32_MAX;
constexpr uint32_t bucket_mask = fusion_bucket_count - 1;
constexpr uint8_t minimum_level = 12;
constexpr uint8_t maximum_level = 30;

static_assert(std::has_single_bit(fusion_bucket_count));

uint8_t role_group(SaccadeTargetRole role) noexcept {
    switch (role) {
    case SACCADE_TARGET_ROLE_BUTTON:
    case SACCADE_TARGET_ROLE_LINK:
    case SACCADE_TARGET_ROLE_CHECKBOX:
    case SACCADE_TARGET_ROLE_RADIO:
    case SACCADE_TARGET_ROLE_MENU_ITEM:
        return 1;
    case SACCADE_TARGET_ROLE_TEXT:
    case SACCADE_TARGET_ROLE_TEXT_FIELD:
        return 2;
    case SACCADE_TARGET_ROLE_SLIDER:
        return 3;
    case SACCADE_TARGET_ROLE_IMAGE:
        return 4;
    case SACCADE_TARGET_ROLE_WINDOW:
        return 5;
    default:
        return 0;
    }
}

uint8_t target_level(const SaccadeTargetRecord& target) noexcept {
    const uint32_t extent = static_cast<uint32_t>(std::max(target.width_q8, target.height_q8));
    const uint8_t level = static_cast<uint8_t>(std::bit_width(extent - 1U));
    return std::clamp(level, minimum_level, maximum_level);
}

int32_t floor_cell(int32_t coordinate, uint8_t level) noexcept {
    const int64_t divisor = INT64_C(1) << level;
    const int64_t value = coordinate;
    int64_t quotient = value / divisor;
    if (value < 0 && value % divisor != 0) {
        --quotient;
    }
    return static_cast<int32_t>(quotient);
}

uint32_t bucket_for(int32_t x, int32_t y, uint8_t level) noexcept {
    uint64_t hash = static_cast<uint32_t>(x);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(y)) * UINT64_C(0x9E3779B185EBCA87);
    hash ^= static_cast<uint64_t>(level) * UINT64_C(0xC2B2AE3D27D4EB4F);
    hash ^= hash >> 33U;
    hash *= UINT64_C(0xFF51AFD7ED558CCD);
    hash ^= hash >> 33U;
    return static_cast<uint32_t>(hash) & bucket_mask;
}

bool compatible_window(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    return (left.window_id == 0 || right.window_id == 0 || left.window_id == right.window_id) &&
           (left.display_id == 0 || right.display_id == 0 || left.display_id == right.display_id);
}

bool compatible_role(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    const uint8_t left_group = role_group(left.role);
    const uint8_t right_group = role_group(right.role);
    return left_group == 0 || right_group == 0 || left_group == right_group;
}

uint64_t area(const SaccadeTargetRecord& target) noexcept {
    return static_cast<uint64_t>(target.width_q8) * static_cast<uint64_t>(target.height_q8);
}

bool ratio_at_least(uint64_t numerator, uint64_t denominator, uint16_t threshold_q16) noexcept {
    const uint64_t quotient = denominator / UINT16_MAX;
    const uint64_t remainder = denominator % UINT16_MAX;
    const uint64_t required = quotient * threshold_q16 + (remainder * threshold_q16 + UINT16_MAX - 1U) / UINT16_MAX;
    return numerator >= required;
}

bool area_ratio_allowed(uint64_t smaller, uint64_t larger, uint16_t maximum_ratio_q8) noexcept {
    const uint64_t quotient = larger / smaller;
    const uint64_t remainder = larger % smaller;
    const uint64_t integer_limit = maximum_ratio_q8 / 256U;
    if (quotient < integer_limit) return true;
    if (quotient > integer_limit) return false;
    const uint64_t fraction = maximum_ratio_q8 % 256U;
    const uint64_t allowed_remainder = (smaller / 256U) * fraction + ((smaller % 256U) * fraction) / 256U;
    return remainder <= allowed_remainder;
}

bool duplicate(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right, const FusionConfig& config,
               FusionStats* stats) noexcept {
    if (!compatible_role(left, right) || !compatible_window(left, right)) {
        return false;
    }
    ++stats->overlap_tests;
    const int64_t left_right = static_cast<int64_t>(left.x_q8) + left.width_q8;
    const int64_t right_right = static_cast<int64_t>(right.x_q8) + right.width_q8;
    const int64_t left_bottom = static_cast<int64_t>(left.y_q8) + left.height_q8;
    const int64_t right_bottom = static_cast<int64_t>(right.y_q8) + right.height_q8;
    const int64_t width = std::min(left_right, right_right) - std::max<int64_t>(left.x_q8, right.x_q8);
    const int64_t height = std::min(left_bottom, right_bottom) - std::max<int64_t>(left.y_q8, right.y_q8);
    if (width <= 0 || height <= 0) {
        return false;
    }
    const uint64_t intersection = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t left_area = area(left);
    const uint64_t right_area = area(right);
    const uint64_t smaller = std::min(left_area, right_area);
    const uint64_t larger = std::max(left_area, right_area);
    const uint64_t union_area = left_area + right_area - intersection;
    return ratio_at_least(intersection, union_area, config.iou_threshold_q16) ||
           (area_ratio_allowed(smaller, larger, config.maximum_area_ratio_q8) &&
            ratio_at_least(intersection, smaller, config.containment_threshold_q16));
}

void merge_target(SaccadeTargetRecord* target, const SaccadeTargetRecord& candidate, FusionStats* stats) noexcept {
    const bool target_semantic =
        (target->source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) != 0 && target->role != SACCADE_TARGET_ROLE_UNKNOWN;
    const bool candidate_semantic = (candidate.source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) != 0 &&
                                    candidate.role != SACCADE_TARGET_ROLE_UNKNOWN;
    target->source_bits |= candidate.source_bits;
    target->confidence_q16 = std::max(target->confidence_q16, candidate.confidence_q16);
    if (target->role == SACCADE_TARGET_ROLE_UNKNOWN) {
        target->role = candidate.role;
    }
    if (target->parent_id == 0) target->parent_id = candidate.parent_id;
    if (target->window_id == 0) target->window_id = candidate.window_id;
    if (target->display_id == 0) target->display_id = candidate.display_id;
    if (target->text.size == 0 && candidate.text.size != 0) target->text = candidate.text;

    const uint32_t safety = SACCADE_TARGET_DISABLED | SACCADE_TARGET_SECURE;
    const uint32_t safety_flags = (target->flags | candidate.flags) & safety;
    if (safety_flags != 0) {
        target->flags |= safety_flags;
        target->flags &= ~static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE);
        target->capability_bits = 0;
        if ((safety_flags & SACCADE_TARGET_SECURE) != 0) {
            target->text = {};
            target->flags |= SACCADE_TARGET_TEXT_REDACTED;
            target->flags &= ~static_cast<uint32_t>(SACCADE_TARGET_TEXT_TRUNCATED);
        }
        ++stats->safety_merges;
    } else {
        if (candidate_semantic) {
            target->capability_bits = candidate.capability_bits;
        } else if (!target_semantic) {
            target->capability_bits |= candidate.capability_bits;
        }
        target->flags |= candidate.flags & (SACCADE_TARGET_OCCLUDED | SACCADE_TARGET_APPROXIMATE |
                                            SACCADE_TARGET_TEXT_REDACTED | SACCADE_TARGET_TEXT_TRUNCATED);
        if (target->capability_bits != 0) target->flags |= SACCADE_TARGET_ACTIONABLE;
    }
}

bool matching_node(const FusionNode& node, int32_t x, int32_t y, uint8_t level,
                   const SaccadeTargetRecord* output) noexcept {
    return node.level == level && floor_cell(output[node.target_index].safe_x_q8, level) == x &&
           floor_cell(output[node.target_index].safe_y_q8, level) == y;
}

uint32_t find_duplicate(const SaccadeTargetRecord& candidate, const SaccadeTargetRecord* output,
                        const FusionConfig& config, const FusionWorkspace& workspace, FusionStats* stats) noexcept {
    const uint8_t own_level = target_level(candidate);
    const int32_t center_x = floor_cell(candidate.safe_x_q8, own_level);
    const int32_t center_y = floor_cell(candidate.safe_y_q8, own_level);
    for (int32_t y = center_y - 1; y <= center_y + 1; ++y) {
        for (int32_t x = center_x - 1; x <= center_x + 1; ++x) {
            ++stats->bucket_visits;
            uint32_t node_index = workspace.heads[bucket_for(x, y, own_level)];
            while (node_index != empty_node) {
                const FusionNode& node = workspace.nodes[node_index];
                if (matching_node(node, x, y, own_level, output) &&
                    duplicate(output[node.target_index], candidate, config, stats)) {
                    return node.target_index;
                }
                node_index = node.next;
            }
        }
    }
    return empty_node;
}

void insert_nodes(uint32_t target_index, const SaccadeTargetRecord& target, FusionWorkspace* workspace,
                  uint32_t* node_count) noexcept {
    const uint8_t own_level = target_level(target);
    const uint8_t first_level = own_level > minimum_level + 1U ? own_level - 2U : minimum_level;
    const uint8_t last_level = std::min<uint8_t>(own_level + 2U, maximum_level);
    for (uint8_t level = first_level; level <= last_level; ++level) {
        FusionNode& node = workspace->nodes[(*node_count)++];
        node.target_index = static_cast<uint16_t>(target_index);
        node.level = level;
        const int32_t cell_x = floor_cell(target.safe_x_q8, level);
        const int32_t cell_y = floor_cell(target.safe_y_q8, level);
        const uint32_t bucket = bucket_for(cell_x, cell_y, level);
        node.next = workspace->heads[bucket];
        workspace->heads[bucket] = *node_count - 1U;
    }
}

bool epochs_match(const PacketView& packet, const FusionEpochs& epochs) noexcept {
    return packet.header != nullptr && packet.targets != nullptr &&
           packet.header->coordinate_space == SACCADE_COORDINATE_SPACE_DESKTOP_Q8 &&
           packet.header->frame_id == epochs.frame_id && packet.header->session_epoch == epochs.session_epoch &&
           packet.header->transform_epoch == epochs.transform_epoch &&
           packet.header->topology_epoch == epochs.topology_epoch;
}

} // namespace

SaccadeResult fuse(const PacketView* packets, uint32_t packet_count, const FusionConfig& config,
                   const FusionEpochs& epochs, FusionWorkspace* workspace, SaccadeMutableSpanU8 output,
                   size_t* required, FusionStats* stats) noexcept {
    if (packets == nullptr || packet_count == 0 || packet_count > 4 || workspace == nullptr || required == nullptr ||
        stats == nullptr || config.maximum_targets == 0 || config.maximum_targets > SACCADE_TARGET_PACKET_MAX_TARGETS ||
        config.iou_threshold_q16 == 0 || config.containment_threshold_q16 == 0 || config.maximum_area_ratio_q8 < 256 ||
        config.reserved != 0 || epochs.scene_epoch == 0 || epochs.frame_id == 0 || epochs.model_epoch == 0 ||
        epochs.session_epoch == 0 || epochs.transform_epoch == 0 || epochs.topology_epoch == 0 ||
        epochs.source_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t maximum_size = sizeof(SaccadeTargetPacketHeader) +
                                static_cast<size_t>(config.maximum_targets) * sizeof(SaccadeTargetRecord) +
                                SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;
    *required = maximum_size;
    if (output.data == nullptr || output.size < maximum_size ||
        (reinterpret_cast<uintptr_t>(output.data) & (alignof(SaccadeTargetPacketHeader) - 1U)) != 0) {
        return SACCADE_ERROR_CAPACITY;
    }
    for (uint32_t index = 0; index < packet_count; ++index) {
        if (!epochs_match(packets[index], epochs)) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
    }

    *stats = {};
    std::fill(workspace->heads.begin(), workspace->heads.end(), empty_node);
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(output.data + sizeof(SaccadeTargetPacketHeader));
    uint32_t target_count = 0;
    uint32_t node_count = 0;
    uint32_t packet_flags = 0;
    for (uint32_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        const PacketView& packet = packets[packet_index];
        packet_flags |= packet.header->flags;
        ++stats->packets_read;
        for (uint32_t index = 0; index < packet.header->target_count; ++index) {
            SaccadeTargetRecord candidate = packet.targets[index];
            candidate.text =
                packet.targets[index].text.size == 0
                    ? SaccadeTargetTextRef{}
                    : SaccadeTargetTextRef{static_cast<uint16_t>(packet_index + 1U), static_cast<uint16_t>(index + 1U)};
            ++stats->candidates_read;
            const uint32_t match =
                config.merge_duplicates ? find_duplicate(candidate, targets, config, *workspace, stats) : empty_node;
            if (match != empty_node) {
                merge_target(&targets[match], candidate, stats);
                ++stats->duplicates_merged;
                continue;
            }
            if (target_count == config.maximum_targets) {
                ++stats->capacity_drops;
                continue;
            }
            targets[target_count] = candidate;
            insert_nodes(target_count, targets[target_count], workspace, &node_count);
            ++target_count;
        }
    }

    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.flags = packet_flags;
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = epochs.scene_epoch;
    header.frame_id = epochs.frame_id;
    header.capture_time_ns = epochs.capture_time_ns;
    header.model_epoch = epochs.model_epoch;
    header.session_epoch = epochs.session_epoch;
    header.transform_epoch = epochs.transform_epoch;
    header.topology_epoch = epochs.topology_epoch;
    header.source_id = epochs.source_id;
    header.targets_offset = sizeof(header);
    const size_t text_base = sizeof(header) + static_cast<size_t>(target_count) * sizeof(SaccadeTargetRecord);
    uint32_t text_size = 0;
    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeTargetRecord& target = targets[index];
        const SaccadeTargetTextRef source = target.text;
        target.text = {};
        if (source.offset == 0 || source.size == 0) continue;
        const PacketView& packet = packets[source.offset - 1U];
        const SaccadeSpanU8 text = packet.target_text(source.size - 1U);
        if (text.size > SACCADE_TARGET_PACKET_MAX_TEXT_BYTES - text_size) {
            target.flags |= SACCADE_TARGET_TEXT_TRUNCATED;
            packet_flags |= SACCADE_TARGET_PACKET_TEXT_TRUNCATED;
            continue;
        }
        target.text = {static_cast<uint16_t>(text_size), static_cast<uint16_t>(text.size)};
        std::memcpy(output.data + text_base + text_size, text.data, text.size);
        text_size += static_cast<uint32_t>(text.size);
    }
    header.flags = packet_flags;
    header.total_size = text_base + text_size;
    std::memcpy(output.data, &header, sizeof(header));
    *required = static_cast<size_t>(header.total_size);
    stats->targets_written = target_count;
    return SACCADE_OK;
}

} // namespace saccade::scene

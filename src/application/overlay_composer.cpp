#include "application/overlay_composer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace saccade::application {
namespace {

constexpr uint32_t empty_index = UINT32_MAX;
constexpr int32_t q8_per_q3 = 32;
constexpr uint32_t placement_bucket_mask = overlay_placement_bucket_count - 1U;
constexpr uint32_t placement_candidate_count = 8;
constexpr uint32_t style_flag_mask = SACCADE_OVERLAY_STYLE_ANIMATED;

struct RectQ3 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

static_assert((overlay_placement_bucket_count & placement_bucket_mask) == 0);

bool style_usable(const SaccadeOverlayStyle& style) noexcept {
    return style.target_stroke_q3 != 0 && style.label_height_q3 != 0 && style.glyph_width_q3 != 0 &&
           style.glyph_height_q3 != 0 && style.glyph_advance_q3 >= style.glyph_width_q3 &&
           style.label_height_q3 >= style.glyph_height_q3 && style.active_stroke_q3 != 0 && style.reserved16 == 0 &&
           (style.flags & ~style_flag_mask) == 0 && style.reserved32 == 0 && style.reserved[0] == 0 &&
           style.reserved[1] == 0;
}

bool config_valid(const scene::PacketView& scene, const OverlayComposeConfig& config) noexcept {
    if (scene.header == nullptr || scene.targets == nullptr ||
        scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || config.display_id == 0 ||
        config.transform_epoch == 0 || config.desktop_to_surface == nullptr || config.styles == nullptr ||
        config.style_count == 0 || config.style_count > SACCADE_OVERLAY_MAX_STYLES || config.glyph_symbols == nullptr ||
        config.glyph_symbol_count == 0 || config.glyph_symbol_count > 32U || config.placement > LabelPlacement::right ||
        config.reserved[0] != 0 || config.reserved[1] != 0 || config.reserved[2] != 0 ||
        !config.desktop_to_surface->valid())
        return false;
    const geometry::TransformDesc& transform = config.desktop_to_surface->descriptor();
    if (transform.epoch != config.transform_epoch || transform.source_space != geometry::CoordinateSpace::desktop ||
        transform.destination_space != geometry::CoordinateSpace::surface || transform.destination.x != 0 ||
        transform.destination.y != 0)
        return false;
    for (uint32_t index = 0; index < config.style_count; ++index) {
        if (!style_usable(config.styles[index])) return false;
    }
    for (uint32_t role = 0; role < config.role_styles.size(); ++role) {
        if (config.role_styles[role] >= config.style_count) return false;
    }
    return true;
}

uint32_t placement_bucket(int32_t cell_x, int32_t cell_y) noexcept {
    constexpr uint32_t x_multiplier = UINT32_C(0x8da6b343);
    constexpr uint32_t y_multiplier = UINT32_C(0xd8163841);
    return (static_cast<uint32_t>(cell_x) * x_multiplier ^ static_cast<uint32_t>(cell_y) * y_multiplier) &
           placement_bucket_mask;
}

bool overlaps(const RectQ3& left, const RectQ3& right) noexcept {
    return left.x < right.x + right.width && right.x < left.x + left.width && left.y < right.y + right.height &&
           right.y < left.y + left.height;
}

bool contains(const RectQ3& rect, const geometry::PointQ8& point) noexcept {
    const int32_t x_q3 = point.x / q8_per_q3;
    const int32_t y_q3 = point.y / q8_per_q3;
    return x_q3 >= rect.x && x_q3 < rect.x + rect.width && y_q3 >= rect.y && y_q3 < rect.y + rect.height;
}

RectQ3 label_rect(const SaccadeOverlayTarget& target, const SaccadeOverlayStyle& style) noexcept {
    return {target.label_x_q3, target.label_y_q3,
            static_cast<int32_t>(style.label_padding_x_q3) * 2 +
                static_cast<int32_t>(style.glyph_advance_q3) * target.glyph_count,
            style.label_height_q3};
}

bool collides(const RectQ3& candidate, int32_t cell_width, int32_t cell_height, const SaccadeOverlayTarget* targets,
              const SaccadeOverlayStyle* styles, const OverlayComposeWorkspace& workspace,
              OverlayComposeStats* stats) noexcept {
    const int32_t center_x = candidate.x + candidate.width / 2;
    const int32_t center_y = candidate.y + candidate.height / 2;
    const int32_t cell_x = center_x / cell_width;
    const int32_t cell_y = center_y / cell_height;
    for (int32_t y = cell_y - 1; y <= cell_y + 1; ++y) {
        for (int32_t x = cell_x - 1; x <= cell_x + 1; ++x) {
            uint32_t node_index = workspace.placement_heads[placement_bucket(x, y)];
            while (node_index != empty_index) {
                const OverlayPlacementNode& node = workspace.placement_nodes[node_index];
                if (node.cell_x == x && node.cell_y == y) {
                    ++stats->collision_tests;
                    const SaccadeOverlayTarget& target = targets[node_index];
                    if (overlaps(candidate, label_rect(target, styles[target.style_index]))) return true;
                }
                node_index = node.next;
            }
        }
    }
    return false;
}

void insert_label(uint32_t target_index, const RectQ3& label, int32_t cell_width, int32_t cell_height,
                  OverlayComposeWorkspace* workspace) noexcept {
    OverlayPlacementNode& node = workspace->placement_nodes[target_index];
    node.cell_x = (label.x + label.width / 2) / cell_width;
    node.cell_y = (label.y + label.height / 2) / cell_height;
    const uint32_t bucket = placement_bucket(node.cell_x, node.cell_y);
    node.next = workspace->placement_heads[bucket];
    workspace->placement_heads[bucket] = target_index;
}

int32_t clamp_origin(int32_t value, int32_t extent, int32_t limit) noexcept {
    return std::clamp(value, 0, limit - extent);
}

bool place_label(SaccadeOverlayTarget* target, const SaccadeOverlayStyle& style, const geometry::PointQ8& safe_point,
                 int32_t surface_width, int32_t surface_height, int32_t cell_width, int32_t cell_height,
                 const SaccadeOverlayTarget* targets, const SaccadeOverlayStyle* styles, LabelPlacement placement,
                 const OverlayComposeWorkspace& workspace, OverlayComposeStats* stats) noexcept {
    const int32_t label_width = static_cast<int32_t>(style.label_padding_x_q3) * 2 +
                                static_cast<int32_t>(style.glyph_advance_q3) * target->glyph_count;
    const int32_t label_height = style.label_height_q3;
    if (label_width > surface_width || label_height > surface_height) return false;
    const int32_t left = target->x_q3;
    const int32_t right = left + target->width_q3;
    const int32_t top = target->y_q3;
    const int32_t bottom = top + target->height_q3;
    const int32_t center = left + target->width_q3 / 2;
    const int32_t gap = style.target_stroke_q3;
    const std::array<std::array<int32_t, 2>, placement_candidate_count> candidates{
        {{{left, top - gap - label_height}},
         {{center - label_width / 2, top - gap - label_height}},
         {{right - label_width, top - gap - label_height}},
         {{left, bottom + gap}},
         {{center - label_width / 2, bottom + gap}},
         {{right - label_width, bottom + gap}},
         {{left - gap - label_width, top}},
         {{right + gap, top}}}};
    constexpr std::array<std::array<uint8_t, placement_candidate_count>, 5> orders{{{{0, 1, 2, 3, 4, 5, 6, 7}},
                                                                                    {{1, 0, 2, 4, 3, 5, 6, 7}},
                                                                                    {{4, 3, 5, 1, 0, 2, 6, 7}},
                                                                                    {{6, 0, 3, 1, 4, 2, 5, 7}},
                                                                                    {{7, 2, 5, 1, 4, 0, 3, 6}}}};
    const auto& order = orders[static_cast<uint8_t>(placement)];
    for (uint32_t attempt = 0; attempt < candidates.size(); ++attempt) {
        const uint32_t index = order[attempt];
        RectQ3 candidate{clamp_origin(candidates[index][0], label_width, surface_width),
                         clamp_origin(candidates[index][1], label_height, surface_height), label_width, label_height};
        ++stats->labels_tested;
        if (contains(candidate, safe_point) ||
            collides(candidate, cell_width, cell_height, targets, styles, workspace, stats))
            continue;
        target->label_x_q3 = static_cast<uint16_t>(candidate.x);
        target->label_y_q3 = static_cast<uint16_t>(candidate.y);
        stats->labels_repositioned += attempt != 0 ? 1U : 0U;
        return true;
    }
    return false;
}

int32_t floor_q3(int32_t q8) noexcept {
    return q8 / q8_per_q3;
}

int32_t ceil_q3(int64_t q8) noexcept {
    return static_cast<int32_t>((q8 + q8_per_q3 - 1) / q8_per_q3);
}

bool glyph_index(const OverlayComposeConfig& config, uint16_t symbol, uint8_t* output) noexcept {
    const uint16_t canonical = symbol >= 'a' && symbol <= 'z' ? static_cast<uint16_t>(symbol - ('a' - 'A')) : symbol;
    for (uint32_t index = 0; index < config.glyph_symbol_count; ++index) {
        const uint16_t candidate = config.glyph_symbols[index] >= 'a' && config.glyph_symbols[index] <= 'z'
                                       ? static_cast<uint16_t>(config.glyph_symbols[index] - ('a' - 'A'))
                                       : config.glyph_symbols[index];
        if (candidate == canonical) {
            *output = static_cast<uint8_t>(index);
            return true;
        }
    }
    return false;
}

} // namespace

SaccadeResult OverlayComposer::compose(const scene::PacketView& scene, const interaction::HintLabel* labels,
                                       uint32_t label_count, const OverlayComposeConfig& config,
                                       OverlayComposeWorkspace* workspace, SaccadeMutableSpanU8 output,
                                       OverlayComposeResult* result) noexcept {
    if (workspace == nullptr || result == nullptr || output.data == nullptr || labels == nullptr ||
        !config_valid(scene, config) || label_count != scene.header->target_count)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *result = {};
    result->active_target_index = SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
    if (output.size < sizeof(SaccadeOverlayPacketHeader)) return SACCADE_ERROR_CAPACITY;

    workspace->placement_heads.fill(empty_index);
    workspace->labels_by_scene_index.fill(nullptr);
    workspace->overlay_index_by_scene_index.fill(empty_index);
    for (uint32_t index = 0; index < label_count; ++index) {
        if (labels[index].target_index >= label_count ||
            workspace->labels_by_scene_index[labels[index].target_index] != nullptr) {
            ++stats_.compose_failures;
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        workspace->labels_by_scene_index[labels[index].target_index] = &labels[index];
    }

    const geometry::TransformDesc& transform = config.desktop_to_surface->descriptor();
    const int32_t surface_width = ceil_q3(transform.destination.width);
    const int32_t surface_height = ceil_q3(transform.destination.height);
    if (surface_width > UINT16_MAX || surface_height > UINT16_MAX) {
        ++stats_.compose_failures;
        return SACCADE_ERROR_UNSUPPORTED;
    }
    int32_t cell_width = 1;
    int32_t cell_height = 1;
    for (uint32_t index = 0; index < config.style_count; ++index) {
        const SaccadeOverlayStyle& style = config.styles[index];
        cell_width = std::max(cell_width, static_cast<int32_t>(style.label_padding_x_q3) * 2 +
                                              static_cast<int32_t>(style.glyph_advance_q3) *
                                                  static_cast<int32_t>(SACCADE_OVERLAY_GLYPHS_PER_TARGET));
        cell_height = std::max(cell_height, static_cast<int32_t>(style.label_height_q3));
    }

    auto* targets = reinterpret_cast<SaccadeOverlayTarget*>(output.data + sizeof(SaccadeOverlayPacketHeader));
    uint32_t written = 0;
    for (uint32_t scene_index = 0; scene_index < scene.header->target_count; ++scene_index) {
        ++stats_.targets_read;
        const SaccadeTargetRecord& source = scene.targets[scene_index];
        if (source.display_id != 0 && source.display_id != config.display_id) {
            ++stats_.targets_filtered;
            continue;
        }
        geometry::RectQ8 mapped{};
        const SaccadeResult mapped_result = config.desktop_to_surface->map_rect_clipped(
            {source.x_q8, source.y_q8, source.width_q8, source.height_q8}, &mapped);
        if (mapped_result == SACCADE_ERROR_NOT_FOUND) {
            ++stats_.targets_filtered;
            continue;
        }
        if (mapped_result != SACCADE_OK) {
            ++stats_.compose_failures;
            return mapped_result;
        }
        const int32_t x = floor_q3(mapped.x);
        const int32_t y = floor_q3(mapped.y);
        const int32_t right = ceil_q3(static_cast<int64_t>(mapped.x) + mapped.width);
        const int32_t bottom = ceil_q3(static_cast<int64_t>(mapped.y) + mapped.height);
        if (right - x < 2 || bottom - y < 2) {
            ++stats_.targets_clipped;
            continue;
        }
        const interaction::HintLabel* label = workspace->labels_by_scene_index[scene_index];
        if (label == nullptr || label->target_id != source.target_id || label->symbol_count == 0 ||
            label->symbol_count > SACCADE_OVERLAY_GLYPHS_PER_TARGET) {
            ++stats_.compose_failures;
            return SACCADE_ERROR_CAPACITY;
        }
        const uint32_t role = std::min<uint32_t>(source.role, overlay_target_role_count - 1U);
        SaccadeOverlayTarget target{};
        target.target_id = source.target_id;
        target.x_q3 = static_cast<uint16_t>(x);
        target.y_q3 = static_cast<uint16_t>(y);
        target.width_q3 = static_cast<uint16_t>(right - x);
        target.height_q3 = static_cast<uint16_t>(bottom - y);
        target.confidence_q16 = static_cast<uint16_t>(std::min<uint32_t>(source.confidence_q16, UINT16_MAX));
        target.style_index = config.role_styles[role];
        target.glyph_count = static_cast<uint8_t>(label->symbol_count);
        for (uint32_t glyph = 0; glyph < label->symbol_count; ++glyph) {
            if (!glyph_index(config, label->symbols[glyph], &target.glyphs[glyph])) {
                ++stats_.compose_failures;
                return SACCADE_ERROR_UNSUPPORTED;
            }
        }
        geometry::PointQ8 safe{};
        if (config.desktop_to_surface->map_point({source.safe_x_q8, source.safe_y_q8}, &safe) != SACCADE_OK) {
            safe = {mapped.x + mapped.width / 2, mapped.y + mapped.height / 2};
        }
        if (!place_label(&target, config.styles[target.style_index], safe, surface_width, surface_height, cell_width,
                         cell_height, targets, config.styles, config.placement, *workspace, &stats_)) {
            ++stats_.compose_failures;
            return SACCADE_ERROR_BUSY;
        }
        const size_t required = sizeof(SaccadeOverlayPacketHeader) +
                                static_cast<size_t>(written + 1U) * sizeof(SaccadeOverlayTarget) +
                                static_cast<size_t>(config.style_count) * sizeof(SaccadeOverlayStyle);
        if (output.size < required) return SACCADE_ERROR_CAPACITY;
        targets[written] = target;
        insert_label(written, label_rect(target, config.styles[target.style_index]), cell_width, cell_height,
                     workspace);
        workspace->overlay_index_by_scene_index[scene_index] = written;
        workspace->scene_index_by_overlay_index[written] = scene_index;
        if (source.target_id == config.active_target_id) result->active_target_index = written;
        ++written;
    }

    const size_t styles_offset =
        sizeof(SaccadeOverlayPacketHeader) + static_cast<size_t>(written) * sizeof(SaccadeOverlayTarget);
    if (written != 0) {
        std::memcpy(output.data + styles_offset, config.styles,
                    static_cast<size_t>(config.style_count) * sizeof(SaccadeOverlayStyle));
    }
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = written;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = written == 0 ? 0 : config.style_count;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene.header->scene_epoch;
    header.transform_epoch = config.transform_epoch;
    header.targets_offset = written == 0 ? 0 : sizeof(header);
    header.styles_offset = written == 0 ? 0 : styles_offset;
    const size_t byte_size =
        written == 0 ? sizeof(header)
                     : styles_offset + static_cast<size_t>(config.style_count) * sizeof(SaccadeOverlayStyle);
    std::memcpy(output.data, &header, sizeof(header));
    overlay::PacketView validated{};
    const SaccadeResult validation = overlay::validate_packet({output.data, byte_size}, &validated);
    if (validation != SACCADE_OK) {
        ++stats_.compose_failures;
        return validation;
    }
    result->byte_size = byte_size;
    result->target_count = written;
    result->labels_placed = written;
    ++stats_.packets_composed;
    return SACCADE_OK;
}

} // namespace saccade::application

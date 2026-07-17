#include "application/overlay_composer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

enum class BenchmarkResult : int { success, transform_failed, compose_failed };

constexpr uint32_t target_count = SACCADE_TARGET_PACKET_MAX_TARGETS;
constexpr uint32_t grid_columns = 100;
constexpr uint32_t grid_rows = target_count / grid_columns;
constexpr uint32_t desktop_width = 7680;
constexpr uint32_t desktop_height = 4320;
constexpr uint32_t target_width = 20;
constexpr uint32_t target_height = 20;
constexpr uint32_t coordinate_fraction_bits = 8;
constexpr int32_t coordinate_scale = INT32_C(1) << coordinate_fraction_bits;
constexpr uint64_t display_id = 1;
constexpr uint64_t scene_epoch = 2;
constexpr uint64_t transform_epoch = 3;
constexpr uint32_t warmup_count = 20;
constexpr uint32_t sample_count = 200;
constexpr uint64_t scene_budget_ns = UINT64_C(33333333);
constexpr uint16_t glyph_symbol = static_cast<uint16_t>('A');
constexpr size_t output_capacity = sizeof(SaccadeOverlayPacketHeader) +
                                   static_cast<size_t>(target_count) * sizeof(SaccadeOverlayTarget) +
                                   sizeof(SaccadeOverlayStyle);

int exit_code(BenchmarkResult value) noexcept {
    return static_cast<int>(value);
}

SaccadeOverlayStyle style() noexcept {
    SaccadeOverlayStyle value{};
    value.target_outline_rgba8 = UINT32_C(0xffffffff);
    value.label_background_rgba8 = UINT32_C(0x000000e0);
    value.label_foreground_rgba8 = UINT32_C(0xffffffff);
    value.active_fill_rgba8 = UINT32_C(0x00ff0060);
    value.active_outline_rgba8 = UINT32_C(0xffffffff);
    value.target_stroke_q3 = 8;
    value.target_radius_q3 = 16;
    value.label_height_q3 = 64;
    value.label_radius_q3 = 16;
    value.label_padding_x_q3 = 8;
    value.glyph_width_q3 = 40;
    value.glyph_height_q3 = 56;
    value.glyph_advance_q3 = 48;
    value.active_stroke_q3 = 16;
    return value;
}

} // namespace

int main() {
    static std::array<SaccadeTargetRecord, target_count> targets;
    static std::array<saccade::interaction::HintLabel, target_count> labels;
    static saccade::application::OverlayComposeWorkspace workspace;
    alignas(SaccadeOverlayPacketHeader) static std::array<uint8_t, output_capacity> output;
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = scene_epoch;
    header.transform_epoch = transform_epoch;
    for (uint32_t index = 0; index < target_count; ++index) {
        const uint32_t column = index % grid_columns;
        const uint32_t row = index / grid_columns;
        SaccadeTargetRecord& target = targets[index];
        target.target_id = index + 1U;
        target.display_id = display_id;
        target.x_q8 =
            static_cast<int32_t>((static_cast<uint64_t>(column) * desktop_width / grid_columns) * coordinate_scale);
        target.y_q8 =
            static_cast<int32_t>((static_cast<uint64_t>(row) * desktop_height / grid_rows) * coordinate_scale);
        target.width_q8 = target_width * coordinate_scale;
        target.height_q8 = target_height * coordinate_scale;
        target.safe_x_q8 = target.x_q8 + target.width_q8 / 2;
        target.safe_y_q8 = target.y_q8 + target.height_q8 / 2;
        target.confidence_q16 = UINT16_MAX;
        target.role = SACCADE_TARGET_ROLE_BUTTON;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
        labels[index].target_id = target.target_id;
        labels[index].target_index = index;
        labels[index].symbol_count = 1;
        labels[index].symbols[0] = glyph_symbol;
    }
    const saccade::scene::PacketView scene{&header, targets.data(), 0};
    saccade::geometry::TransformDesc transform_desc{};
    transform_desc.source = {0, 0, static_cast<int32_t>(desktop_width * coordinate_scale),
                             static_cast<int32_t>(desktop_height * coordinate_scale)};
    transform_desc.destination = transform_desc.source;
    transform_desc.epoch = transform_epoch;
    transform_desc.source_space = saccade::geometry::CoordinateSpace::desktop;
    transform_desc.destination_space = saccade::geometry::CoordinateSpace::surface;
    saccade::geometry::CoordinateTransform transform;
    if (transform.initialize(transform_desc) != SACCADE_OK) return exit_code(BenchmarkResult::transform_failed);
    const SaccadeOverlayStyle packet_style = style();
    saccade::application::OverlayComposeConfig config{};
    config.display_id = display_id;
    config.transform_epoch = transform_epoch;
    config.desktop_to_surface = &transform;
    config.styles = &packet_style;
    config.style_count = 1;
    config.glyph_symbols = &glyph_symbol;
    config.glyph_symbol_count = 1;
    saccade::application::OverlayComposer composer;
    saccade::application::OverlayComposeResult composed{};
    std::array<uint64_t, sample_count> samples{};
    for (uint32_t iteration = 0; iteration < warmup_count + sample_count; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const SaccadeResult result = composer.compose(scene, labels.data(), target_count, config, &workspace,
                                                      {output.data(), output.size()}, &composed);
        const auto finish = std::chrono::steady_clock::now();
        if (result != SACCADE_OK || composed.target_count != target_count)
            return exit_code(BenchmarkResult::compose_failed);
        if (iteration >= warmup_count) {
            samples[iteration - warmup_count] =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        }
    }
    std::sort(samples.begin(), samples.end());
    const uint64_t median = samples[sample_count / 2U];
    const uint64_t p95 = samples[sample_count * 95U / 100U];
    std::printf("overlay_compose targets=%u median_ns=%llu p95_ns=%llu "
                "per_target_ns=%.1f scene_budget_pct=%.3f\n",
                target_count, static_cast<unsigned long long>(median), static_cast<unsigned long long>(p95),
                static_cast<double>(p95) / target_count, static_cast<double>(p95) / scene_budget_ns * 100.0);
    return exit_code(BenchmarkResult::success);
}

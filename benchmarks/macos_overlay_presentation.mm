#include "core/stack_string_builder.hpp"
#include "platform/macos/display_topology.hpp"
#include "platform/macos/overlay_surface.hpp"

#import <AppKit/AppKit.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <time.h>
#include <unistd.h>

namespace {

using saccade::core::StackStringBuilder;
using saccade::platform::macos::OverlaySurface;
using saccade::platform::macos::OverlaySurfaceCallbacks;
using saccade::platform::macos::OverlaySurfaceInfo;
using saccade::platform::macos::OverlaySurfaceMemoryStats;
using saccade::platform::macos::OverlaySurfaceStats;

constexpr uint32_t target_count = SACCADE_OVERLAY_MAX_TARGETS;
constexpr uint32_t warmup_seconds = 1;
constexpr uint32_t default_duration_seconds = 10;
constexpr uint32_t maximum_duration_seconds = 60;
constexpr uint64_t parts_per_million = UINT64_C(1000000);
constexpr uint64_t minimum_refresh_ratio_per_thousand = 990;
constexpr uint64_t maximum_deadline_miss_ppm = 10000;
constexpr uint64_t maximum_busy_ppm = 1000;
constexpr uint64_t callback_budget_ns = UINT64_C(8333333);
constexpr size_t maximum_packet_bytes = sizeof(SaccadeOverlayPacketHeader) +
                                        static_cast<size_t>(target_count) * sizeof(SaccadeOverlayTarget) +
                                        sizeof(SaccadeOverlayStyle);

enum class ExitCode : int { success, usage, setup, qualification, output };

struct PacketStorage {
    alignas(64) std::array<uint8_t, maximum_packet_bytes> bytes{};
    SaccadeOverlayFrameDesc frame{};
    uint64_t first_load_ns = 0;
    uint64_t last_load_ns = 0;
    uint64_t measured_load_count = 0;
    bool measuring = false;
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

template <class Record> void store_record(PacketStorage* storage, size_t offset, const Record& record) noexcept {
    std::memcpy(storage->bytes.data() + offset, &record, sizeof(record));
}

void build_packet(PacketStorage* storage) noexcept {
    const size_t targets_offset = sizeof(SaccadeOverlayPacketHeader);
    const size_t styles_offset = targets_offset + static_cast<size_t>(target_count) * sizeof(SaccadeOverlayTarget);
    const size_t packet_size = styles_offset + sizeof(SaccadeOverlayStyle);

    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = 1;
    header.transform_epoch = 1;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store_record(storage, 0, header);

    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeOverlayTarget target{};
        target.target_id = static_cast<uint64_t>(index) + 1U;
        target.x_q3 = static_cast<uint16_t>((index % 100U) * 64U);
        target.y_q3 = static_cast<uint16_t>(((index / 100U) % 100U) * 48U);
        target.width_q3 = 48;
        target.height_q3 = 32;
        target.label_x_q3 = target.x_q3;
        target.label_y_q3 = target.y_q3;
        target.confidence_q16 = UINT16_MAX;
        target.glyphs[0] = static_cast<uint8_t>(index % 32U);
        target.glyphs[1] = static_cast<uint8_t>((index + 1U) % 32U);
        target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyph_count = 2;
        store_record(storage, targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }

    SaccadeOverlayStyle style{};
    style.target_stroke_q3 = 8;
    style.label_height_q3 = 24;
    style.label_padding_x_q3 = 4;
    style.glyph_width_q3 = 8;
    style.glyph_height_q3 = 16;
    style.glyph_advance_q3 = 8;
    style.active_stroke_q3 = 8;
    store_record(storage, styles_offset, style);

    storage->frame.struct_size = sizeof(storage->frame);
    storage->frame.api_version = SACCADE_API_VERSION;
    storage->frame.scene_epoch = header.scene_epoch;
    storage->frame.transform_epoch = header.transform_epoch;
    storage->frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
    storage->frame.active_target_index = 0;
    storage->frame.packet = {storage->bytes.data(), packet_size};
}

SaccadeResult load_frame(void* context, uint64_t display_id, SaccadeOverlayFrameDesc* output) noexcept {
    if (context == nullptr || display_id == 0 || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* storage = static_cast<PacketStorage*>(context);
    if (storage->measuring) {
        const uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (storage->measured_load_count == 0) storage->first_load_ns = now_ns;
        storage->last_load_ns = now_ns;
        ++storage->measured_load_count;
    }
    *output = storage->frame;
    return SACCADE_OK;
}

bool parse_duration(const char* text, uint32_t* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const char* end = text;
    while (*end != '\0')
        ++end;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > maximum_duration_seconds) return false;
    *output = value;
    return true;
}

const saccade::geometry::DisplaySurface* main_display(const saccade::geometry::DisplaySnapshot& snapshot) noexcept {
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        if ((snapshot.displays[index].flags & saccade::geometry::display_surface_main) != 0) {
            return &snapshot.displays[index];
        }
    }
    return nullptr;
}

const char* path_name(saccade::backend::metal::Path path) noexcept {
    switch (path) {
    case saccade::backend::metal::Path::metal3:
        return "metal3";
    case saccade::backend::metal::Path::metal4:
        return "metal4";
    case saccade::backend::metal::Path::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

void emit(std::string_view value) noexcept {
    (void)write(STDOUT_FILENO, value.data(), value.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return exit_code(ExitCode::usage);
    uint32_t duration_seconds = default_duration_seconds;
    if (argc == 3 && !parse_duration(argv[2], &duration_seconds)) return exit_code(ExitCode::usage);

    @autoreleasepool {
        [NSApplication sharedApplication];
        saccade::geometry::DisplayCatalog catalog;
        saccade::platform::macos::DisplayCollector collector;
        if (collector.refresh(&catalog) != SACCADE_OK) return exit_code(ExitCode::setup);
        const saccade::geometry::DisplaySurface* display = main_display(catalog.snapshot());
        if (display == nullptr || display->maximum_fps < 120) return exit_code(ExitCode::setup);

        static PacketStorage packet;
        build_packet(&packet);
        OverlaySurface surface;
        const OverlaySurfaceCallbacks callbacks{&packet, load_frame, nullptr};
        if (surface.initialize(*display, argv[1], saccade::backend::metal::PathPreference::automatic, callbacks) !=
                SACCADE_OK ||
            surface.start() != SACCADE_OK || surface.request_present(UINT32_MAX, true) != SACCADE_OK) {
            return exit_code(ExitCode::setup);
        }

        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:warmup_seconds]];
        OverlaySurfaceStats baseline_stats{};
        saccade::backend::metal::Stats baseline_renderer{};
        if (surface.read_stats(&baseline_stats) != SACCADE_OK ||
            surface.read_renderer_stats(&baseline_renderer) != SACCADE_OK || baseline_stats.display_ticks == 0) {
            return exit_code(ExitCode::setup);
        }

        packet.first_load_ns = 0;
        packet.last_load_ns = 0;
        packet.measured_load_count = 0;
        packet.measuring = true;
        const uint64_t started_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:duration_seconds]];
        const uint64_t stopped_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (surface.stop() != SACCADE_OK || stopped_ns <= started_ns) return exit_code(ExitCode::setup);
        packet.measuring = false;

        OverlaySurfaceInfo info{};
        OverlaySurfaceStats stats{};
        OverlaySurfaceMemoryStats memory{};
        saccade::backend::metal::Stats renderer{};
        if (surface.read_info(&info) != SACCADE_OK || surface.read_stats(&stats) != SACCADE_OK ||
            surface.read_memory_stats(&memory) != SACCADE_OK || surface.read_renderer_stats(&renderer) != SACCADE_OK ||
            stats.display_ticks <= baseline_stats.display_ticks ||
            stats.rendered_frames < baseline_stats.rendered_frames ||
            renderer.presented_frames < baseline_renderer.presented_frames ||
            stats.deadline_misses < baseline_stats.deadline_misses || stats.busy_frames < baseline_stats.busy_frames ||
            renderer.active_dispatches < baseline_renderer.active_dispatches || packet.measured_load_count < 2 ||
            packet.last_load_ns <= packet.first_load_ns) {
            return exit_code(ExitCode::setup);
        }

        const uint64_t duration_ns = stopped_ns - started_ns;
        const uint64_t display_ticks = stats.display_ticks - baseline_stats.display_ticks;
        const uint64_t rendered_frames = stats.rendered_frames - baseline_stats.rendered_frames;
        const uint64_t presented_frames = renderer.presented_frames - baseline_renderer.presented_frames;
        const uint64_t deadline_misses = stats.deadline_misses - baseline_stats.deadline_misses;
        const uint64_t busy_frames = stats.busy_frames - baseline_stats.busy_frames;
        const uint64_t active_dispatches = renderer.active_dispatches - baseline_renderer.active_dispatches;
        const uint64_t wall_refresh_millihz = display_ticks * UINT64_C(1000000000000) / duration_ns;
        const uint64_t cadence_duration_ns = packet.last_load_ns - packet.first_load_ns;
        const uint64_t refresh_millihz =
            (packet.measured_load_count - 1U) * UINT64_C(1000000000000) / cadence_duration_ns;
        const uint64_t deadline_miss_ppm = deadline_misses * parts_per_million / display_ticks;
        const uint64_t busy_ppm = busy_frames * parts_per_million / display_ticks;
        const uint64_t minimum_refresh_millihz =
            static_cast<uint64_t>(info.preferred_fps) * minimum_refresh_ratio_per_thousand;
        const bool qualified = info.preferred_fps == 120 && refresh_millihz >= minimum_refresh_millihz &&
                               deadline_miss_ppm <= maximum_deadline_miss_ppm && busy_ppm <= maximum_busy_ppm &&
                               stats.failures == 0 && stats.maximum_callback_ns <= callback_budget_ns &&
                               rendered_frames == presented_frames && renderer.target_capacity == target_count &&
                               active_dispatches != 0 && packet.measured_load_count == display_ticks;

        StackStringBuilder<1024> output;
        const bool written =
            output.append("macos_overlay_presentation targets=") && output.append_unsigned(target_count) &&
            output.append(" transparent=1 path=") && output.append(path_name(renderer.path)) &&
            output.append(" duration_ns=") && output.append_unsigned(duration_ns) && output.append(" preferred_fps=") &&
            output.append_unsigned(info.preferred_fps) && output.append(" warmup_ticks=") &&
            output.append_unsigned(baseline_stats.display_ticks) && output.append(" display_ticks=") &&
            output.append_unsigned(display_ticks) && output.append(" rendered_frames=") &&
            output.append_unsigned(rendered_frames) && output.append(" presented_frames=") &&
            output.append_unsigned(presented_frames) && output.append(" wall_refresh_millihz=") &&
            output.append_unsigned(wall_refresh_millihz) && output.append(" cadence_refresh_millihz=") &&
            output.append_unsigned(refresh_millihz) && output.append(" cadence_duration_ns=") &&
            output.append_unsigned(cadence_duration_ns) && output.append(" deadline_misses=") &&
            output.append_unsigned(deadline_misses) && output.append(" deadline_miss_ppm=") &&
            output.append_unsigned(deadline_miss_ppm) && output.append(" busy_frames=") &&
            output.append_unsigned(busy_frames) && output.append(" busy_ppm=") && output.append_unsigned(busy_ppm) &&
            output.append(" failures=") && output.append_unsigned(stats.failures) &&
            output.append(" maximum_callback_ns=") && output.append_unsigned(stats.maximum_callback_ns) &&
            output.append(" static_dispatches=") && output.append_unsigned(renderer.static_dispatches) &&
            output.append(" active_dispatches=") && output.append_unsigned(active_dispatches) &&
            output.append(" drawable_bytes_estimate=") && output.append_unsigned(memory.drawable_bytes_estimate) &&
            output.append(" total_known_and_estimated=") && output.append_unsigned(memory.total_known_and_estimated) &&
            output.append(" qualified=") && output.append_unsigned(qualified ? 1U : 0U) && output.append('\n');
        if (!written || output.truncated()) return exit_code(ExitCode::output);
        emit(output.view());
        return qualified ? exit_code(ExitCode::success) : exit_code(ExitCode::qualification);
    }
}

#include "backends/metal/overlay_expander.hpp"
#include "core/stack_string_builder.hpp"

#include <saccade/saccade_overlay.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    benchmark_failure = 2,
    fallback_failure = 3,
    output_truncated = 4,
    output_failure = 5,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr size_t max_packet_bytes = sizeof(SaccadeOverlayPacketHeader) +
                                    SACCADE_OVERLAY_MAX_TARGETS * sizeof(SaccadeOverlayTarget) +
                                    sizeof(SaccadeOverlayStyle);

alignas(64) std::array<uint8_t, max_packet_bytes> packet_storage{};
uint64_t benchmark_sink = 0;

template <class Record> void store_record(size_t offset, const Record& record) noexcept {
    std::memcpy(packet_storage.data() + offset, &record, sizeof(record));
}

SaccadeSpanU8 build_packet(uint32_t target_count, uint64_t scene_epoch) noexcept {
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
    header.scene_epoch = scene_epoch;
    header.transform_epoch = 1;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store_record(0, header);

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
        store_record(targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }

    SaccadeOverlayStyle style{};
    style.target_stroke_q3 = 8;
    style.label_height_q3 = 24;
    style.label_padding_x_q3 = 4;
    style.glyph_width_q3 = 8;
    style.glyph_height_q3 = 16;
    style.glyph_advance_q3 = 8;
    style.active_stroke_q3 = 8;
    store_record(styles_offset, style);
    return {packet_storage.data(), packet_size};
}

void set_scene_epoch(uint64_t scene_epoch) noexcept {
    SaccadeOverlayPacketHeader header{};
    std::memcpy(&header, packet_storage.data(), sizeof(header));
    header.scene_epoch = scene_epoch;
    store_record(0, header);
}

const char* path_name(saccade::backend::metal::Path path) noexcept {
    switch (path) {
    case saccade::backend::metal::Path::metal3:
        return "metal3";
    case saccade::backend::metal::Path::metal4:
        return "metal4";
    case saccade::backend::metal::Path::unavailable:
        break;
    }
    return "unavailable";
}

template <class Operation> uint64_t measure_ns(uint32_t iterations, Operation&& operation) noexcept {
    uint64_t accumulator = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        accumulator += operation(iteration) ? 1U : 0U;
    }
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink = accumulator;
    if (accumulator != iterations) {
        return 0;
    }
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

bool append_result(saccade::core::StackStringBuilder<2048>* text, const char* path, uint32_t target_count,
                   const char* operation, uint32_t iterations, uint64_t elapsed_ns) noexcept {
    return elapsed_ns != 0 && text->append("path=") && text->append(path) && text->append(" targets=") &&
           text->append_unsigned(target_count) && text->append(" operation=") && text->append(operation) &&
           text->append(" iterations=") && text->append_unsigned(iterations) && text->append(" total_ns=") &&
           text->append_unsigned(elapsed_ns) && text->append(" ns_per_operation=") &&
           text->append_unsigned(elapsed_ns / iterations) && text->append('\n');
}

bool submit_and_wait(saccade::backend::metal::OverlayExpander* expander,
                     const SaccadeOverlayFrameDesc& frame) noexcept {
    saccade::backend::metal::Submission submission{};
    return expander->submit(frame, &submission) == SACCADE_OK &&
           expander->wait(submission, UINT64_C(1000000000)) == SACCADE_OK;
}

template <class Prepare>
uint64_t measure_pipelined_ns(uint32_t iterations, saccade::backend::metal::OverlayExpander* expander,
                              SaccadeOverlayFrameDesc* frame, Prepare&& prepare) noexcept {
    if (iterations == 0 || iterations % 3U != 0) {
        return 0;
    }
    uint32_t completed = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t base = 0; base < iterations; base += 3U) {
        std::array<saccade::backend::metal::Submission, 3> submissions{};
        for (uint32_t lane = 0; lane < submissions.size(); ++lane) {
            prepare(base + lane, frame);
            if (expander->submit(*frame, &submissions[lane]) != SACCADE_OK) {
                return 0;
            }
        }
        if (expander->wait(submissions.back(), UINT64_C(1000000000)) != SACCADE_OK) {
            return 0;
        }
        completed += static_cast<uint32_t>(submissions.size());
    }
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink = completed;
    return completed == iterations
               ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count())
               : 0;
}

bool run_case(saccade::backend::metal::OverlayExpander* expander, uint32_t target_count, uint32_t scene_iterations,
              uint32_t active_iterations, saccade::core::StackStringBuilder<2048>* text) noexcept {
    SaccadeSpanU8 packet = build_packet(target_count, 1);
    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.transform_epoch = 1;
    frame.packet = packet;

    const uint64_t scene_ns = measure_ns(scene_iterations, [&](uint32_t iteration) noexcept {
        const uint64_t epoch = static_cast<uint64_t>(iteration) + 1U;
        set_scene_epoch(epoch);
        frame.scene_epoch = epoch;
        return submit_and_wait(expander, frame);
    });
    const uint32_t pipelined_scene_iterations = target_count < 10000 ? 600 : 120;
    const uint64_t pipelined_scene_ns = measure_pipelined_ns(
        pipelined_scene_iterations, expander, &frame, [&](uint32_t iteration, SaccadeOverlayFrameDesc* next) noexcept {
            const uint64_t epoch = UINT64_C(100000) + iteration;
            set_scene_epoch(epoch);
            next->scene_epoch = epoch;
        });

    packet = build_packet(target_count, UINT64_C(1000000));
    frame.packet = packet;
    frame.scene_epoch = UINT64_C(1000000);
    for (uint32_t slot = 0; slot < 3; ++slot) {
        if (!submit_and_wait(expander, frame)) {
            return false;
        }
    }
    const uint64_t static_before = expander->stats().static_dispatches;
    const uint64_t active_ns = measure_ns(active_iterations, [&](uint32_t iteration) noexcept {
        frame.flags = (iteration & 1U) != 0 ? SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET : 0;
        frame.active_target_index = target_count == 0 ? 0 : iteration % target_count;
        return submit_and_wait(expander, frame);
    });
    const uint64_t static_after = expander->stats().static_dispatches;
    const uint64_t pipelined_active_ns =
        measure_pipelined_ns(2100, expander, &frame, [&](uint32_t iteration, SaccadeOverlayFrameDesc* next) noexcept {
            next->flags = (iteration & 1U) != 0 ? SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET : 0;
            next->active_target_index = target_count == 0 ? 0 : iteration % target_count;
        });
    const uint64_t pipelined_static_after = expander->stats().static_dispatches;
    return static_before == static_after && static_after == pipelined_static_after &&
           append_result(text, path_name(expander->stats().path), target_count, "scene_expand", scene_iterations,
                         scene_ns) &&
           append_result(text, path_name(expander->stats().path), target_count, "scene_expand_pipelined",
                         pipelined_scene_iterations, pipelined_scene_ns) &&
           append_result(text, path_name(expander->stats().path), target_count, "active_update", active_iterations,
                         active_ns) &&
           append_result(text, path_name(expander->stats().path), target_count, "active_update_pipelined", 2100,
                         pipelined_active_ns);
}

bool run_path(const char* metallib_path, saccade::backend::metal::PathPreference preference,
              saccade::core::StackStringBuilder<2048>* text, saccade::backend::metal::Path* out_path) noexcept {
    saccade::backend::metal::OverlayExpander expander;
    if (expander.initialize(metallib_path, preference) != SACCADE_OK || !run_case(&expander, 100, 500, 2000, text) ||
        !run_case(&expander, 10000, 100, 2000, text)) {
        return false;
    }
    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    const saccade::backend::metal::Stats stats = expander.stats();
    *out_path = stats.path;
    return expander.memory_stats(&memory) == SACCADE_OK && text->append("path=") &&
           text->append(path_name(stats.path)) && text->append(" device_bytes=") &&
           text->append_unsigned(memory.device_owned) && text->append(" framework_bytes=") &&
           text->append_unsigned(memory.framework_opaque) && text->append(" allocator_bytes=") &&
           text->append_unsigned(stats.command_allocator_bytes) && text->append(" packet_upload_bytes=") &&
           text->append_unsigned(stats.packet_upload_bytes) && text->append('\n');
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return to_process_exit_code(ExitCode::invalid_arguments);
    }
    saccade::core::StackStringBuilder<2048> text;
    saccade::backend::metal::Path selected_path = saccade::backend::metal::Path::unavailable;
    if (!run_path(argv[1], saccade::backend::metal::PathPreference::automatic, &text, &selected_path)) {
        return to_process_exit_code(ExitCode::benchmark_failure);
    }
    if (selected_path == saccade::backend::metal::Path::metal4 &&
        !run_path(argv[1], saccade::backend::metal::PathPreference::metal3, &text, &selected_path)) {
        return to_process_exit_code(ExitCode::fallback_failure);
    }
    if (text.truncated()) {
        return to_process_exit_code(ExitCode::output_truncated);
    }
    return to_process_exit_code(std::fwrite(text.view().data(), 1, text.view().size(), stdout) == text.view().size()
                                    ? ExitCode::success
                                    : ExitCode::output_failure);
}

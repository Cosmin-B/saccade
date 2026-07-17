#include "core/stack_string_builder.hpp"
#include "overlay/packet.hpp"

#include <saccade/saccade_overlay.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class ExitCode : int { success = 0, benchmark_failed = 1, output_failed = 2 };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

constexpr size_t max_targets = SACCADE_OVERLAY_MAX_TARGETS;
constexpr size_t max_instances = max_targets * 5U;
constexpr size_t max_packet_bytes =
    sizeof(SaccadeOverlayPacketHeader) + max_targets * sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);

alignas(64) std::array<uint8_t, max_packet_bytes> packet_storage{};
alignas(64) std::array<SaccadeOverlayRect, max_instances> rect_storage{};
alignas(64) std::array<SaccadeOverlayInstanceMeta, max_instances> metadata_storage{};
uint64_t benchmark_sink = 0;

template <class Record> void store_record(uint8_t* bytes, size_t offset, const Record& record) noexcept {
    std::memcpy(bytes + offset, &record, sizeof(record));
}

SaccadeSpanU8 build_packet(uint32_t target_count) noexcept {
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
    store_record(packet_storage.data(), 0, header);

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
        store_record(packet_storage.data(), targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }

    SaccadeOverlayStyle style{};
    style.target_stroke_q3 = 8;
    style.label_height_q3 = 24;
    style.label_padding_x_q3 = 4;
    style.glyph_width_q3 = 8;
    style.glyph_height_q3 = 16;
    style.glyph_advance_q3 = 8;
    style.active_stroke_q3 = 8;
    store_record(packet_storage.data(), styles_offset, style);
    return {packet_storage.data(), packet_size};
}

template <class Operation> uint64_t measure_ns(uint32_t iterations, Operation&& operation) noexcept {
    uint64_t accumulator = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        accumulator += operation();
    }
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink = accumulator;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

bool append_result(saccade::core::StackStringBuilder<512>* text, uint32_t target_count, const char* operation,
                   uint32_t iterations, uint64_t elapsed_ns) noexcept {
    return text->append("targets=") && text->append_unsigned(target_count) && text->append(" operation=") &&
           text->append(operation) && text->append(" iterations=") && text->append_unsigned(iterations) &&
           text->append(" total_ns=") && text->append_unsigned(elapsed_ns) && text->append(" ns_per_operation=") &&
           text->append_unsigned(elapsed_ns / iterations) && text->append('\n');
}

bool run_case(uint32_t target_count, uint32_t iterations, saccade::core::StackStringBuilder<512>* text) noexcept {
    const SaccadeSpanU8 packet = build_packet(target_count);
    saccade::overlay::PacketView view{};
    if (saccade::overlay::validate_packet(packet, &view) != SACCADE_OK) {
        return false;
    }

    size_t output_count = 0;
    const saccade::overlay::ExpandedInstanceSpan output{rect_storage.data(), metadata_storage.data(),
                                                        metadata_storage.size()};
    if (saccade::overlay::expand_static(view, output, &output_count) != SACCADE_OK ||
        output_count != static_cast<size_t>(target_count) * 5U) {
        return false;
    }

    const uint64_t validate_ns = measure_ns(iterations, [&]() noexcept {
        saccade::overlay::PacketView measured{};
        const SaccadeResult result = saccade::overlay::validate_packet(packet, &measured);
        return result == SACCADE_OK ? measured.header.target_count : 0U;
    });
    const uint64_t expand_ns = measure_ns(iterations, [&]() noexcept {
        size_t measured_count = 0;
        const SaccadeResult result = saccade::overlay::expand_static(view, output, &measured_count);
        return result == SACCADE_OK ? measured_count : 0U;
    });

    return append_result(text, target_count, "validate", iterations, validate_ns) &&
           append_result(text, target_count, "expand", iterations, expand_ns);
}

} // namespace

int main() {
    saccade::core::StackStringBuilder<512> text;
    if (!run_case(100, 20000, &text) || !run_case(10000, 250, &text) || text.truncated()) {
        return exit_code(ExitCode::benchmark_failed);
    }
    return exit_code(std::fwrite(text.view().data(), 1, text.view().size(), stdout) == text.view().size()
                         ? ExitCode::success
                         : ExitCode::output_failed);
}

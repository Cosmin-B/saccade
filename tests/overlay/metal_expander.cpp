#include "backends/metal/overlay_expander.hpp"
#include "overlay/packet.hpp"

#include <saccade/saccade_overlay.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    automatic_path = 2,
    metal3_path = 3,
    metal4_path = 4,
    contract = 5,
    initialize = 6,
    unsupported = 77,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr size_t max_packet_size = sizeof(SaccadeOverlayPacketHeader) +
                                   SACCADE_OVERLAY_MAX_TARGETS * sizeof(SaccadeOverlayTarget) +
                                   SACCADE_OVERLAY_MAX_STYLES * sizeof(SaccadeOverlayStyle);
constexpr size_t max_instance_count = SACCADE_OVERLAY_MAX_TARGETS * 5U + 1U;

alignas(64) std::array<uint8_t, max_packet_size> packet_bytes{};
alignas(64) std::array<SaccadeOverlayRect, max_instance_count> expected_rects{};
alignas(64) std::array<SaccadeOverlayInstanceMeta, max_instance_count> expected_metadata{};
alignas(64) std::array<SaccadeOverlayRect, max_instance_count> actual_rects{};
alignas(64) std::array<SaccadeOverlayInstanceMeta, max_instance_count> actual_metadata{};

template <class Record> void store(size_t offset, const Record& record) noexcept {
    std::memcpy(packet_bytes.data() + offset, &record, sizeof(record));
}

SaccadeSpanU8 build_packet(uint32_t target_count, uint64_t scene_epoch, uint64_t transform_epoch = 7) noexcept {
    const uint32_t style_count = target_count == 0 ? 0 : 4;
    const size_t targets_offset = target_count == 0 ? 0 : sizeof(SaccadeOverlayPacketHeader);
    const size_t styles_offset =
        target_count == 0 ? 0 : targets_offset + static_cast<size_t>(target_count) * sizeof(SaccadeOverlayTarget);
    const size_t packet_size = target_count == 0 ? sizeof(SaccadeOverlayPacketHeader)
                                                 : styles_offset + style_count * sizeof(SaccadeOverlayStyle);

    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = style_count;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene_epoch;
    header.transform_epoch = transform_epoch;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store(0, header);

    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeOverlayTarget target{};
        target.target_id = static_cast<uint64_t>(index) + 1U;
        target.x_q3 = static_cast<uint16_t>((index % 100U) * 64U + (transform_epoch == 7 ? 0U : 8U));
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
        target.style_index = static_cast<uint8_t>(index % style_count);
        target.glyph_count = 2;
        store(targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }

    for (uint32_t index = 0; index < style_count; ++index) {
        SaccadeOverlayStyle style{};
        style.target_stroke_q3 = static_cast<uint16_t>(4U + index);
        style.label_height_q3 = static_cast<uint16_t>(24U + index);
        style.label_padding_x_q3 = static_cast<uint16_t>(4U + index);
        style.glyph_width_q3 = 8;
        style.glyph_height_q3 = 16;
        style.glyph_advance_q3 = static_cast<uint16_t>(8U + index);
        style.active_stroke_q3 = 8;
        store(styles_offset + static_cast<size_t>(index) * sizeof(style), style);
    }
    return {packet_bytes.data(), packet_size};
}

bool compare_case(saccade::backend::metal::OverlayExpander* expander, uint32_t target_count, uint64_t scene_epoch,
                  bool has_active, uint64_t transform_epoch = 7) noexcept {
    const SaccadeSpanU8 packet = build_packet(target_count, scene_epoch, transform_epoch);
    saccade::overlay::PacketView view{};
    if (saccade::overlay::validate_packet(packet, &view) != SACCADE_OK) {
        return false;
    }

    size_t static_count = 0;
    const saccade::overlay::ExpandedInstanceSpan expected{expected_rects.data(), expected_metadata.data(),
                                                          expected_rects.size()};
    if (saccade::overlay::expand_static(view, expected, &static_count) != SACCADE_OK) {
        return false;
    }
    size_t active_count = 0;
    const uint32_t active_index = has_active ? target_count - 1U : SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
    if (saccade::overlay::expand_active(view, active_index,
                                        {expected_rects.data() + static_count, expected_metadata.data() + static_count,
                                         expected_rects.size() - static_count},
                                        &active_count) != SACCADE_OK) {
        return false;
    }

    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = scene_epoch;
    frame.transform_epoch = transform_epoch;
    frame.packet = packet;
    if (has_active) {
        frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
        frame.active_target_index = active_index;
    }

    saccade::backend::metal::Submission submission{};
    if (expander->submit(frame, &submission) != SACCADE_OK ||
        expander->wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
        return false;
    }

    size_t actual_count = 0;
    if (expander->copy_instances(submission, {actual_rects.data(), actual_metadata.data(), actual_rects.size()},
                                 &actual_count) != SACCADE_OK ||
        actual_count != static_count + active_count) {
        return false;
    }
    return std::memcmp(actual_rects.data(), expected_rects.data(), actual_count * sizeof(SaccadeOverlayRect)) == 0 &&
           std::memcmp(actual_metadata.data(), expected_metadata.data(),
                       actual_count * sizeof(SaccadeOverlayInstanceMeta)) == 0;
}

bool exercise_path(const char* metallib_path, saccade::backend::metal::PathPreference preference) noexcept {
    saccade::backend::metal::OverlayExpander expander;
    const SaccadeResult initialized = expander.initialize(metallib_path, preference);
    if (initialized == SACCADE_ERROR_UNSUPPORTED && preference == saccade::backend::metal::PathPreference::metal4) {
        return true;
    }
    if (initialized != SACCADE_OK || !compare_case(&expander, 1, 1, false) || !compare_case(&expander, 100, 2, true) ||
        !compare_case(&expander, 10000, 3, true)) {
        return false;
    }

    for (uint32_t repeat = 0; repeat < 4; ++repeat) {
        if (!compare_case(&expander, 100, 4, (repeat & 1U) != 0)) {
            return false;
        }
    }
    const saccade::backend::metal::Stats stats = expander.stats();
    if (stats.path == saccade::backend::metal::Path::unavailable || stats.slot_count != 3 ||
        stats.target_capacity != SACCADE_OVERLAY_MAX_TARGETS || stats.instance_capacity != max_instance_count ||
        stats.submissions != 7 || stats.static_dispatches != 6 || stats.active_dispatches != 7 ||
        stats.packet_upload_bytes == 0) {
        return false;
    }

    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    return expander.memory_stats(&memory) == SACCADE_OK && memory.device_owned != 0 &&
           memory.high_water_bytes >= memory.device_owned;
}

bool exercise_contract(const char* metallib_path) noexcept {
    using saccade::backend::metal::OverlayExpander;
    using saccade::backend::metal::PathPreference;
    using saccade::backend::metal::Submission;

    OverlayExpander rejected;
    if (rejected.initialize(nullptr, PathPreference::automatic) != SACCADE_ERROR_INVALID_ARGUMENT ||
        rejected.initialize(metallib_path, static_cast<PathPreference>(99)) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return false;
    }

    OverlayExpander expander;
    if (expander.initialize(metallib_path, PathPreference::automatic) != SACCADE_OK ||
        expander.initialize(metallib_path, PathPreference::automatic) != SACCADE_ERROR_STATE) {
        return false;
    }

    const SaccadeSpanU8 packet = build_packet(100, 0);
    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = 0;
    frame.transform_epoch = 7;
    frame.packet = packet;

    Submission ignored{};
    SaccadeOverlayFrameDesc invalid = frame;
    invalid.transform_epoch = 8;
    if (expander.submit(invalid, &ignored) != SACCADE_ERROR_INVALID_ARGUMENT ||
        expander.submit(frame, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return false;
    }

    Submission first{};
    if (expander.submit(frame, &first) != SACCADE_OK) {
        return false;
    }
    bool complete = false;
    if (expander.poll(first, &complete) != SACCADE_OK) {
        return false;
    }
    if (!complete) {
        const SaccadeResult immediate = expander.wait(first, 0);
        if (immediate != SACCADE_ERROR_TIMEOUT) {
            return false;
        }
    }
    if (expander.wait(first, UINT64_C(1000000000)) != SACCADE_OK) {
        return false;
    }

    size_t required = 0;
    if (expander.copy_instances(first, {}, &required) != SACCADE_ERROR_CAPACITY || required != 500) {
        return false;
    }

    if (!compare_case(&expander, 100, 0, false, 8)) {
        return false;
    }

    const SaccadeSpanU8 stable_packet = build_packet(1, 50);
    frame.scene_epoch = 50;
    frame.transform_epoch = 7;
    frame.packet = stable_packet;
    Submission stable_first{};
    if (expander.submit(frame, &stable_first) != SACCADE_OK ||
        expander.wait(stable_first, UINT64_C(1000000000)) != SACCADE_OK) {
        return false;
    }
    SaccadeOverlayTarget mutated{};
    std::memcpy(&mutated, packet_bytes.data() + sizeof(SaccadeOverlayPacketHeader), sizeof(mutated));
    mutated.style_index = UINT8_MAX;
    store(sizeof(SaccadeOverlayPacketHeader), mutated);
    Submission stable_second{};
    if (expander.submit(frame, &stable_second) != SACCADE_OK ||
        expander.wait(stable_second, UINT64_C(1000000000)) != SACCADE_OK) {
        return false;
    }
    size_t stable_count = 0;
    if (expander.copy_instances(stable_second, {actual_rects.data(), actual_metadata.data(), 5}, &stable_count) !=
            SACCADE_OK ||
        stable_count != 5 || saccade_overlay_instance_meta_style(actual_metadata[0]) != 0) {
        return false;
    }

    const SaccadeSpanU8 empty_packet = build_packet(0, 60);
    frame.scene_epoch = 60;
    frame.packet = empty_packet;
    Submission empty{};
    size_t empty_count = 1;
    if (expander.submit(frame, &empty) != SACCADE_OK || expander.wait(empty, UINT64_C(1000000000)) != SACCADE_OK ||
        expander.copy_instances(empty, {}, &empty_count) != SACCADE_OK || empty_count != 0) {
        return false;
    }

    for (uint32_t index = 0; index < 3; ++index) {
        Submission replacement{};
        if (expander.submit(frame, &replacement) != SACCADE_OK ||
            expander.wait(replacement, UINT64_C(1000000000)) != SACCADE_OK) {
            return false;
        }
    }
    if (expander.poll(first, &complete) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }

    SaccadeMemoryStats invalid_memory{};
    return expander.memory_stats(&invalid_memory) == SACCADE_ERROR_INVALID_ARGUMENT;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return to_process_exit_code(ExitCode::invalid_arguments);
    }
    saccade::backend::metal::OverlayExpander available;
    const SaccadeResult availability =
        available.initialize(argv[1], saccade::backend::metal::PathPreference::automatic);
    if (availability == SACCADE_ERROR_UNSUPPORTED) {
        return to_process_exit_code(ExitCode::unsupported);
    }
    if (availability != SACCADE_OK) {
        return to_process_exit_code(ExitCode::initialize);
    }
    if (!exercise_path(argv[1], saccade::backend::metal::PathPreference::automatic)) {
        return to_process_exit_code(ExitCode::automatic_path);
    }
    if (!exercise_path(argv[1], saccade::backend::metal::PathPreference::metal3)) {
        return to_process_exit_code(ExitCode::metal3_path);
    }
    if (!exercise_path(argv[1], saccade::backend::metal::PathPreference::metal4)) {
        return to_process_exit_code(ExitCode::metal4_path);
    }
    if (!exercise_contract(argv[1])) {
        return to_process_exit_code(ExitCode::contract);
    }
    return to_process_exit_code(ExitCode::success);
}

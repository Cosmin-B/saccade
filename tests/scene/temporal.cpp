#include "scene/temporal.hpp"
#include "../support/allocation_tracker.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    allocation_tracker_failed = 1,
    initialize_failed = 2,
    baseline_failed = 3,
    delta_failed = 4,
    payload_failed = 5,
    capacity_failed = 6,
    translation_failed = 7,
    reset_failed = 8,
    allocation_detected = 9,
    overlapping_windows_failed = 10
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

struct PacketStorage {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, 4> targets{};
    std::array<uint8_t, 32> text{};
};

SaccadeTargetRecord target(uint64_t id, int32_t x, uint16_t text_offset, uint16_t text_size, uint64_t window_id = 11,
                           uint64_t display_id = 12) noexcept {
    SaccadeTargetRecord value{};
    value.target_id = id;
    value.window_id = window_id;
    value.display_id = display_id;
    value.x_q8 = x;
    value.y_q8 = 200;
    value.width_q8 = 100;
    value.height_q8 = 80;
    value.safe_x_q8 = x + 50;
    value.safe_y_q8 = 240;
    value.confidence_q16 = 50000;
    value.role = SACCADE_TARGET_ROLE_BUTTON;
    value.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    value.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    value.flags = SACCADE_TARGET_ACTIONABLE;
    value.order = static_cast<uint32_t>(id);
    value.text = {text_offset, text_size};
    return value;
}

void initialize(PacketStorage* packet, uint64_t scene_epoch, uint64_t frame_id, uint64_t session_epoch,
                uint64_t transform_epoch, uint32_t count, const char* text) noexcept {
    packet->header = {};
    packet->header.struct_size = sizeof(packet->header);
    packet->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    packet->header.target_count = count;
    packet->header.target_stride = sizeof(SaccadeTargetRecord);
    packet->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    packet->header.scene_epoch = scene_epoch;
    packet->header.frame_id = frame_id;
    packet->header.capture_time_ns = 1000 + frame_id;
    packet->header.model_epoch = 4;
    packet->header.session_epoch = session_epoch;
    packet->header.transform_epoch = transform_epoch;
    packet->header.topology_epoch = 6;
    packet->header.source_id = 7;
    packet->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    const size_t text_size = std::strlen(text);
    std::memcpy(packet->text.data(), text, text_size);
    packet->header.total_size =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(count) * sizeof(SaccadeTargetRecord) + text_size;
}

saccade::scene::PacketView view(PacketStorage* packet, size_t text_size) noexcept {
    return {&packet->header, packet->targets.data(), static_cast<size_t>(packet->header.total_size),
            packet->text.data(), static_cast<uint32_t>(text_size)};
}

const SaccadeSceneDeltaHeader* header(const std::array<uint8_t, saccade::scene::temporal_delta_max_bytes>& bytes) {
    return reinterpret_cast<const SaccadeSceneDeltaHeader*>(bytes.data());
}

const SaccadeSceneDeltaRecord* records(const std::array<uint8_t, saccade::scene::temporal_delta_max_bytes>& bytes) {
    return reinterpret_cast<const SaccadeSceneDeltaRecord*>(bytes.data() + sizeof(SaccadeSceneDeltaHeader));
}

} // namespace

int main() {
    if (!saccade::test::allocation_tracker_self_test()) {
        return exit_code(ExitCode::allocation_tracker_failed);
    }

    static saccade::scene::TemporalStorage storage;
    saccade::scene::TemporalCompiler compiler;
    if (compiler.initialize(&storage) != SACCADE_OK) {
        return exit_code(ExitCode::initialize_failed);
    }

    alignas(SaccadeSceneDeltaHeader) static std::array<uint8_t, saccade::scene::temporal_delta_max_bytes> output{};
    PacketStorage first{};
    initialize(&first, 1, 10, 3, 5, 3, "OneTwo");
    first.targets[0] = target(1, 100, 0, 3);
    first.targets[1] = target(2, 300, 3, 3);
    first.targets[2] = target(3, 500, 0, 0);

    size_t required = 0;
    saccade::scene::TemporalStats stats{};
    if (compiler.compile(view(&first, 6), {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        header(output)->operation_count != 3 || (header(output)->flags & SACCADE_SCENE_DELTA_BASELINE) == 0 ||
        stats.additions != 3 || stats.updates != 0 || stats.removals != 0 ||
        records(output)[0].operation != SACCADE_SCENE_DELTA_ADD || records(output)[0].window_id != 11 ||
        (records(output)[0].changed_fields & SACCADE_SCENE_DELTA_TEXT) == 0) {
        return exit_code(ExitCode::baseline_failed);
    }
    SaccadeSceneDeltaOwner owner{};
    std::memcpy(&owner, output.data() + records(output)[0].payload_offset, sizeof(owner));
    if (owner.display_id != 12) {
        return exit_code(ExitCode::baseline_failed);
    }

    PacketStorage second{};
    initialize(&second, 2, 11, 3, 5, 3, "OneChanged");
    second.targets[0] = target(1, 100, 0, 3);
    second.targets[1] = target(2, 332, 3, 7);
    second.targets[1].confidence_q16 = 51000;
    second.targets[2] = target(4, 700, 0, 0);
    if (compiler.compile(view(&second, 10), {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        stats.additions != 1 || stats.updates != 1 || stats.removals != 1 || header(output)->operation_count != 3 ||
        records(output)[0].target_id != 2 || records(output)[0].operation != SACCADE_SCENE_DELTA_UPDATE ||
        records(output)[0].changed_fields !=
            (SACCADE_SCENE_DELTA_GEOMETRY | SACCADE_SCENE_DELTA_CONFIDENCE | SACCADE_SCENE_DELTA_TEXT) ||
        records(output)[1].target_id != 4 || records(output)[1].operation != SACCADE_SCENE_DELTA_ADD ||
        records(output)[2].target_id != 3 || records(output)[2].operation != SACCADE_SCENE_DELTA_REMOVE) {
        return exit_code(ExitCode::delta_failed);
    }
    const SaccadeSceneDeltaRecord& update = records(output)[0];
    const uint8_t* update_payload = output.data() + update.payload_offset;
    SaccadeSceneDeltaGeometry geometry{};
    std::memcpy(&geometry, update_payload, sizeof(geometry));
    uint32_t confidence = 0;
    std::memcpy(&confidence, update_payload + sizeof(geometry), sizeof(confidence));
    const size_t text_offset = sizeof(geometry) + sizeof(confidence);
    if (geometry.x_q8 != 332 || confidence != 51000 || update.payload_size != text_offset + 7 ||
        std::memcmp(update_payload + text_offset, "Changed", 7) != 0) {
        return exit_code(ExitCode::payload_failed);
    }

    PacketStorage capacity{};
    initialize(&capacity, 3, 12, 3, 5, 3, "OneChanged");
    capacity.targets = second.targets;
    size_t exact_required = 0;
    if (compiler.compile(view(&capacity, 10), {output.data(), sizeof(SaccadeSceneDeltaHeader) - 1U}, &exact_required,
                         &stats) != SACCADE_ERROR_CAPACITY ||
        exact_required != sizeof(SaccadeSceneDeltaHeader)) {
        return exit_code(ExitCode::capacity_failed);
    }

    PacketStorage translated{};
    initialize(&translated, 3, 12, 3, 6, 3, "OneChanged");
    translated.targets = second.targets;
    for (uint32_t index = 0; index < translated.header.target_count; ++index) {
        translated.targets[index].x_q8 += 64;
        translated.targets[index].y_q8 -= 32;
        translated.targets[index].safe_x_q8 += 64;
        translated.targets[index].safe_y_q8 -= 32;
    }
    if (compiler.compile(view(&translated, 10), {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        header(output)->operation_count != 1 || (header(output)->flags & SACCADE_SCENE_DELTA_WINDOW_TRANSFORMS) == 0 ||
        records(output)[0].operation != SACCADE_SCENE_DELTA_WINDOW_TRANSFORM || records(output)[0].window_id != 11 ||
        stats.window_transforms != 1) {
        return exit_code(ExitCode::translation_failed);
    }
    SaccadeSceneWindowTransform transform{};
    std::memcpy(&transform, output.data() + records(output)[0].payload_offset, sizeof(transform));
    if (transform.display_id != 12 || transform.translation_x_q8 != 64 || transform.translation_y_q8 != -32 ||
        transform.target_count != 3) {
        return exit_code(ExitCode::translation_failed);
    }

    static saccade::scene::TemporalStorage overlapping_storage;
    saccade::scene::TemporalCompiler overlapping_compiler;
    PacketStorage overlapping_first{};
    PacketStorage overlapping_second{};
    initialize(&overlapping_first, 1, 20, 8, 10, 4, "");
    overlapping_first.targets[0] = target(1, 100, 0, 0, 21, 31);
    overlapping_first.targets[1] = target(2, 300, 0, 0, 21, 31);
    overlapping_first.targets[2] = target(1, 100, 0, 0, 22, 31);
    overlapping_first.targets[3] = target(2, 300, 0, 0, 22, 31);
    initialize(&overlapping_second, 2, 21, 8, 11, 4, "");
    overlapping_second.header.topology_epoch = 12;
    overlapping_second.targets = overlapping_first.targets;
    for (uint32_t index = 0; index < 2; ++index) {
        overlapping_second.targets[index].x_q8 += 40;
        overlapping_second.targets[index].safe_x_q8 += 40;
    }
    for (uint32_t index = 2; index < 4; ++index) {
        overlapping_second.targets[index].x_q8 -= 24;
        overlapping_second.targets[index].safe_x_q8 -= 24;
    }
    if (overlapping_compiler.initialize(&overlapping_storage) != SACCADE_OK ||
        overlapping_compiler.compile(view(&overlapping_first, 0), {output.data(), output.size()}, &required, &stats) !=
            SACCADE_OK ||
        overlapping_compiler.compile(view(&overlapping_second, 0), {output.data(), output.size()}, &required, &stats) !=
            SACCADE_OK ||
        stats.window_transforms != 2 || stats.updates != 0 || header(output)->operation_count != 2 ||
        (header(output)->flags & SACCADE_SCENE_DELTA_TOPOLOGY_CHANGED) == 0 ||
        records(output)[0].operation != SACCADE_SCENE_DELTA_WINDOW_TRANSFORM || records(output)[0].window_id != 21 ||
        records(output)[1].operation != SACCADE_SCENE_DELTA_WINDOW_TRANSFORM || records(output)[1].window_id != 22) {
        return exit_code(ExitCode::overlapping_windows_failed);
    }

    PacketStorage reset{};
    initialize(&reset, 1, 1, 4, 1, 1, "New");
    reset.targets[0] = target(9, 900, 0, 3);
    if (compiler.compile(view(&reset, 3), {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        (header(output)->flags & SACCADE_SCENE_DELTA_RESET) == 0 || stats.removals != 3 || stats.additions != 1 ||
        records(output)[0].operation != SACCADE_SCENE_DELTA_REMOVE ||
        records(output)[3].operation != SACCADE_SCENE_DELTA_ADD) {
        return exit_code(ExitCode::reset_failed);
    }

    PacketStorage measured{};
    initialize(&measured, 2, 2, 4, 1, 1, "New");
    measured.targets[0] = reset.targets[0];
    measured.targets[0].confidence_q16 = 52000;
    saccade::test::begin_allocation_tracking();
    const SaccadeResult measured_result =
        compiler.compile(view(&measured, 3), {output.data(), output.size()}, &required, &stats);
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (measured_result != SACCADE_OK || allocations != 0) {
        return exit_code(ExitCode::allocation_detected);
    }

    return exit_code(ExitCode::success);
}

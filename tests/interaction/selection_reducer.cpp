#include "interaction/selection_reducer.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_scene = 1,
    single_selection = 2,
    dual_selection = 3,
    multi_selection = 4,
    path_selection = 5,
    duplicate_selection = 6,
    focus_changed = 7,
    timeout = 8,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t target_count = 8;
constexpr size_t packet_size = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord);

struct alignas(8) SceneStorage {
    std::array<uint8_t, packet_size> bytes{};
};

void make_scene(SceneStorage* storage) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = 1;
    header.frame_id = 2;
    header.model_epoch = 3;
    header.session_epoch = 4;
    header.transform_epoch = 5;
    header.topology_epoch = 6;
    header.source_id = 7;
    header.targets_offset = sizeof(header);
    header.total_size = storage->bytes.size();
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(storage->bytes.data() + sizeof(header));
    for (uint32_t index = 0; index < target_count; ++index) {
        targets[index].target_id = 100 + index;
        targets[index].width_q8 = 256;
        targets[index].height_q8 = 256;
        targets[index].confidence_q16 = UINT16_MAX;
        targets[index].source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        targets[index].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
        targets[index].flags = SACCADE_TARGET_ACTIONABLE;
        targets[index].order = index * UINT32_C(100000000);
    }
}

saccade::interaction::SelectionContext context() noexcept {
    return {1, 5, 6, 9, 1000};
}

} // namespace

int main() {
    static SceneStorage scene_storage;
    static saccade::interaction::SelectionStorage selection_storage;
    make_scene(&scene_storage);
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet({scene_storage.bytes.data(), scene_storage.bytes.size()}, &scene) !=
        SACCADE_OK) {
        return to_process_exit_code(ExitCode::invalid_scene);
    }

    saccade::interaction::SelectionReducer reducer;
    auto current = context();
    saccade::test::begin_allocation_tracking();
    if (reducer.begin(scene, saccade::interaction::SelectionMode::single, current, &selection_storage) != SACCADE_OK ||
        reducer.select(103) != SACCADE_OK || reducer.view().state != saccade::interaction::SelectionState::complete ||
        reducer.view().target_count != 1 || reducer.reset() != SACCADE_OK) {
        return to_process_exit_code(ExitCode::single_selection);
    }

    if (reducer.begin(scene, saccade::interaction::SelectionMode::dual, current, &selection_storage) != SACCADE_OK ||
        reducer.select(101) != SACCADE_OK || reducer.select(106) != SACCADE_OK ||
        reducer.view().state != saccade::interaction::SelectionState::complete || reducer.view().target_count != 2 ||
        reducer.reset() != SACCADE_OK) {
        return to_process_exit_code(ExitCode::dual_selection);
    }

    if (reducer.begin(scene, saccade::interaction::SelectionMode::multi, current, &selection_storage) != SACCADE_OK ||
        reducer.select(100) != SACCADE_OK || reducer.select(102) != SACCADE_OK || reducer.select(104) != SACCADE_OK ||
        reducer.backspace() != SACCADE_OK || reducer.confirm() != SACCADE_OK || reducer.view().target_count != 2 ||
        reducer.view().target_ids[1] != 102 || reducer.reset() != SACCADE_OK) {
        return to_process_exit_code(ExitCode::multi_selection);
    }

    if (reducer.begin(scene, saccade::interaction::SelectionMode::path, current, &selection_storage) != SACCADE_OK ||
        reducer.select(106) != SACCADE_OK || reducer.select(104) != SACCADE_OK ||
        reducer.view().state != saccade::interaction::SelectionState::collecting || reducer.select(102) != SACCADE_OK ||
        reducer.confirm() != SACCADE_OK || reducer.view().state != saccade::interaction::SelectionState::complete ||
        reducer.view().target_count != 5 || reducer.view().target_ids[0] != 106 ||
        reducer.view().target_ids[1] != 105 || reducer.view().target_ids[2] != 104 ||
        reducer.view().target_ids[3] != 103 || reducer.view().target_ids[4] != 102 || reducer.reset() != SACCADE_OK) {
        return to_process_exit_code(ExitCode::path_selection);
    }

    if (reducer.begin(scene, saccade::interaction::SelectionMode::multi, current, &selection_storage) != SACCADE_OK ||
        reducer.select(100) != SACCADE_OK || reducer.select(100) != SACCADE_ERROR_ALREADY_EXISTS) {
        return to_process_exit_code(ExitCode::duplicate_selection);
    }
    current.focus_id = 10;
    if (reducer.validate(current, 100) != SACCADE_ERROR_STALE_HANDLE ||
        reducer.view().state != saccade::interaction::SelectionState::cancelled ||
        reducer.view().cancel_reason != saccade::interaction::SelectionCancelReason::focus_changed ||
        reducer.reset() != SACCADE_OK) {
        return to_process_exit_code(ExitCode::focus_changed);
    }

    current = context();
    if (reducer.begin(scene, saccade::interaction::SelectionMode::multi, current, &selection_storage) != SACCADE_OK ||
        reducer.validate(current, 1000) != SACCADE_ERROR_STALE_HANDLE ||
        reducer.view().cancel_reason != saccade::interaction::SelectionCancelReason::timeout ||
        saccade::test::end_allocation_tracking() != 0) {
        return to_process_exit_code(ExitCode::timeout);
    }
    return to_process_exit_code(ExitCode::success);
}

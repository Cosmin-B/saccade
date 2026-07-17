#include "application/debugger.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t target_count = saccade::application::maximum_debug_target_samples + 1U;
constexpr size_t packet_size =
    sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(target_count) * sizeof(SaccadeTargetRecord);

enum ExitCode : int {
    initialize_failed = 1,
    capture_failed,
    frame_view_failed,
    scene_view_failed,
    capture_allocated,
    invalid_context_accepted,
    dry_run_failed,
    dry_run_allocated,
    replay_failed,
    final_state_failed
};

void make_scene(std::array<uint8_t, packet_size>* bytes) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = 11;
    header.frame_id = 12;
    header.model_epoch = 13;
    header.session_epoch = 14;
    header.transform_epoch = 15;
    header.topology_epoch = 16;
    header.source_id = 17;
    header.targets_offset = sizeof(header);
    header.total_size = bytes->size();
    std::memcpy(bytes->data(), &header, sizeof(header));

    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(bytes->data() + sizeof(header));
    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeTargetRecord& target = targets[index];
        target.target_id = 101U + index;
        target.window_id = 201;
        target.display_id = 301;
        target.x_q8 = 1280 + static_cast<int32_t>(index) * 1024;
        target.y_q8 = 2048;
        target.width_q8 = 512;
        target.height_q8 = 512;
        target.safe_x_q8 = target.x_q8 + 220;
        target.safe_y_q8 = 2300;
        target.confidence_q16 = UINT16_MAX;
        target.role = SACCADE_TARGET_ROLE_BUTTON;
        target.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_BUTTON;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }

    targets[1].role = SACCADE_TARGET_ROLE_TEXT_FIELD;
    targets[1].source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    targets[1].capability_bits = 0;
    targets[1].flags = SACCADE_TARGET_DISABLED | SACCADE_TARGET_SECURE;
    targets[2].role = SACCADE_TARGET_ROLE_LINK;
    targets[2].source_bits = SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    targets[2].flags = SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_OCCLUDED | SACCADE_TARGET_APPROXIMATE;
}

} // namespace

int main() {
    static saccade::application::DebuggerStorage storage;
    saccade::application::Debugger debugger;
    if (debugger.initialize(&storage) != SACCADE_OK || debugger.replay(nullptr) != SACCADE_ERROR_STATE)
        return initialize_failed;

    std::array<uint8_t, packet_size> bytes{};
    make_scene(&bytes);
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet({bytes.data(), bytes.size()}, &scene) != SACCADE_OK) return capture_failed;

    saccade::application::DebuggerTransformRecord transform{};
    transform.source_id = 17;
    transform.display_id = 301;
    transform.transform.source = {0, 0, 1920 * 256, 1080 * 256};
    transform.transform.destination = {-1280 * 256, 0, 1920 * 256, 1080 * 256};
    transform.transform.epoch = 15;
    transform.transform.source_space = saccade::geometry::CoordinateSpace::capture;
    transform.transform.destination_space = saccade::geometry::CoordinateSpace::desktop;

    saccade::scene::FusionStats fusion{};
    fusion.packets_read = 2;
    fusion.candidates_read = target_count + 4U;
    fusion.duplicates_merged = 4;
    fusion.targets_written = target_count;
    saccade::application::DebuggerCaptureContext capture{};
    capture.timestamp_ns = 123456;
    capture.transforms = &transform;
    capture.fusion = &fusion;
    capture.transform_count = 1;
    capture.fusion_input_count = 2;

    saccade::test::begin_allocation_tracking();
    const SaccadeResult captured = debugger.capture_scene(scene, capture);
    const size_t capture_allocations = saccade::test::end_allocation_tracking();
    if (captured != SACCADE_OK) return capture_failed;
    if (capture_allocations != 0) return capture_allocated;

    const saccade::application::DebuggerFramesTransformsView frame_view = debugger.frames_transforms();
    if (frame_view.frame.scene.frame_id != 12 || frame_view.frame.scene.target_count != target_count ||
        frame_view.frame.timestamp_ns != capture.timestamp_ns || frame_view.frame.byte_size != bytes.size() ||
        frame_view.transform_count != 1 || frame_view.transforms[0].source_id != transform.source_id ||
        frame_view.transforms[0].display_id != transform.display_id ||
        frame_view.transforms[0].transform.epoch != transform.transform.epoch) {
        return frame_view_failed;
    }

    const saccade::application::DebuggerSceneFusionView scene_view = debugger.scene_fusion();
    if (scene_view.scene.scene_epoch != 11 || scene_view.timestamp_ns != capture.timestamp_ns ||
        scene_view.targets.target_count != target_count || scene_view.targets.actionable != target_count - 1U ||
        scene_view.targets.disabled != 1 || scene_view.targets.occluded != 1 || scene_view.targets.secure != 1 ||
        scene_view.targets.approximate != 1 || scene_view.targets.neural != target_count - 1U ||
        scene_view.targets.accessibility != 2 || scene_view.targets.fused != 1 ||
        scene_view.targets.roles[SACCADE_TARGET_ROLE_BUTTON] != target_count - 2U ||
        scene_view.sample_count != saccade::application::maximum_debug_target_samples ||
        scene_view.samples_omitted != 1 || scene_view.samples[0].target_id != 101 ||
        scene_view.samples.back().target_id != 101U + saccade::application::maximum_debug_target_samples - 1U ||
        scene_view.fusion_input_count != 2 || scene_view.fusion.packets_read != 2 ||
        scene_view.fusion.targets_written != target_count) {
        return scene_view_failed;
    }

    saccade::application::DebuggerTransformRecord stale_transform = transform;
    stale_transform.transform.epoch = transform.transform.epoch - 1U;
    saccade::application::DebuggerCaptureContext invalid_capture = capture;
    invalid_capture.transforms = &stale_transform;
    if (debugger.capture_scene(scene, invalid_capture) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return invalid_context_accepted;
    }
    invalid_capture = capture;
    invalid_capture.transform_count = saccade::application::maximum_debug_transforms + 1U;
    if (debugger.capture_scene(scene, invalid_capture) != SACCADE_ERROR_INVALID_ARGUMENT ||
        debugger.frames_transforms().frame.scene.frame_id != 12) {
        return invalid_context_accepted;
    }

    auto* source_target = reinterpret_cast<SaccadeTargetRecord*>(bytes.data() + sizeof(SaccadeTargetPacketHeader));
    source_target->target_id = 999;
    const uint64_t target_id = 101;
    saccade::interaction::ActionContext context{};
    context.plan_id = 1;
    context.scene_epoch = 11;
    context.transform_epoch = 15;
    context.topology_epoch = 16;
    context.permission_epoch = 21;
    context.focus_id = 201;
    context.now_ns = 100;
    context.deadline_ns = 1000;
    context.permissions = SACCADE_INPUT_PERMISSION_POINTER;
    saccade::interaction::ActionRequest request{};
    request.kind = saccade::interaction::ActionKind::click;
    request.target_ids = &target_id;
    request.target_count = 1;

    saccade::application::DebuggerPlanView dry_run{};
    saccade::test::begin_allocation_tracking();
    if (debugger.dry_run(context, request, &dry_run) != SACCADE_OK || dry_run.plan.header == nullptr ||
        dry_run.plan.header->command_count != 1 || dry_run.plan.commands[0].target_id != target_id)
        return dry_run_failed;
    const size_t allocation_count = saccade::test::end_allocation_tracking();
    if (allocation_count != 0) return dry_run_allocated;

    saccade::application::DebuggerPlanView replay{};
    if (debugger.replay(&replay) != SACCADE_OK || replay.bytes.size != dry_run.bytes.size ||
        std::memcmp(replay.bytes.data, dry_run.bytes.data, replay.bytes.size) != 0)
        return replay_failed;
    const saccade::application::DebuggerStats stats = debugger.stats();
    if (stats.scenes_captured != 1 || stats.scene_bytes_copied != bytes.size() || stats.dry_runs != 1 ||
        stats.replays != 1 || stats.replay_mismatches != 0 ||
        debugger.arm_fault(saccade::application::DebugFaultPoint::capture, 2, SACCADE_ERROR_BACKEND) != SACCADE_OK ||
        debugger.consume_fault(saccade::application::DebugFaultPoint::capture) != SACCADE_ERROR_BACKEND ||
        debugger.consume_fault(saccade::application::DebugFaultPoint::capture) != SACCADE_ERROR_BACKEND ||
        debugger.consume_fault(saccade::application::DebugFaultPoint::capture) != SACCADE_OK ||
        debugger.arm_fault(saccade::application::DebugFaultPoint::capture, 17, SACCADE_ERROR_BACKEND) !=
            SACCADE_ERROR_INVALID_ARGUMENT ||
        debugger.clear() != SACCADE_OK || debugger.has_scene() || debugger.has_plan() ||
        debugger.frames_transforms().frame.scene.struct_size != 0 || debugger.scene_fusion().scene.struct_size != 0)
        return final_state_failed;
    return 0;
}

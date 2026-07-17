#include "application/debugger.hpp"

#include <bit>
#include <cstring>

namespace saccade::application {
namespace {

constexpr uint16_t target_source_mask = SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_ACCESSIBILITY |
                                        SACCADE_TARGET_SOURCE_PIXEL | SACCADE_TARGET_SOURCE_GRID;

bool capture_context_valid(const DebuggerCaptureContext& context, uint64_t transform_epoch) noexcept {
    if ((context.transform_count == 0) != (context.transforms == nullptr) ||
        context.transform_count > maximum_debug_transforms ||
        (context.fusion_input_count == 0) != (context.fusion == nullptr) || context.fusion_input_count > 4) {
        return false;
    }

    for (uint32_t index = 0; index < context.transform_count; ++index) {
        const DebuggerTransformRecord& record = context.transforms[index];
        geometry::CoordinateTransform transform;
        if (record.source_id == 0 || record.display_id == 0 || record.transform.epoch != transform_epoch ||
            transform.initialize(record.transform) != SACCADE_OK) {
            return false;
        }
    }

    return context.fusion == nullptr ||
           (context.fusion->targets_written <= SACCADE_TARGET_PACKET_MAX_TARGETS && context.fusion->reserved == 0);
}

DebuggerTargetSample target_sample(const SaccadeTargetRecord& target) noexcept {
    DebuggerTargetSample sample{};
    sample.target_id = target.target_id;
    sample.parent_id = target.parent_id;
    sample.window_id = target.window_id;
    sample.display_id = target.display_id;
    sample.bounds = {target.x_q8, target.y_q8, target.width_q8, target.height_q8};
    sample.safe_point = {target.safe_x_q8, target.safe_y_q8};
    sample.confidence_q16 = target.confidence_q16;
    sample.capability_bits = target.capability_bits;
    sample.flags = target.flags;
    sample.order = target.order;
    sample.role = target.role;
    sample.source_bits = target.source_bits;
    sample.text_size = target.text.size;
    return sample;
}

void count_target(const SaccadeTargetRecord& target, DebuggerTargetSummary* summary) noexcept {
    ++summary->target_count;
    ++summary->roles[target.role];
    summary->actionable += (target.flags & SACCADE_TARGET_ACTIONABLE) != 0 ? 1U : 0U;
    summary->disabled += (target.flags & SACCADE_TARGET_DISABLED) != 0 ? 1U : 0U;
    summary->occluded += (target.flags & SACCADE_TARGET_OCCLUDED) != 0 ? 1U : 0U;
    summary->secure += (target.flags & SACCADE_TARGET_SECURE) != 0 ? 1U : 0U;
    summary->approximate += (target.flags & SACCADE_TARGET_APPROXIMATE) != 0 ? 1U : 0U;
    summary->text_redacted += (target.flags & SACCADE_TARGET_TEXT_REDACTED) != 0 ? 1U : 0U;
    summary->text_truncated += (target.flags & SACCADE_TARGET_TEXT_TRUNCATED) != 0 ? 1U : 0U;
    summary->neural += (target.source_bits & SACCADE_TARGET_SOURCE_NEURAL) != 0 ? 1U : 0U;
    summary->accessibility += (target.source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) != 0 ? 1U : 0U;
    summary->pixel += (target.source_bits & SACCADE_TARGET_SOURCE_PIXEL) != 0 ? 1U : 0U;
    summary->grid += (target.source_bits & SACCADE_TARGET_SOURCE_GRID) != 0 ? 1U : 0U;
    summary->fused += std::popcount(static_cast<uint16_t>(target.source_bits & target_source_mask)) > 1 ? 1U : 0U;
    summary->capability_bits |= target.capability_bits;
    summary->text_bytes += target.text.size;
}

} // namespace

SaccadeResult Debugger::reject(SaccadeResult result) noexcept {
    ++stats_.rejected;
    return result;
}

SaccadeResult Debugger::initialize(DebuggerStorage* storage) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (storage == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    storage_ = storage;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult Debugger::capture_scene(const scene::PacketView& source) noexcept {
    return capture_scene(source, {});
}

SaccadeResult Debugger::capture_scene(const scene::PacketView& source, const DebuggerCaptureContext& context) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if (source.header == nullptr || source.targets == nullptr || source.byte_size < sizeof(SaccadeTargetPacketHeader) ||
        source.byte_size > storage_->scene.size() || !capture_context_valid(context, source.header->transform_epoch)) {
        return reject(SACCADE_ERROR_INVALID_ARGUMENT);
    }

    std::memcpy(storage_->scene.data(), source.header, source.byte_size);
    scene::PacketView captured{};
    const SaccadeResult result = scene::validate_packet({storage_->scene.data(), source.byte_size}, &captured);
    if (result != SACCADE_OK) return reject(result);

    DebuggerFramesTransformsView frames_transforms{};
    frames_transforms.frame.scene = *captured.header;
    frames_transforms.frame.timestamp_ns = context.timestamp_ns;
    frames_transforms.frame.byte_size = captured.byte_size;
    frames_transforms.transform_count = context.transform_count;
    for (uint32_t index = 0; index < context.transform_count; ++index)
        frames_transforms.transforms[index] = context.transforms[index];

    DebuggerSceneFusionView scene_fusion{};
    scene_fusion.scene = *captured.header;
    scene_fusion.timestamp_ns = context.timestamp_ns;
    scene_fusion.fusion_input_count = context.fusion_input_count;
    if (context.fusion != nullptr) scene_fusion.fusion = *context.fusion;
    scene_fusion.sample_count = captured.header->target_count > maximum_debug_target_samples
                                    ? maximum_debug_target_samples
                                    : captured.header->target_count;
    scene_fusion.samples_omitted = captured.header->target_count - scene_fusion.sample_count;
    for (uint32_t index = 0; index < captured.header->target_count; ++index) {
        const SaccadeTargetRecord& target = captured.targets[index];
        count_target(target, &scene_fusion.targets);
        if (index < scene_fusion.sample_count) scene_fusion.samples[index] = target_sample(target);
    }

    scene_ = captured;
    frames_transforms_ = frames_transforms;
    scene_fusion_ = scene_fusion;
    plan_size_ = 0;
    ++stats_.scenes_captured;
    stats_.scene_bytes_copied += source.byte_size;
    return SACCADE_OK;
}

SaccadeResult Debugger::copy_request(const interaction::ActionRequest& source) noexcept {
    if (source.target_count > interaction::maximum_action_targets ||
        (source.target_count != 0 && source.target_ids == nullptr) ||
        (source.target_point_count != 0 && source.target_points == nullptr) ||
        source.text.size > storage_->text.size() || (source.text.size != 0 && source.text.data == nullptr))
        return SACCADE_ERROR_INVALID_ARGUMENT;

    request_ = source;
    if (source.target_count != 0) {
        std::memcpy(storage_->target_ids.data(), source.target_ids, source.target_count * sizeof(uint64_t));
        request_.target_ids = storage_->target_ids.data();
    }
    if (source.target_point_count != 0) {
        std::memcpy(storage_->target_points.data(), source.target_points,
                    source.target_point_count * sizeof(geometry::PointQ8));
        request_.target_points = storage_->target_points.data();
    }
    if (source.text.size != 0) {
        std::memcpy(storage_->text.data(), source.text.data, source.text.size);
        request_.text = {storage_->text.data(), source.text.size};
    }
    return SACCADE_OK;
}

SaccadeResult Debugger::dry_run(const interaction::ActionContext& context, const interaction::ActionRequest& request,
                                DebuggerPlanView* output) noexcept {
    if (!initialized_ || !has_scene()) return SACCADE_ERROR_STATE;
    if (output == nullptr) return reject(SACCADE_ERROR_INVALID_ARGUMENT);
    *output = {};

    const SaccadeResult copied = copy_request(request);
    if (copied != SACCADE_OK) return reject(copied);
    context_ = context;

    SaccadeSpanU8 bytes{};
    const SaccadeResult built = planner_.build(scene_, context_, request_, &storage_->plan, &bytes);
    if (built != SACCADE_OK) return reject(built);
    input::PlanView plan{};
    const SaccadeResult validated = input::validate_plan(bytes, &plan);
    if (validated != SACCADE_OK) return reject(validated);

    plan_size_ = bytes.size;
    *output = {bytes, plan};
    ++stats_.dry_runs;
    return SACCADE_OK;
}

SaccadeResult Debugger::dry_run_first_click(uint64_t now_ns, DebuggerPlanView* output) noexcept {
    if (!initialized_ || !has_scene()) return SACCADE_ERROR_STATE;
    if (now_ns == 0 || now_ns > UINT64_MAX - UINT64_C(1'000'000'000)) return reject(SACCADE_ERROR_INVALID_ARGUMENT);

    const SaccadeTargetRecord* target = nullptr;
    for (uint32_t index = 0; index < scene_.header->target_count; ++index) {
        const SaccadeTargetRecord& candidate = scene_.targets[index];
        if ((candidate.flags & SACCADE_TARGET_ACTIONABLE) != 0 &&
            (candidate.flags & (SACCADE_TARGET_DISABLED | SACCADE_TARGET_OCCLUDED | SACCADE_TARGET_SECURE)) == 0 &&
            (candidate.capability_bits & SACCADE_TARGET_CAPABILITY_BUTTON) != 0) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr) return reject(SACCADE_ERROR_NOT_FOUND);

    interaction::ActionContext context{};
    context.plan_id = next_plan_id_++;
    context.scene_epoch = scene_.header->scene_epoch;
    context.transform_epoch = scene_.header->transform_epoch;
    context.topology_epoch = scene_.header->topology_epoch;
    context.permission_epoch = 1;
    context.focus_id = target->window_id == 0 ? target->target_id : target->window_id;
    context.now_ns = now_ns;
    context.deadline_ns = now_ns + UINT64_C(1'000'000'000);
    context.permissions = SACCADE_INPUT_PERMISSION_POINTER;
    interaction::ActionRequest request{};
    request.kind = interaction::ActionKind::click;
    request.target_ids = &target->target_id;
    request.target_count = 1;
    return dry_run(context, request, output);
}

SaccadeResult Debugger::replay(DebuggerPlanView* output) noexcept {
    if (!initialized_ || !has_plan()) return SACCADE_ERROR_STATE;
    if (output == nullptr) return reject(SACCADE_ERROR_INVALID_ARGUMENT);
    *output = {};

    SaccadeSpanU8 bytes{};
    const SaccadeResult built = planner_.build(scene_, context_, request_, &storage_->replay, &bytes);
    if (built != SACCADE_OK) return reject(built);
    if (bytes.size != plan_size_ || std::memcmp(storage_->plan.bytes.data(), bytes.data, plan_size_) != 0) {
        ++stats_.replay_mismatches;
        return SACCADE_ERROR_STATE;
    }
    input::PlanView plan{};
    const SaccadeResult validated = input::validate_plan(bytes, &plan);
    if (validated != SACCADE_OK) return reject(validated);

    *output = {bytes, plan};
    ++stats_.replays;
    return SACCADE_OK;
}

SaccadeResult Debugger::arm_fault(DebugFaultPoint point, uint32_t count, SaccadeResult result) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const uint32_t index = static_cast<uint32_t>(point);
    if (index >= debug_fault_point_count || count == 0 || count > maximum_debug_fault_injections ||
        (result != SACCADE_ERROR_BACKEND && result != SACCADE_ERROR_TIMEOUT && result != SACCADE_ERROR_CANCELLED &&
         result != SACCADE_ERROR_BUSY && result != SACCADE_ERROR_PERMISSION))
        return reject(SACCADE_ERROR_INVALID_ARGUMENT);
    fault_remaining_[index] = static_cast<uint16_t>(count);
    fault_results_[index] = result;
    stats_.faults_armed += count;
    return SACCADE_OK;
}

SaccadeResult Debugger::consume_fault(DebugFaultPoint point) noexcept {
    if (!initialized_) return SACCADE_OK;
    const uint32_t index = static_cast<uint32_t>(point);
    if (index >= debug_fault_point_count || fault_remaining_[index] == 0) return SACCADE_OK;
    --fault_remaining_[index];
    ++stats_.faults_injected;
    return fault_results_[index];
}

SaccadeResult Debugger::clear() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    scene_ = {};
    context_ = {};
    request_ = {};
    plan_size_ = 0;
    next_plan_id_ = 1;
    fault_remaining_.fill(0);
    fault_results_.fill(SACCADE_OK);
    frames_transforms_ = {};
    scene_fusion_ = {};
    ++stats_.clears;
    return SACCADE_OK;
}

} // namespace saccade::application

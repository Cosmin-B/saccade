#ifndef SACCADE_APPLICATION_DEBUGGER_HPP
#define SACCADE_APPLICATION_DEBUGGER_HPP

#include "interaction/action_planner.hpp"
#include "scene/fusion.hpp"
#include "scene/store.hpp"

#include <array>
#include <cstdint>

namespace saccade::application {

enum class DebugFaultPoint : uint32_t { capture = 0, inference = 1, scene = 2, overlay = 3, input = 4 };
constexpr uint32_t debug_fault_point_count = 5;
constexpr uint32_t maximum_debug_fault_injections = 16;
constexpr uint32_t maximum_debug_transforms = 16;
constexpr uint32_t maximum_debug_target_samples = 64;
constexpr uint32_t debug_target_role_count = SACCADE_TARGET_ROLE_WINDOW + 1U;

struct DebuggerTransformRecord {
    uint64_t source_id = 0;
    uint64_t display_id = 0;
    geometry::TransformDesc transform{};
};

struct DebuggerCaptureContext {
    uint64_t timestamp_ns = 0;
    const DebuggerTransformRecord* transforms = nullptr;
    const scene::FusionStats* fusion = nullptr;
    uint32_t transform_count = 0;
    uint32_t fusion_input_count = 0;
};

struct DebuggerFrameRecord {
    SaccadeTargetPacketHeader scene{};
    uint64_t timestamp_ns = 0;
    uint64_t byte_size = 0;
};

struct DebuggerFramesTransformsView {
    DebuggerFrameRecord frame{};
    std::array<DebuggerTransformRecord, maximum_debug_transforms> transforms{};
    uint32_t transform_count = 0;
    uint32_t reserved = 0;
};

struct DebuggerTargetSample {
    uint64_t target_id = 0;
    uint64_t parent_id = 0;
    uint64_t window_id = 0;
    uint64_t display_id = 0;
    geometry::RectQ8 bounds{};
    geometry::PointQ8 safe_point{};
    uint32_t confidence_q16 = 0;
    uint32_t capability_bits = 0;
    uint32_t flags = 0;
    uint32_t order = 0;
    uint16_t role = 0;
    uint16_t source_bits = 0;
    uint16_t text_size = 0;
    uint16_t reserved = 0;
};

struct DebuggerTargetSummary {
    std::array<uint32_t, debug_target_role_count> roles{};
    uint32_t target_count = 0;
    uint32_t actionable = 0;
    uint32_t disabled = 0;
    uint32_t occluded = 0;
    uint32_t secure = 0;
    uint32_t approximate = 0;
    uint32_t text_redacted = 0;
    uint32_t text_truncated = 0;
    uint32_t neural = 0;
    uint32_t accessibility = 0;
    uint32_t pixel = 0;
    uint32_t grid = 0;
    uint32_t fused = 0;
    uint32_t capability_bits = 0;
    uint32_t text_bytes = 0;
};

struct DebuggerSceneFusionView {
    SaccadeTargetPacketHeader scene{};
    DebuggerTargetSummary targets{};
    scene::FusionStats fusion{};
    std::array<DebuggerTargetSample, maximum_debug_target_samples> samples{};
    uint64_t timestamp_ns = 0;
    uint32_t sample_count = 0;
    uint32_t samples_omitted = 0;
    uint32_t fusion_input_count = 0;
    uint32_t reserved = 0;
};

struct DebuggerStorage {
    alignas(64) std::array<uint8_t, scene::target_packet_max_bytes> scene{};
    interaction::ActionPlanStorage plan{};
    interaction::ActionPlanStorage replay{};
    std::array<uint64_t, interaction::maximum_action_targets> target_ids{};
    std::array<geometry::PointQ8, interaction::maximum_action_targets> target_points{};
    std::array<uint8_t, interaction::maximum_action_payload_bytes> text{};
};

struct DebuggerPlanView {
    SaccadeSpanU8 bytes{};
    input::PlanView plan{};
};

struct DebuggerStats {
    uint64_t scenes_captured = 0;
    uint64_t scene_bytes_copied = 0;
    uint64_t dry_runs = 0;
    uint64_t replays = 0;
    uint64_t replay_mismatches = 0;
    uint64_t rejected = 0;
    uint64_t clears = 0;
    uint64_t faults_armed = 0;
    uint64_t faults_injected = 0;
};

class Debugger final {
  public:
    SaccadeResult initialize(DebuggerStorage*) noexcept;
    SaccadeResult capture_scene(const scene::PacketView&) noexcept;
    SaccadeResult capture_scene(const scene::PacketView&, const DebuggerCaptureContext&) noexcept;
    SaccadeResult dry_run(const interaction::ActionContext&, const interaction::ActionRequest&,
                          DebuggerPlanView*) noexcept;
    SaccadeResult dry_run_first_click(uint64_t now_ns, DebuggerPlanView*) noexcept;
    SaccadeResult replay(DebuggerPlanView*) noexcept;
    SaccadeResult arm_fault(DebugFaultPoint, uint32_t count, SaccadeResult result) noexcept;
    SaccadeResult consume_fault(DebugFaultPoint) noexcept;
    SaccadeResult clear() noexcept;

    [[nodiscard]] bool has_scene() const noexcept { return scene_.header != nullptr; }

    [[nodiscard]] bool has_plan() const noexcept { return plan_size_ != 0; }

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    [[nodiscard]] DebuggerStats stats() const noexcept { return stats_; }

    [[nodiscard]] DebuggerFramesTransformsView frames_transforms() const noexcept { return frames_transforms_; }

    [[nodiscard]] DebuggerSceneFusionView scene_fusion() const noexcept { return scene_fusion_; }

  private:
    SaccadeResult reject(SaccadeResult) noexcept;
    SaccadeResult copy_request(const interaction::ActionRequest&) noexcept;

    DebuggerStorage* storage_ = nullptr;
    scene::PacketView scene_{};
    interaction::ActionPlanner planner_{};
    interaction::ActionContext context_{};
    interaction::ActionRequest request_{};
    DebuggerStats stats_{};
    size_t plan_size_ = 0;
    uint64_t next_plan_id_ = 1;
    std::array<uint16_t, debug_fault_point_count> fault_remaining_{};
    std::array<SaccadeResult, debug_fault_point_count> fault_results_{};
    DebuggerFramesTransformsView frames_transforms_{};
    DebuggerSceneFusionView scene_fusion_{};
    bool initialized_ = false;
};

static_assert(sizeof(DebuggerTransformRecord) == 80);
static_assert(sizeof(DebuggerFrameRecord) == 120);
static_assert(sizeof(DebuggerTargetSample) == 80);
static_assert(sizeof(DebuggerStats) == 72);

} // namespace saccade::application

#endif

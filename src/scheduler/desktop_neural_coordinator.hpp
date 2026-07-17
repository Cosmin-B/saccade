#ifndef SACCADE_SCHEDULER_DESKTOP_NEURAL_COORDINATOR_HPP
#define SACCADE_SCHEDULER_DESKTOP_NEURAL_COORDINATOR_HPP

#include "scheduler/neural_coordinator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::scheduler {

constexpr uint32_t desktop_neural_source_capacity = 16;

struct DesktopNeuralCoordinatorStorage {
    alignas(64) std::array<uint8_t, scene::target_packet_max_bytes> inference_output{};
};

struct DesktopNeuralCoordinatorConfig {
    SaccadeRuntimeHandle runtime = 0;
    SaccadeExecutionContextHandle session = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t desktop_source_id = 0;
    uint64_t first_scene_epoch = 1;
    uint32_t maximum_output_bytes = 0;
    uint32_t maximum_targets = 0;
    uint64_t start_time_ns = 0;
    DualRateConfig rates{};
};

struct DesktopNeuralFrame : NeuralFrame {
    uint64_t scene_transform_epoch = 0;
    uint32_t source_count = 0;
    uint32_t reserved = 0;
};

struct DesktopNeuralAdvance {
    bool interaction_due = false;
    bool scene_published = false;
    bool scope_complete = false;
    uint8_t reserved = 0;
    uint32_t target_count = 0;
    uint32_t sources_expected = 0;
    uint32_t sources_completed = 0;
    uint32_t sources_failed = 0;
    uint64_t interaction_time_ns = 0;
    uint64_t scene_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t batch_latency_ns = 0;
    uint64_t full_scope_latency_ns = 0;
};

struct DesktopNeuralCoordinatorStats {
    uint64_t frames_offered = 0;
    uint64_t frames_replaced = 0;
    uint64_t frames_stale = 0;
    uint64_t batches_started = 0;
    uint64_t batches_published = 0;
    uint64_t sources_submitted = 0;
    uint64_t sources_completed = 0;
    uint64_t sources_failed = 0;
    uint64_t targets_considered = 0;
    uint64_t targets_replaced = 0;
    uint64_t targets_published = 0;
    uint64_t batch_latency_total_ns = 0;
    uint64_t batch_latency_max_ns = 0;
    uint64_t batch_deadlines_missed = 0;
    uint64_t full_scope_latency_total_ns = 0;
    uint64_t full_scope_latency_max_ns = 0;
    uint64_t full_scope_deadlines_missed = 0;
    uint64_t batches_incomplete = 0;
    uint64_t failures = 0;
};

class DesktopNeuralCoordinator final {
  public:
    DesktopNeuralCoordinator() noexcept = default;
    ~DesktopNeuralCoordinator();

    DesktopNeuralCoordinator(const DesktopNeuralCoordinator&) = delete;
    DesktopNeuralCoordinator& operator=(const DesktopNeuralCoordinator&) = delete;
    DesktopNeuralCoordinator(DesktopNeuralCoordinator&&) = delete;
    DesktopNeuralCoordinator& operator=(DesktopNeuralCoordinator&&) = delete;

    SaccadeResult initialize(const DesktopNeuralCoordinatorConfig&, DesktopNeuralCoordinatorStorage*,
                             scene::SceneStore*) noexcept;
    SaccadeResult offer(DesktopNeuralFrame) noexcept;
    SaccadeResult advance(uint64_t now_ns, DesktopNeuralAdvance*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] DesktopNeuralCoordinatorStats stats() const noexcept { return stats_; }

    [[nodiscard]] DualRateStats scheduler_stats() const noexcept { return scheduler_.stats(); }

  private:
    SaccadeResult begin_batch(DesktopNeuralAdvance*) noexcept;
    SaccadeResult start_next(DesktopNeuralAdvance*) noexcept;
    SaccadeResult retire_running(DesktopNeuralAdvance*) noexcept;
    SaccadeResult append_output(size_t) noexcept;
    SaccadeResult finish_batch(DesktopNeuralAdvance*) noexcept;
    void consider_target(SaccadeTargetRecord) noexcept;
    void release_frame(DesktopNeuralFrame*) noexcept;
    void clear_frames(std::array<DesktopNeuralFrame, desktop_neural_source_capacity>*) noexcept;

    DesktopNeuralCoordinatorConfig config_{};
    DesktopNeuralCoordinatorStorage* storage_ = nullptr;
    scene::SceneStore* scenes_ = nullptr;
    DualRateScheduler scheduler_{};
    DesktopNeuralCoordinatorStats stats_{};
    std::array<DesktopNeuralFrame, desktop_neural_source_capacity> pending_{};
    std::array<DesktopNeuralFrame, desktop_neural_source_capacity> batch_{};
    DesktopNeuralFrame running_{};
    scene::MutableScenePacket aggregate_{};
    SaccadeTargetPacketHeader aggregate_header_{};
    SaccadeTicketHandle running_ticket_ = 0;
    uint64_t next_scene_epoch_ = 1;
    uint64_t current_time_ns_ = 0;
    uint64_t batch_started_ns_ = 0;
    uint64_t batch_capture_time_ns_ = 0;
    uint32_t batch_count_ = 0;
    uint32_t batch_index_ = 0;
    uint32_t batch_expected_count_ = 0;
    uint32_t batch_completed_count_ = 0;
    uint32_t batch_failed_count_ = 0;
    uint32_t heap_size_ = 0;
    bool batch_active_ = false;
    bool initialized_ = false;
};

static_assert(sizeof(DesktopNeuralAdvance) == 80);
static_assert(sizeof(DesktopNeuralCoordinatorStats) == 152);

} // namespace saccade::scheduler

#endif

#ifndef SACCADE_SCHEDULER_NEURAL_COORDINATOR_HPP
#define SACCADE_SCHEDULER_NEURAL_COORDINATOR_HPP

#include "geometry/coordinate_transform.hpp"
#include "scene/store.hpp"
#include "scheduler/dual_rate_scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::scheduler {

struct NeuralCoordinatorStorage {
    alignas(64) std::array<uint8_t, scene::target_packet_max_bytes> inference_output{};
};

struct NeuralCoordinatorConfig {
    SaccadeRuntimeHandle runtime = 0;
    SaccadeExecutionContextHandle session = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint32_t maximum_output_bytes = 0;
    uint32_t reserved = 0;
    uint64_t start_time_ns = 0;
    DualRateConfig rates{};
};

using RetireNeuralFrameFn = void (*)(void*, SaccadeFrameHandle) noexcept;

struct NeuralFrame {
    SaccadeFrameHandle frame = 0;
    uint64_t source_id = 0;
    uint64_t topology_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t capture_time_ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    geometry::CoordinateTransform source_to_desktop{};
    void* retire_context = nullptr;
    RetireNeuralFrameFn retire = nullptr;
};

struct NeuralAdvance {
    bool interaction_due = false;
    bool scene_published = false;
    uint16_t reserved = 0;
    uint32_t target_count = 0;
    uint64_t interaction_time_ns = 0;
    uint64_t scene_epoch = 0;
};

struct NeuralCoordinatorStats {
    uint64_t frames_offered = 0;
    uint64_t frames_replaced = 0;
    uint64_t frames_submitted = 0;
    uint64_t empty_scene_deadlines = 0;
    uint64_t tickets_completed = 0;
    uint64_t tickets_cancelled = 0;
    uint64_t scenes_published = 0;
    uint64_t targets_published = 0;
    uint64_t stale_outputs = 0;
    uint64_t invalid_outputs = 0;
    uint64_t failures = 0;
};

class NeuralCoordinator final {
  public:
    NeuralCoordinator() noexcept = default;
    ~NeuralCoordinator();

    NeuralCoordinator(const NeuralCoordinator&) = delete;
    NeuralCoordinator& operator=(const NeuralCoordinator&) = delete;
    NeuralCoordinator(NeuralCoordinator&&) = delete;
    NeuralCoordinator& operator=(NeuralCoordinator&&) = delete;

    SaccadeResult initialize(const NeuralCoordinatorConfig&, NeuralCoordinatorStorage*, scene::SceneStore*) noexcept;
    SaccadeResult offer(NeuralFrame) noexcept;
    SaccadeResult advance(uint64_t now_ns, NeuralAdvance*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] NeuralCoordinatorStats stats() const noexcept { return stats_; }

    [[nodiscard]] DualRateStats scheduler_stats() const noexcept { return scheduler_.stats(); }

  private:
    SaccadeResult start_pending(uint64_t scene_time_ns, NeuralAdvance*) noexcept;
    SaccadeResult retire_running(NeuralAdvance*) noexcept;
    SaccadeResult publish_output(size_t byte_size, NeuralAdvance*) noexcept;
    void finish_scene_without_frame() noexcept;
    void release_frame(NeuralFrame*) noexcept;

    NeuralCoordinatorConfig config_{};
    NeuralCoordinatorStorage* storage_ = nullptr;
    scene::SceneStore* scenes_ = nullptr;
    DualRateScheduler scheduler_{};
    NeuralCoordinatorStats stats_{};
    NeuralFrame pending_frame_{};
    NeuralFrame running_frame_{};
    SaccadeTicketHandle running_ticket_ = 0;
    uint64_t next_scene_epoch_ = 1;
    bool initialized_ = false;
};

} // namespace saccade::scheduler

#endif

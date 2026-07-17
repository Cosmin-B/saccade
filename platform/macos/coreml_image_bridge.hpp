#ifndef SACCADE_PLATFORM_MACOS_COREML_IMAGE_BRIDGE_HPP
#define SACCADE_PLATFORM_MACOS_COREML_IMAGE_BRIDGE_HPP

#include "backends/metal/preprocessor.hpp"
#include "platform/macos/scene_capture.hpp"
#include "scheduler/neural_coordinator.hpp"

#include <array>
#include <cstdint>

namespace saccade::platform::macos {

struct CoreMlImageBridgeConfig {
    SaccadeRuntimeHandle runtime = 0;
    void* metal_device = nullptr;
    const char* metallib_path = nullptr;
    backend::metal::PathPreference path = backend::metal::PathPreference::automatic;
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t reserved = 0;
    std::array<float, 3> letterbox_rgb{};
};

struct CoreMlImageBridgeStats {
    uint64_t submissions = 0;
    uint64_t completions = 0;
    uint64_t runtime_imports = 0;
    uint64_t capture_releases = 0;
    uint64_t output_retires = 0;
    uint64_t busy_submissions = 0;
    uint64_t failures = 0;
    uint64_t cached_replays = 0;
};

class CoreMlImageBridge final {
  public:
    CoreMlImageBridge() noexcept = default;
    ~CoreMlImageBridge();

    CoreMlImageBridge(const CoreMlImageBridge&) = delete;
    CoreMlImageBridge& operator=(const CoreMlImageBridge&) = delete;
    CoreMlImageBridge(CoreMlImageBridge&&) = delete;
    CoreMlImageBridge& operator=(CoreMlImageBridge&&) = delete;

    SaccadeResult initialize(CoreMlImageBridgeConfig) noexcept;
    SaccadeResult begin(SceneCaptureSet*, const SceneCaptureFrame&, const geometry::DisplaySurface&) noexcept;
    SaccadeResult begin_scope(SceneCaptureSet*, const SceneCaptureFrame*, const geometry::DisplaySurface*,
                              uint32_t display_count, geometry::RectQ8 scope, uint64_t source_id) noexcept;
    SaccadeResult begin_cached(uint64_t capture_time_ns) noexcept;
    SaccadeResult poll(scheduler::NeuralFrame*, bool* ready) noexcept;
    SaccadeResult discard() noexcept;
    SaccadeResult shutdown() noexcept;
    SaccadeResult read_memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] bool busy() const noexcept { return preprocessing_ || output_in_use_; }

    [[nodiscard]] bool preprocessing() const noexcept { return preprocessing_; }

    [[nodiscard]] bool output_in_use() const noexcept { return output_in_use_; }

    [[nodiscard]] bool atlas_matches(geometry::RectQ8 scope, uint64_t topology_epoch) const noexcept;

    [[nodiscard]] CoreMlImageBridgeStats stats() const noexcept { return stats_; }

    static void retire_callback(void*, SaccadeFrameHandle) noexcept;

  private:
    void retire(SaccadeFrameHandle) noexcept;
    SaccadeResult release_captures() noexcept;

    CoreMlImageBridgeConfig config_{};
    backend::metal::ImagePreprocessor preprocessor_{};
    backend::metal::PreprocessSubmission submission_{};
    SceneCaptureSet* capture_set_ = nullptr;
    std::array<SceneCaptureFrame, geometry::display_capacity> capture_frames_{};
    geometry::RectQ8 scope_{};
    geometry::RectQ8 atlas_scope_{};
    CoreMlImageBridgeStats stats_{};
    uint64_t source_id_ = 0;
    uint64_t topology_epoch_ = 0;
    uint64_t frame_id_ = 0;
    uint64_t transform_epoch_ = 0;
    uint64_t capture_time_ns_ = 0;
    uint64_t atlas_topology_epoch_ = 0;
    uint64_t next_output_frame_id_ = 1;
    SaccadeFrameHandle output_frame_ = 0;
    uint32_t capture_count_ = 0;
    bool initialized_ = false;
    bool preprocessing_ = false;
    bool output_in_use_ = false;
    bool atlas_ready_ = false;
    bool replay_pending_ = false;
};

static_assert(sizeof(CoreMlImageBridgeConfig) == 56);
static_assert(sizeof(CoreMlImageBridgeStats) == 64);

} // namespace saccade::platform::macos

#endif

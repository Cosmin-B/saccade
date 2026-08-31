#ifndef SACCADE_PLATFORM_MACOS_SCENE_CAPTURE_HPP
#define SACCADE_PLATFORM_MACOS_SCENE_CAPTURE_HPP

#include "geometry/display_catalog.hpp"
#include "platform/macos/screen_capture.hpp"

#include <array>
#include <cstdint>
#include <pthread.h>

namespace saccade::platform::macos {

struct SceneCaptureFrame;
using SceneCaptureReleaseFn = SaccadeResult (*)(void*, const SceneCaptureFrame&) noexcept;

struct SceneCaptureFrame {
    SaccadeCapturedFrame frame{};
    NativeCapturedFrame native{};
    uint64_t display_id = 0;
    uint64_t topology_epoch = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
    void* release_context = nullptr;
    SceneCaptureReleaseFn release = nullptr;
};

struct SceneCaptureStats {
    uint64_t topology_epoch = 0;
    uint64_t synchronize_calls = 0;
    uint64_t streams_added = 0;
    uint64_t streams_removed = 0;
    uint64_t frames_acquired = 0;
    uint64_t frames_released = 0;
    uint64_t empty_acquires = 0;
    uint64_t failures = 0;
    uint32_t active_streams = 0;
    uint32_t leased_frames = 0;
};

class SceneCaptureSet final {
  public:
    SceneCaptureSet() noexcept = default;
    ~SceneCaptureSet();

    SceneCaptureSet(const SceneCaptureSet&) = delete;
    SceneCaptureSet& operator=(const SceneCaptureSet&) = delete;
    SceneCaptureSet(SceneCaptureSet&&) = delete;
    SceneCaptureSet& operator=(SceneCaptureSet&&) = delete;

    SaccadeResult initialize(ScreenCaptureProvider*, uint32_t max_width, uint32_t max_height) noexcept;
    SaccadeResult synchronize(const geometry::DisplaySnapshot&) noexcept;
    SaccadeResult set_running(bool) noexcept;
    SaccadeResult acquire(uint64_t display_id, SceneCaptureFrame*) noexcept;
    SaccadeResult release(const SceneCaptureFrame&) noexcept;
    SaccadeResult shutdown() noexcept;
    SaccadeResult display_at(uint32_t index, uint64_t*) const noexcept;
    SaccadeResult read_stats(SceneCaptureStats*) const noexcept;
    SaccadeResult read_memory_stats(SaccadeMemoryStats*) const noexcept;

  private:
    struct StreamSlot {
        geometry::DisplaySurface display_{};
        uint64_t display_id_ = 0;
        uint64_t source_id_ = 0;
        SaccadeCaptureStreamHandle stream_ = 0;
        uint32_t generation_ = 1;
        bool active_ = false;
        bool running_ = false;
        bool leased_ = false;
    };

    [[nodiscard]] bool owns_thread() const noexcept;
    [[nodiscard]] StreamSlot* find(uint64_t display_id) noexcept;
    [[nodiscard]] const StreamSlot* find(uint64_t display_id) const noexcept;
    SaccadeResult remove(StreamSlot&) noexcept;

    ScreenCaptureProvider* provider_ = nullptr;
    SaccadeCaptureProviderDesc backend_{};
    std::array<StreamSlot, geometry::display_capacity> streams_{};
    SceneCaptureStats stats_{};
    pthread_t owner_{};
    uint32_t max_width_ = 0;
    uint32_t max_height_ = 0;
    bool initialized_ = false;
    bool running_ = false;
};

static_assert(sizeof(SceneCaptureFrame) == 184);
static_assert(sizeof(SceneCaptureStats) == 72);

} // namespace saccade::platform::macos

#endif

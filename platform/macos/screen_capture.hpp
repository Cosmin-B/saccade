#ifndef SACCADE_PLATFORM_MACOS_SCREEN_CAPTURE_HPP
#define SACCADE_PLATFORM_MACOS_SCREEN_CAPTURE_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

enum : uint32_t { capture_source_cursor = UINT32_C(1) << 0, capture_source_audio = UINT32_C(1) << 1 };

struct ScreenCaptureStats {
    uint64_t callbacks = 0;
    uint64_t published = 0;
    uint64_t acquired = 0;
    uint64_t released = 0;
    uint64_t replaced = 0;
    uint64_t dropped_capacity = 0;
    uint64_t dropped_status = 0;
    uint64_t import_failures = 0;
    uint64_t stale_releases = 0;
    uint64_t start_failures = 0;
    uint64_t stop_failures = 0;
    uint64_t copied_bytes = 0;
    uint64_t imported_bytes = 0;
    uint64_t imported_high_water = 0;
    uint64_t last_frame_id = 0;
    uint64_t last_display_time = 0;
    uint64_t latest_callback_sequence = 0;
    uint64_t latest_status_sequence = 0;
    uint64_t did_stop_with_error = 0;
};

struct NativeCapturedFrame {
    void* pixel_buffer = nullptr;
    void* iosurface = nullptr;
    void* metal_texture = nullptr;
    uint64_t iosurface_id = 0;
    uint32_t plane_index = 0;
    uint32_t pixel_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

static_assert(sizeof(ScreenCaptureStats) == 152);
static_assert(sizeof(NativeCapturedFrame) == 48);

uint64_t screen_capture_window_source_id(uint64_t public_window_id) noexcept;

class ScreenCaptureProvider final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 256 * 1024;

    ScreenCaptureProvider() noexcept;
    ~ScreenCaptureProvider();

    ScreenCaptureProvider(const ScreenCaptureProvider&) = delete;
    ScreenCaptureProvider& operator=(const ScreenCaptureProvider&) = delete;
    ScreenCaptureProvider(ScreenCaptureProvider&&) = delete;
    ScreenCaptureProvider& operator=(ScreenCaptureProvider&&) = delete;

    SaccadeResult initialize(void* metal_device) noexcept;
    [[nodiscard]] SaccadeCaptureProviderDesc descriptor() noexcept;
    SaccadeResult read_stats(SaccadeCaptureStreamHandle, ScreenCaptureStats*) const noexcept;
    SaccadeResult read_native_frame(SaccadeCaptureStreamHandle, SaccadeFrameHandle, NativeCapturedFrame*) const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

} // namespace saccade::platform::macos

#endif

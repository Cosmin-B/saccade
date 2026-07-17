#ifndef SACCADE_PLATFORM_WINDOWS_SCREEN_CAPTURE_HPP
#define SACCADE_PLATFORM_WINDOWS_SCREEN_CAPTURE_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace saccade::platform::windows {

struct ScreenCaptureStats {
    uint64_t acquired = 0;
    uint64_t released = 0;
    uint64_t replaced = 0;
    uint64_t empty_acquires = 0;
    uint64_t stale_releases = 0;
    uint64_t start_failures = 0;
    uint64_t stop_failures = 0;
    uint64_t resize_events = 0;
    uint64_t imported_bytes = 0;
    uint64_t imported_high_water = 0;
    uint64_t last_frame_id = 0;
    uint64_t last_timestamp_ns = 0;
    uint64_t source_closed = 0;
};

struct NativeCapturedFrame {
    void* d3d11_texture = nullptr;
    uint32_t subresource = 0;
    uint32_t pixel_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t adapter_luid = 0;
};

class ScreenCaptureProvider final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 128 * 1024;

    ScreenCaptureProvider() noexcept;
    ~ScreenCaptureProvider();

    ScreenCaptureProvider(const ScreenCaptureProvider&) = delete;
    ScreenCaptureProvider& operator=(const ScreenCaptureProvider&) = delete;
    ScreenCaptureProvider(ScreenCaptureProvider&&) = delete;
    ScreenCaptureProvider& operator=(ScreenCaptureProvider&&) = delete;

    SaccadeResult initialize() noexcept;
    SaccadeResult initialize_native(uint64_t adapter_luid) noexcept;
    SaccadeResult initialize(ID3D11Device*) noexcept;
    [[nodiscard]] SaccadeCaptureProviderDesc descriptor() noexcept;
    SaccadeResult read_stats(SaccadeCaptureStreamHandle, ScreenCaptureStats*) const noexcept;
    SaccadeResult read_native_frame(SaccadeCaptureStreamHandle, SaccadeFrameHandle,
                                    NativeCapturedFrame*) const noexcept;
    SaccadeResult read_last_native_error(int32_t*) const noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] uint64_t adapter_luid() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;
    SaccadeResult initialize_impl(ID3D11Device*, uint64_t adapter_luid) noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(ScreenCaptureStats) == 104);
static_assert(sizeof(NativeCapturedFrame) == 32);

} // namespace saccade::platform::windows

#endif

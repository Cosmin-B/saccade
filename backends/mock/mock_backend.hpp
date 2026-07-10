#ifndef SACCADE_BACKENDS_MOCK_MOCK_BACKEND_HPP
#define SACCADE_BACKENDS_MOCK_MOCK_BACKEND_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::mock {

enum class FaultPoint : uint32_t {
    none = 0,
    inference_submit,
    inference_synchronize,
    capture_acquire,
    capture_synchronize,
    overlay_submit,
    accessibility_request,
    accessibility_synchronize,
    input_execute,
    input_release_all,
    input_synchronize
};

struct MemoryConfig {
    uint64_t host_committed = 4096;
    uint64_t host_reserved = 8192;
    uint64_t device_imported = 0;
    uint64_t device_owned = 0;
    uint64_t framework_opaque = 0;
    uint64_t copied_bytes = 0;
    uint64_t high_water_bytes = 8192;
};

struct Config {
    uint32_t completion_polls = 1;
    uint32_t queue_capacity = 2;
    uint32_t capability_bits =
        SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT |
        SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_CANCELLATION |
        SACCADE_PROVIDER_CAPABILITY_DAMAGE;
    uint32_t format_bits = SACCADE_FORMAT_BGRA8 | SACCADE_FORMAT_RGBA8;
    uint32_t precision_bits = SACCADE_PRECISION_FP32;
    uint32_t import_bits = SACCADE_IMPORT_HOST;
    uint32_t capture_width = 64;
    uint32_t capture_height = 48;
    uint32_t capture_pixel_format = SACCADE_FORMAT_BGRA8;
    MemoryConfig memory{};
};

struct Observations {
    uint64_t inference_submissions = 0;
    uint64_t captured_frames = 0;
    uint64_t overlay_submissions = 0;
    uint64_t accessibility_requests = 0;
    uint64_t input_executions = 0;
    uint64_t release_all_calls = 0;
    uint64_t last_scene_epoch = 0;
    uint64_t last_transform_epoch = 0;
    uint64_t last_command_hash = 0;
    uint32_t overlay_visible = 0;
};

class Backend final {
  public:
    static constexpr size_t storage_size = 16384;

    explicit Backend(const Config& = {}) noexcept;
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    [[nodiscard]] SaccadeInferenceProviderDesc inference_provider() noexcept;
    [[nodiscard]] SaccadeCaptureProviderDesc capture_provider() noexcept;
    [[nodiscard]] SaccadeOverlayProviderDesc overlay_provider() noexcept;
    [[nodiscard]] SaccadeAccessibilityProviderDesc accessibility_provider() noexcept;
    [[nodiscard]] SaccadeInputProviderDesc input_provider() noexcept;
    [[nodiscard]] SaccadeDeviceInfo device_info() const noexcept;

    void set_fault(FaultPoint, SaccadeResult, uint32_t count = 1) noexcept;
    void clear_fault() noexcept;
    [[nodiscard]] Observations observations() const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::backend::mock

#endif

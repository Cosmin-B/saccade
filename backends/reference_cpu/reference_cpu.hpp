#ifndef SACCADE_BACKENDS_REFERENCE_CPU_REFERENCE_CPU_HPP
#define SACCADE_BACKENDS_REFERENCE_CPU_REFERENCE_CPU_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::reference_cpu {

constexpr size_t model_byte_count = 12;
constexpr size_t maximum_targets = 32;
constexpr size_t serialized_header_size = sizeof(SaccadeTargetPacketHeader);
constexpr size_t serialized_target_size = sizeof(SaccadeTargetRecord);
constexpr size_t maximum_output_size = serialized_header_size + maximum_targets * serialized_target_size;

struct FrameView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_stride_bytes = 0;
    uint32_t pixel_format = 0;
    uint64_t frame_id = 0;
};

struct ModelParameters {
    uint8_t luma_threshold = 200;
    uint32_t minimum_area = 4;
};

struct Target {
    uint64_t stable_id = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t safe_x = 0;
    int32_t safe_y = 0;
    uint32_t area = 0;
    uint32_t confidence_q16 = 0;
};

struct DetectionResult {
    uint32_t target_count = 0;
    std::array<Target, maximum_targets> targets{};
};

struct DecodedOutput {
    uint64_t frame_id = 0;
    uint64_t model_epoch = 0;
    uint64_t session_epoch = 0;
    uint64_t transform_epoch = 0;
    uint64_t topology_epoch = 0;
    uint64_t source_id = 0;
    DetectionResult detections{};
};

[[nodiscard]] std::array<uint8_t, model_byte_count> encode_model(const ModelParameters&) noexcept;
SaccadeResult detect(const FrameView&, const ModelParameters&, DetectionResult*) noexcept;
SaccadeResult decode_output(SaccadeSpanU8, DecodedOutput*) noexcept;

class Backend final {
  public:
    static constexpr size_t storage_size = 32768;

    Backend() noexcept;
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    SaccadeResult register_frame(const FrameView&, SaccadeFrameHandle*) noexcept;
    SaccadeResult release_frame(SaccadeFrameHandle) noexcept;

    [[nodiscard]] SaccadeInferenceProviderDesc provider() noexcept;
    [[nodiscard]] SaccadeDeviceInfo device_info() const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::backend::reference_cpu

#endif

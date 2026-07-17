#ifndef SACCADE_BACKENDS_IMAGE_PREPROCESSOR_HPP
#define SACCADE_BACKENDS_IMAGE_PREPROCESSOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::image {

enum class TensorFormat : uint32_t { direct_texture = 0, planar_fp16 = 1, planar_int8 = 2, image_bgra8 = 3 };

struct TensorSpec {
    uint32_t width = 0;
    uint32_t height = 0;
    TensorFormat format = TensorFormat::direct_texture;
    uint32_t reserved = 0;
    std::array<float, 3> channel_scale{1.0F, 1.0F, 1.0F};
    std::array<float, 3> channel_bias{};
    std::array<float, 3> letterbox_rgb{};
};

struct SourceRegion {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PreprocessSubmission {
    uint64_t sequence = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
};

struct TensorView {
    void* buffer = nullptr;
    size_t byte_size = 0;
    size_t plane_stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    TensorFormat format = TensorFormat::direct_texture;
    uint32_t channels = 0;
};

struct DirectTextureView {
    void* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixel_format = 0;
    uint32_t reserved = 0;
};

static_assert(sizeof(TensorSpec) == 52);
static_assert(sizeof(SourceRegion) == 16);
static_assert(sizeof(PreprocessSubmission) == 24);
static_assert(sizeof(TensorView) == 40);
static_assert(sizeof(DirectTextureView) == 24);

} // namespace saccade::backend::image

#endif

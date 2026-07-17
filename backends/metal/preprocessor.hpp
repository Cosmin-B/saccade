#ifndef SACCADE_BACKENDS_METAL_PREPROCESSOR_HPP
#define SACCADE_BACKENDS_METAL_PREPROCESSOR_HPP

#include "backends/image_preprocessor.hpp"
#include "backends/metal/overlay_expander.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::metal {

using image::DirectTextureView;
using image::PreprocessSubmission;
using image::SourceRegion;
using image::TensorFormat;
using image::TensorSpec;
using image::TensorView;

struct ImageView {
    void* pixel_buffer = nullptr;
    void* texture = nullptr;
    uint64_t iosurface_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixel_format = 0;
    uint32_t reserved = 0;
    SourceRegion content{};
};

constexpr uint32_t atlas_source_capacity = 16;

enum class AtlasLoad : uint32_t { clear = 0, preserve = 1 };

struct AtlasSource {
    void* texture = nullptr;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    SourceRegion source{};
    SourceRegion destination{};
};

static_assert(sizeof(AtlasSource) == 48);

struct PreprocessorStats {
    Path path = Path::unavailable;
    TensorFormat format = TensorFormat::direct_texture;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint64_t submissions = 0;
    uint64_t busy_submissions = 0;
    uint64_t completed = 0;
    uint64_t failures = 0;
    uint64_t direct_texture_views = 0;
    uint64_t atlas_submissions = 0;
    uint64_t atlas_sources = 0;
    uint64_t output_bytes = 0;
    uint64_t command_allocator_bytes = 0;
};

class ImagePreprocessor final {
  public:
    static constexpr size_t storage_size = 4096;

    ImagePreprocessor() noexcept;
    ~ImagePreprocessor();

    ImagePreprocessor(const ImagePreprocessor&) = delete;
    ImagePreprocessor& operator=(const ImagePreprocessor&) = delete;
    ImagePreprocessor(ImagePreprocessor&&) = delete;
    ImagePreprocessor& operator=(ImagePreprocessor&&) = delete;

    SaccadeResult initialize(void* metal_device, const char* metallib_path, PathPreference, const TensorSpec&) noexcept;
    SaccadeResult direct_texture(void* texture, uint32_t width, uint32_t height, DirectTextureView*) noexcept;
    SaccadeResult submit(void* texture, uint32_t width, uint32_t height, SourceRegion, uint64_t frame_id,
                         uint64_t transform_epoch, PreprocessSubmission*) noexcept;
    SaccadeResult submit_atlas(const AtlasSource*, uint32_t source_count, SourceRegion content, AtlasLoad,
                               uint64_t frame_id, uint64_t transform_epoch, PreprocessSubmission*) noexcept;
    SaccadeResult poll(const PreprocessSubmission&, bool*) noexcept;
    SaccadeResult wait(const PreprocessSubmission&, uint64_t timeout_ns) noexcept;
    SaccadeResult tensor(const PreprocessSubmission&, TensorView*) noexcept;
    SaccadeResult image(const PreprocessSubmission&, ImageView*) noexcept;
    SaccadeResult memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] PreprocessorStats stats() const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::backend::metal

#endif

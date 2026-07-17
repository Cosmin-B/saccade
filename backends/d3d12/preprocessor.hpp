#ifndef SACCADE_BACKENDS_D3D12_PREPROCESSOR_HPP
#define SACCADE_BACKENDS_D3D12_PREPROCESSOR_HPP

#include "backends/image_preprocessor.hpp"

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;

namespace saccade::backend::d3d12 {

using image::PreprocessSubmission;
using image::SourceRegion;
using image::TensorFormat;
using image::TensorSpec;
using image::TensorView;

struct PreprocessorStats {
    uint64_t submissions = 0;
    uint64_t completions = 0;
    uint64_t busy_submissions = 0;
    uint64_t failures = 0;
    uint64_t output_bytes = 0;
};

class ImagePreprocessor final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 1024;

    ImagePreprocessor() noexcept;
    ~ImagePreprocessor();

    ImagePreprocessor(const ImagePreprocessor&) = delete;
    ImagePreprocessor& operator=(const ImagePreprocessor&) = delete;
    ImagePreprocessor(ImagePreprocessor&&) = delete;
    ImagePreprocessor& operator=(ImagePreprocessor&&) = delete;

    SaccadeResult initialize(ID3D12Device*, ID3D12CommandQueue*, const char* shader_directory,
                             const TensorSpec&) noexcept;
    SaccadeResult adopt_current_thread() noexcept;
    SaccadeResult submit(ID3D12Resource*, uint32_t source_width, uint32_t source_height, SourceRegion,
                         uint64_t frame_id, uint64_t transform_epoch, PreprocessSubmission*) noexcept;
    SaccadeResult poll(const PreprocessSubmission*) noexcept;
    SaccadeResult wait(const PreprocessSubmission*, uint64_t timeout_ns) noexcept;
    SaccadeResult tensor(const PreprocessSubmission*, TensorView*) noexcept;
    SaccadeResult tensor_storage(TensorView*) const noexcept;
    SaccadeResult completion_dependency(const PreprocessSubmission*, ID3D12Fence**, uint64_t*) noexcept;

    [[nodiscard]] PreprocessorStats stats() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(PreprocessorStats) == 40);

} // namespace saccade::backend::d3d12

#endif

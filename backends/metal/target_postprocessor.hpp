#ifndef SACCADE_BACKENDS_METAL_TARGET_POSTPROCESSOR_HPP
#define SACCADE_BACKENDS_METAL_TARGET_POSTPROCESSOR_HPP

#include "backends/metal/overlay_expander.hpp"
#include "kernels/targets/postprocess.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::metal {

struct TargetPostprocessorSpec {
    uint32_t candidate_capacity = 0;
    uint32_t target_capacity = 0;
    void* candidate_buffer = nullptr;
    size_t candidate_offset = 0;
};

struct TargetPostprocessSubmission {
    uint64_t sequence = 0;
    uint64_t frame_id = 0;
    uint32_t candidate_count = 0;
    uint32_t reserved = 0;
};

struct TargetPacketSpan {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct TargetPostprocessorStats {
    Path path = Path::unavailable;
    uint32_t candidate_capacity = 0;
    uint32_t target_capacity = 0;
    uint32_t radix_passes = 0;
    uint64_t submissions = 0;
    uint64_t busy_submissions = 0;
    uint64_t completed = 0;
    uint64_t failures = 0;
    uint64_t workspace_bytes = 0;
    uint64_t packet_readback_bytes = 0;
    uint64_t command_allocator_bytes = 0;
};

class TargetPostprocessor final {
  public:
    static constexpr size_t storage_size = 8192;

    TargetPostprocessor() noexcept;
    ~TargetPostprocessor();

    TargetPostprocessor(const TargetPostprocessor&) = delete;
    TargetPostprocessor& operator=(const TargetPostprocessor&) = delete;
    TargetPostprocessor(TargetPostprocessor&&) = delete;
    TargetPostprocessor& operator=(TargetPostprocessor&&) = delete;

    SaccadeResult initialize(void* metal_device, const char* metallib_path, PathPreference,
                             const TargetPostprocessorSpec&) noexcept;
    SaccadeResult submit(uint32_t candidate_count, const kernels::targets::PostprocessConfig&,
                         const kernels::targets::PostprocessEpochs&, TargetPostprocessSubmission*) noexcept;
    SaccadeResult poll(const TargetPostprocessSubmission&, bool*) noexcept;
    SaccadeResult wait(const TargetPostprocessSubmission&, uint64_t timeout_ns) noexcept;
    SaccadeResult packet(const TargetPostprocessSubmission&, TargetPacketSpan*) noexcept;
    SaccadeResult memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] TargetPostprocessorStats stats() const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::backend::metal

#endif

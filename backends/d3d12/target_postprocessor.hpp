#ifndef SACCADE_BACKENDS_D3D12_TARGET_POSTPROCESSOR_HPP
#define SACCADE_BACKENDS_D3D12_TARGET_POSTPROCESSOR_HPP

#include "kernels/targets/postprocess.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;

namespace saccade::backend::d3d12 {

enum class CandidateInput : uint32_t { packed_q3 = 0, normalized_fp16 = 1 };

struct TargetPostprocessorSpec {
    uint32_t candidate_capacity = 0;
    uint32_t target_capacity = 0;
    ID3D12Resource* candidate_buffer = nullptr;
    size_t candidate_offset = 0;
    CandidateInput candidate_input = CandidateInput::packed_q3;
    uint32_t model_width = 0;
    uint32_t model_height = 0;
    uint32_t reserved = 0;
};

struct TargetPostprocessSubmission {
    uint64_t sequence = 0;
    uint64_t frame_id = 0;
};

struct TargetPacketSpan {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct TargetPostprocessorStats {
    uint32_t candidate_capacity = 0;
    uint32_t target_capacity = 0;
    uint32_t radix_passes = 0;
    uint32_t reserved = 0;
    uint64_t submissions = 0;
    uint64_t busy_submissions = 0;
    uint64_t completed = 0;
    uint64_t failures = 0;
    uint64_t workspace_bytes = 0;
    uint64_t packet_readback_bytes = 0;
};

class TargetPostprocessor final {
  public:
    struct Impl;
    static constexpr size_t storage_size = 8192;

    TargetPostprocessor() noexcept;
    ~TargetPostprocessor();

    TargetPostprocessor(const TargetPostprocessor&) = delete;
    TargetPostprocessor& operator=(const TargetPostprocessor&) = delete;
    TargetPostprocessor(TargetPostprocessor&&) = delete;
    TargetPostprocessor& operator=(TargetPostprocessor&&) = delete;

    SaccadeResult initialize(ID3D12Device*, ID3D12CommandQueue*, const char*, const TargetPostprocessorSpec&) noexcept;
    SaccadeResult adopt_current_thread() noexcept;
    SaccadeResult submit(uint32_t candidate_count, const kernels::targets::PostprocessConfig&,
                         const kernels::targets::PostprocessEpochs&, TargetPostprocessSubmission*) noexcept;
    SaccadeResult submit(uint32_t candidate_count, uint32_t source_width, uint32_t source_height,
                         const kernels::targets::PostprocessConfig&, const kernels::targets::PostprocessEpochs&,
                         TargetPostprocessSubmission*) noexcept;
    SaccadeResult poll(const TargetPostprocessSubmission&, bool*) noexcept;
    SaccadeResult wait(const TargetPostprocessSubmission&, uint64_t) noexcept;
    SaccadeResult packet(const TargetPostprocessSubmission&, TargetPacketSpan*) noexcept;
    SaccadeResult memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] TargetPostprocessorStats stats() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(TargetPostprocessorSpec) == 40);
static_assert(sizeof(TargetPostprocessSubmission) == 16);
static_assert(sizeof(TargetPacketSpan) == 16);
static_assert(sizeof(TargetPostprocessorStats) == 64);

} // namespace saccade::backend::d3d12

#endif

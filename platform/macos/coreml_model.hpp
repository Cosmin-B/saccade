#ifndef SACCADE_PLATFORM_MACOS_COREML_MODEL_HPP
#define SACCADE_PLATFORM_MACOS_COREML_MODEL_HPP

#include "kernels/targets/postprocess.hpp"
#include "model/artifact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

enum class CoreMlComputePolicy : uint32_t { all = 1, cpu_and_gpu = 2, cpu_only = 3, cpu_and_neural_engine = 4 };

struct CoreMlModelConfig {
    const char* model_root = nullptr;
    CoreMlComputePolicy compute_policy = CoreMlComputePolicy::cpu_and_neural_engine;
    bool allow_low_precision_gpu = false;
    uint8_t reserved[3]{};
};

struct CoreMlPrediction {
    void* pixel_buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixel_format = 0;
    SaccadeRectI32 scope{};
    kernels::targets::PostprocessEpochs epochs{};
};

struct CoreMlPredictionResult {
    size_t byte_size = 0;
    uint32_t target_count = 0;
    uint32_t candidate_count = 0;
};

struct CoreMlModelStats {
    uint64_t model_loads = 0;
    uint64_t predictions = 0;
    uint64_t candidates_decoded = 0;
    uint64_t targets_published = 0;
    uint64_t failures = 0;
    uint64_t unsupported_inputs = 0;
    uint64_t output_contract_failures = 0;
    uint64_t reserved = 0;
};

SaccadeResult coreml_bundle_digest(const char*, std::array<uint8_t, 32>*) noexcept;

class CoreMlModel final {
  public:
    static constexpr size_t storage_size = 2 * 1024 * 1024;

    CoreMlModel() noexcept;
    ~CoreMlModel();

    CoreMlModel(const CoreMlModel&) = delete;
    CoreMlModel& operator=(const CoreMlModel&) = delete;
    CoreMlModel(CoreMlModel&&) = delete;
    CoreMlModel& operator=(CoreMlModel&&) = delete;

    SaccadeResult initialize(const model::ArtifactView&, CoreMlModelConfig) noexcept;
    SaccadeResult predict(const CoreMlPrediction&, SaccadeMutableSpanU8, CoreMlPredictionResult*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] uint64_t stable_id() const noexcept;
    [[nodiscard]] uint32_t maximum_output_bytes() const noexcept;
    [[nodiscard]] CoreMlModelStats stats() const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

static_assert(sizeof(CoreMlModelConfig) == 16);
static_assert(sizeof(CoreMlPrediction) == 88);
static_assert(sizeof(CoreMlPredictionResult) == 16);
static_assert(sizeof(CoreMlModelStats) == 64);

} // namespace saccade::platform::macos

#endif

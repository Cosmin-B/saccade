#ifndef SACCADE_APPLICATION_INFERENCE_RUNTIME_HPP
#define SACCADE_APPLICATION_INFERENCE_RUNTIME_HPP

#include <saccade/saccade_backend.h>

#include <cstdint>

namespace saccade::application {

struct InferenceRuntimeConfig {
    SaccadeInferenceProviderDesc provider{};
    SaccadeSpanU8 artifact{};
    uint64_t model_stable_id = 0;
    uint64_t provider_stable_id = 0;
    uint64_t device_stable_id = 0;
    uint32_t required_capability_bits = 0;
    uint32_t preferred_capability_bits = 0;
    uint32_t required_format_bits = 0;
    uint32_t required_precision_bits = 0;
    uint32_t required_import_bits = 0;
    uint32_t queue_capacity = 1;
    uint32_t max_in_flight = 1;
};

struct InferenceRuntimeStats {
    uint64_t initializations = 0;
    uint64_t shutdowns = 0;
    uint64_t failures = 0;
};

class InferenceRuntime final {
  public:
    InferenceRuntime() noexcept = default;
    ~InferenceRuntime();

    InferenceRuntime(const InferenceRuntime&) = delete;
    InferenceRuntime& operator=(const InferenceRuntime&) = delete;
    InferenceRuntime(InferenceRuntime&&) = delete;
    InferenceRuntime& operator=(InferenceRuntime&&) = delete;

    SaccadeResult initialize(const InferenceRuntimeConfig&) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] SaccadeRuntimeHandle runtime() const noexcept { return runtime_; }

    [[nodiscard]] SaccadeExecutionContextHandle session() const noexcept { return session_; }

    [[nodiscard]] const SaccadeInferenceSessionInfo& info() const noexcept { return info_; }

    [[nodiscard]] InferenceRuntimeStats stats() const noexcept { return stats_; }

  private:
    SaccadeRuntimeHandle runtime_ = 0;
    SaccadeExecutionContextHandle session_ = 0;
    SaccadeInferenceSessionInfo info_{};
    InferenceRuntimeStats stats_{};
};

static_assert(sizeof(InferenceRuntimeStats) == 24);

} // namespace saccade::application

#endif

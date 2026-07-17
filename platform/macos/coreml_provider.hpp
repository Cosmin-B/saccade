#ifndef SACCADE_PLATFORM_MACOS_COREML_PROVIDER_HPP
#define SACCADE_PLATFORM_MACOS_COREML_PROVIDER_HPP

#include "model/artifact.hpp"
#include "platform/macos/coreml_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

struct CoreMlProviderConfig {
    const char* model_root = nullptr;
    CoreMlComputePolicy compute_policy = CoreMlComputePolicy::cpu_and_neural_engine;
    bool allow_low_precision_gpu = false;
    uint8_t reserved[3]{};
    model::ArtifactVerifier verifier{};
};

class CoreMlInferenceProvider final {
  public:
    static constexpr size_t storage_size = 4 * 1024 * 1024;

    CoreMlInferenceProvider() noexcept;
    ~CoreMlInferenceProvider();

    CoreMlInferenceProvider(const CoreMlInferenceProvider&) = delete;
    CoreMlInferenceProvider& operator=(const CoreMlInferenceProvider&) = delete;
    CoreMlInferenceProvider(CoreMlInferenceProvider&&) = delete;
    CoreMlInferenceProvider& operator=(CoreMlInferenceProvider&&) = delete;

    SaccadeResult initialize(CoreMlProviderConfig) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] SaccadeInferenceProviderDesc descriptor() noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(128) std::array<std::byte, storage_size> storage_{};
};

static_assert(sizeof(CoreMlProviderConfig) == 32);

} // namespace saccade::platform::macos

#endif

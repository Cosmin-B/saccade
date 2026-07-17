#ifndef SACCADE_BACKENDS_D3D12_DIRECTML_PROVIDER_HPP
#define SACCADE_BACKENDS_D3D12_DIRECTML_PROVIDER_HPP

#include "model/artifact.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D12Device;

namespace saccade::backend::d3d12 {

enum class DirectMlExecutionPolicy : uint32_t { hardware_only = 0, software_only = 1, hardware_then_software = 2 };

enum class DirectMlModelStage : uint32_t {
    none = 0,
    artifact,
    candidate_buffer,
    preprocessor,
    input_tensor,
    inference,
    postprocessor,
    ready
};

struct DirectMlProviderConfig {
    const char* shader_directory = nullptr;
    model::ArtifactVerifier verifier{};
    DirectMlExecutionPolicy execution_policy = DirectMlExecutionPolicy::hardware_only;
    bool profile_stages = false;
    uint8_t reserved[3]{};
    uint64_t device_stable_id = 0;
};

struct DirectMlPipelineStats {
    uint64_t tickets = 0;
    uint64_t direct_imports = 0;
    uint64_t wrapped_imports = 0;
    uint64_t import_ns = 0;
    uint64_t preprocess_ns = 0;
    uint64_t inference_ns = 0;
    uint64_t postprocess_ns = 0;
};

class DirectMlInferenceProvider final {
  public:
    struct Impl;
    static constexpr size_t storage_size = 1024 * 1024;

    DirectMlInferenceProvider() noexcept;
    ~DirectMlInferenceProvider();

    DirectMlInferenceProvider(const DirectMlInferenceProvider&) = delete;
    DirectMlInferenceProvider& operator=(const DirectMlInferenceProvider&) = delete;
    DirectMlInferenceProvider(DirectMlInferenceProvider&&) = delete;
    DirectMlInferenceProvider& operator=(DirectMlInferenceProvider&&) = delete;

    SaccadeResult initialize(DirectMlProviderConfig) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] ID3D11Device* capture_device() const noexcept;
    [[nodiscard]] ID3D12Device* graphics_device() const noexcept;
    [[nodiscard]] uint64_t adapter_luid() const noexcept;
    [[nodiscard]] DirectMlModelStage model_stage() const noexcept;
    [[nodiscard]] DirectMlPipelineStats pipeline_stats() const noexcept;
    [[nodiscard]] SaccadeInferenceProviderDesc descriptor() noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

static_assert(sizeof(DirectMlProviderConfig) == 40);
static_assert(sizeof(DirectMlPipelineStats) == 56);

} // namespace saccade::backend::d3d12

#endif

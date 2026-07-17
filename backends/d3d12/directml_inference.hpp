#ifndef SACCADE_BACKENDS_D3D12_DIRECTML_INFERENCE_HPP
#define SACCADE_BACKENDS_D3D12_DIRECTML_INFERENCE_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;

namespace saccade::backend::d3d12 {

constexpr uint32_t directml_binding_capacity = 4;
constexpr uint32_t directml_shape_capacity = 6;

enum class TensorElementType : uint32_t { fp32 = 1, fp16 = 2, int8 = 3, uint8 = 4, int32 = 5, int64 = 6 };

struct DirectMlBindingDesc {
    const char* name = nullptr;
    ID3D12Resource* resource = nullptr;
    size_t byte_size = 0;
    std::array<int64_t, directml_shape_capacity> shape{};
    uint32_t rank = 0;
    TensorElementType element_type = TensorElementType::fp16;
};

struct DirectMlSessionDesc {
    SaccadeSpanU8 model{};
    const DirectMlBindingDesc* inputs = nullptr;
    const DirectMlBindingDesc* outputs = nullptr;
    uint32_t input_count = 0;
    uint32_t output_count = 0;
};

struct DirectMlStats {
    uint64_t runs = 0;
    uint64_t failures = 0;
    uint64_t model_bytes = 0;
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    int32_t last_native_code = 0;
    uint32_t input_count = 0;
    uint32_t output_count = 0;
};

class DirectMlInference final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 4096;

    DirectMlInference() noexcept;
    ~DirectMlInference();

    DirectMlInference(const DirectMlInference&) = delete;
    DirectMlInference& operator=(const DirectMlInference&) = delete;
    DirectMlInference(DirectMlInference&&) = delete;
    DirectMlInference& operator=(DirectMlInference&&) = delete;

    SaccadeResult initialize(ID3D12Device*, ID3D12CommandQueue*, const DirectMlSessionDesc&) noexcept;
    SaccadeResult adopt_current_thread() noexcept;
    SaccadeResult run() noexcept;
    SaccadeResult synchronize_outputs() noexcept;

    [[nodiscard]] DirectMlStats stats() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(DirectMlBindingDesc) == 80);
static_assert(sizeof(DirectMlSessionDesc) == 40);
static_assert(sizeof(DirectMlStats) == 56);

} // namespace saccade::backend::d3d12

#endif

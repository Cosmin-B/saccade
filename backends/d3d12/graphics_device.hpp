#ifndef SACCADE_BACKENDS_D3D12_GRAPHICS_DEVICE_HPP
#define SACCADE_BACKENDS_D3D12_GRAPHICS_DEVICE_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;

namespace saccade::backend::d3d12 {

struct TextureLease {
    ID3D11Texture2D* capture_texture = nullptr;
    ID3D12Resource* texture = nullptr;
};

enum class InitializationStage : uint32_t {
    none,
    factory,
    hardware_adapter,
    software_adapter,
    command_queue,
    capture_bridge
};

enum class DevicePreference : uint32_t { hardware_only = 0, software_only = 1, hardware_then_software = 2 };

struct InitializationError {
    InitializationStage stage = InitializationStage::none;
    int32_t native_code = 0;
};

class GraphicsDevice final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 256;

    GraphicsDevice() noexcept;
    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;
    GraphicsDevice(GraphicsDevice&&) = delete;
    GraphicsDevice& operator=(GraphicsDevice&&) = delete;

    SaccadeResult initialize(DevicePreference = DevicePreference::hardware_only,
                             uint64_t requested_adapter_luid = 0) noexcept;
    SaccadeResult shutdown() noexcept;
    SaccadeResult adopt_current_thread() noexcept;
    [[nodiscard]] ID3D12Device* device() const noexcept;
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept;
    [[nodiscard]] ID3D11Device* capture_device() const noexcept;
    [[nodiscard]] uint64_t adapter_luid() const noexcept;
    [[nodiscard]] InitializationError initialization_error() const noexcept;
    [[nodiscard]] bool software_device() const noexcept;

    SaccadeResult unwrap(ID3D11Texture2D*, TextureLease*) noexcept;
    SaccadeResult return_texture(TextureLease*, ID3D12Fence* completion_fence, uint64_t completion_value) noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(TextureLease) == 16);
static_assert(sizeof(InitializationError) == 8);

} // namespace saccade::backend::d3d12

#endif

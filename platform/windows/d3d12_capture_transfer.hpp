#ifndef SACCADE_PLATFORM_WINDOWS_D3D12_CAPTURE_TRANSFER_HPP
#define SACCADE_PLATFORM_WINDOWS_D3D12_CAPTURE_TRANSFER_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;

namespace saccade::platform::windows {

enum class D3d12CaptureTransferStage : uint32_t { none, create_texture, share_texture, open_texture, signal_copy };

struct D3d12CaptureTransferFrame {
    ID3D12Resource* texture = nullptr;
    ID3D12Fence* ready_fence = nullptr;
    uint64_t ready_value = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
};

struct D3d12CaptureTransferStats {
    uint64_t copies = 0;
    uint64_t releases = 0;
    uint64_t copied_bytes = 0;
    uint64_t committed_bytes = 0;
    uint64_t high_water_bytes = 0;
    uint64_t failures = 0;
    int32_t native_error = 0;
    D3d12CaptureTransferStage error_stage = D3d12CaptureTransferStage::none;
};

class D3d12CaptureTransfer final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 4096;

    D3d12CaptureTransfer() noexcept;
    ~D3d12CaptureTransfer();

    D3d12CaptureTransfer(const D3d12CaptureTransfer&) = delete;
    D3d12CaptureTransfer& operator=(const D3d12CaptureTransfer&) = delete;
    D3d12CaptureTransfer(D3d12CaptureTransfer&&) = delete;
    D3d12CaptureTransfer& operator=(D3d12CaptureTransfer&&) = delete;

    SaccadeResult initialize(ID3D11Device* producer_device, ID3D11DeviceContext* producer_context,
                             ID3D12Device* consumer_device) noexcept;
    SaccadeResult copy(ID3D11Texture2D*, uint32_t width, uint32_t height, D3d12CaptureTransferFrame*) noexcept;
    SaccadeResult release(D3d12CaptureTransferFrame) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] D3d12CaptureTransferStats stats() const noexcept;

  private:
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

static_assert(sizeof(D3d12CaptureTransferFrame) == 32);
static_assert(sizeof(D3d12CaptureTransferStats) == 56);

} // namespace saccade::platform::windows

#endif

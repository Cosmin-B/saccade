#ifndef SACCADE_PLATFORM_WINDOWS_NEURAL_BRIDGE_HPP
#define SACCADE_PLATFORM_WINDOWS_NEURAL_BRIDGE_HPP

#include "platform/windows/d3d12_capture_transfer.hpp"
#include "platform/windows/scene_capture.hpp"
#include "scheduler/desktop_neural_coordinator.hpp"

#include <array>
#include <cstdint>

namespace saccade::platform::windows {

struct NeuralBridgeStats {
    uint64_t imports = 0;
    uint64_t capture_releases = 0;
    uint64_t failures = 0;
    uint64_t transfer_copies = 0;
    uint64_t transfer_releases = 0;
    uint64_t copied_bytes = 0;
    uint64_t reserved[2]{};
};

struct NeuralBridgeConfig {
    SaccadeRuntimeHandle runtime = 0;
    ID3D11Device* producer_device = nullptr;
    ID3D11DeviceContext* producer_context = nullptr;
    ID3D12Device* consumer_device = nullptr;
};

class NeuralBridge final {
  public:
    NeuralBridge() noexcept = default;
    ~NeuralBridge();

    SaccadeResult initialize(SaccadeRuntimeHandle) noexcept;
    SaccadeResult initialize(const NeuralBridgeConfig&) noexcept;
    SaccadeResult import(SceneCaptureSet*, const SceneCaptureFrame&, const geometry::DisplaySurface&,
                         uint64_t scene_transform_epoch, scheduler::DesktopNeuralFrame*) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] NeuralBridgeStats stats() const noexcept { return stats_; }

    [[nodiscard]] D3d12CaptureTransferStats transfer_stats() const noexcept { return transfer_.stats(); }

  private:
    struct RetirementSlot {
        NeuralBridge* owner = nullptr;
        SceneCaptureSet* captures = nullptr;
        SceneCaptureFrame capture{};
        D3d12CaptureTransferFrame transfer{};
        SaccadeFrameHandle frame = 0;
    };

    static void retire_callback(void*, SaccadeFrameHandle) noexcept;
    void retire(RetirementSlot*, SaccadeFrameHandle) noexcept;

    SaccadeRuntimeHandle runtime_ = 0;
    NeuralBridgeStats stats_{};
    std::array<RetirementSlot, geometry::display_capacity> retirements_{};
    D3d12CaptureTransfer transfer_{};
    bool transfer_initialized_ = false;
};

static_assert(sizeof(NeuralBridgeStats) == 64);
static_assert(sizeof(NeuralBridgeConfig) == 32);

} // namespace saccade::platform::windows

#endif

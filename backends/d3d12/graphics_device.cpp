#include "backends/d3d12/graphics_device.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <new>

namespace saccade::backend::d3d12 {

using Microsoft::WRL::ComPtr;

namespace {

uint64_t luid_value(const LUID& luid) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32U) | luid.LowPart;
}

} // namespace

struct GraphicsDevice::Impl {
    DWORD owner_thread_ = 0;
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<ID3D11Device> capture_device_{};
    ComPtr<ID3D11DeviceContext> capture_context_{};
    ComPtr<ID3D11On12Device2> bridge_{};
    ID3D11Texture2D* outstanding_capture_texture_ = nullptr;
    uint64_t adapter_luid_ = 0;
    InitializationError initialization_error_{};
    bool software_device_ = false;

    [[nodiscard]] bool owns_thread() const noexcept { return owner_thread_ == GetCurrentThreadId(); }
};

static_assert(sizeof(GraphicsDevice::Impl) <= GraphicsDevice::storage_size);
static_assert(alignof(GraphicsDevice::Impl) <= 64);

GraphicsDevice::GraphicsDevice() noexcept {
    new (storage_.data()) Impl{};
}

GraphicsDevice::~GraphicsDevice() {
    Impl& state = impl();
    if (initialized_ && state.owns_thread()) {
        if (state.outstanding_capture_texture_ != nullptr) {
            (void)state.bridge_->ReturnUnderlyingResource(state.outstanding_capture_texture_, 0, nullptr, nullptr);
            state.outstanding_capture_texture_ = nullptr;
        }
        state.capture_context_->Flush();
    }
    state.~Impl();
}

GraphicsDevice::Impl& GraphicsDevice::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const GraphicsDevice::Impl& GraphicsDevice::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult GraphicsDevice::initialize(DevicePreference preference, uint64_t requested_adapter_luid) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (preference < DevicePreference::hardware_only || preference > DevicePreference::hardware_then_software ||
        (requested_adapter_luid != 0 && preference == DevicePreference::software_only))
        return SACCADE_ERROR_INVALID_ARGUMENT;

    Impl& state = impl();
    state.owner_thread_ = GetCurrentThreadId();
    ComPtr<IDXGIFactory6> factory;
    HRESULT native_result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(native_result)) {
        state.initialization_error_ = {InitializationStage::factory, native_result};
        return SACCADE_ERROR_BACKEND;
    }
    if (preference != DevicePreference::software_only) {
        for (UINT adapter_index = 0;; ++adapter_index) {
            ComPtr<IDXGIAdapter1> adapter;
            native_result = factory->EnumAdapterByGpuPreference(adapter_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(adapter.GetAddressOf()));
            if (native_result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(native_result)) continue;
            DXGI_ADAPTER_DESC1 adapter_desc{};
            if (FAILED(adapter->GetDesc1(&adapter_desc)) || (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (requested_adapter_luid != 0 && luid_value(adapter_desc.AdapterLuid) != requested_adapter_luid) continue;
            native_result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                              IID_PPV_ARGS(state.device_.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(native_result)) {
                state.adapter_luid_ = luid_value(adapter_desc.AdapterLuid);
                break;
            }
        }
    }
    if (state.device_ == nullptr && preference != DevicePreference::hardware_only && requested_adapter_luid == 0) {
        ComPtr<IDXGIAdapter> adapter;
        native_result = factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf()));
        if (SUCCEEDED(native_result)) {
            native_result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                              IID_PPV_ARGS(state.device_.ReleaseAndGetAddressOf()));
            DXGI_ADAPTER_DESC adapter_desc{};
            if (SUCCEEDED(native_result) && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
                state.adapter_luid_ = luid_value(adapter_desc.AdapterLuid);
            }
        }
        state.software_device_ = state.device_ != nullptr;
    }
    if (state.device_ == nullptr) {
        state.initialization_error_ = {preference == DevicePreference::hardware_only
                                           ? InitializationStage::hardware_adapter
                                           : InitializationStage::software_adapter,
                                       native_result};
        return SACCADE_ERROR_UNSUPPORTED;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    native_result = state.device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(state.queue_.GetAddressOf()));
    if (FAILED(native_result)) {
        state.initialization_error_ = {InitializationStage::command_queue, native_result};
        return SACCADE_ERROR_BACKEND;
    }
    IUnknown* queues[]{state.queue_.Get()};
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    native_result = D3D11On12CreateDevice(
        state.device_.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(), static_cast<UINT>(levels.size()), queues,
        1, 0, state.capture_device_.GetAddressOf(), state.capture_context_.GetAddressOf(), nullptr);
    if (SUCCEEDED(native_result)) {
        native_result = state.capture_device_.As(&state.bridge_);
    }
    if (FAILED(native_result)) {
        state.initialization_error_ = {InitializationStage::capture_bridge, native_result};
        return SACCADE_ERROR_BACKEND;
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult GraphicsDevice::adopt_current_thread() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    if (state.outstanding_capture_texture_ != nullptr) return SACCADE_ERROR_BUSY;
    state.owner_thread_ = GetCurrentThreadId();
    return SACCADE_OK;
}

SaccadeResult GraphicsDevice::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (state.outstanding_capture_texture_ != nullptr) return SACCADE_ERROR_BUSY;
    state.capture_context_->Flush();
    state.~Impl();
    new (storage_.data()) Impl{};
    initialized_ = false;
    return SACCADE_OK;
}

ID3D12Device* GraphicsDevice::device() const noexcept {
    return initialized_ ? impl().device_.Get() : nullptr;
}

ID3D12CommandQueue* GraphicsDevice::queue() const noexcept {
    return initialized_ ? impl().queue_.Get() : nullptr;
}

ID3D11Device* GraphicsDevice::capture_device() const noexcept {
    return initialized_ ? impl().capture_device_.Get() : nullptr;
}

uint64_t GraphicsDevice::adapter_luid() const noexcept {
    return initialized_ ? impl().adapter_luid_ : 0;
}

InitializationError GraphicsDevice::initialization_error() const noexcept {
    return impl().initialization_error_;
}

bool GraphicsDevice::software_device() const noexcept {
    return initialized_ && impl().software_device_;
}

SaccadeResult GraphicsDevice::unwrap(ID3D11Texture2D* capture_texture, TextureLease* output) noexcept {
    if (!initialized_ || capture_texture == nullptr || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (state.outstanding_capture_texture_ != nullptr) return SACCADE_ERROR_BUSY;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(state.bridge_->UnwrapUnderlyingResource(capture_texture, state.queue_.Get(),
                                                       IID_PPV_ARGS(texture.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    state.outstanding_capture_texture_ = capture_texture;
    output->capture_texture = capture_texture;
    output->texture = texture.Detach();
    return SACCADE_OK;
}

SaccadeResult GraphicsDevice::return_texture(TextureLease* lease, ID3D12Fence* completion_fence,
                                             uint64_t completion_value) noexcept {
    if (!initialized_ || lease == nullptr || lease->capture_texture == nullptr || lease->texture == nullptr ||
        (completion_fence == nullptr) != (completion_value == 0)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    if (state.outstanding_capture_texture_ != lease->capture_texture) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const UINT fence_count = completion_fence == nullptr ? 0U : 1U;
    ID3D12Fence* fences[]{completion_fence};
    const HRESULT returned = state.bridge_->ReturnUnderlyingResource(
        lease->capture_texture, fence_count, completion_fence == nullptr ? nullptr : &completion_value,
        completion_fence == nullptr ? nullptr : fences);
    lease->texture->Release();
    *lease = {};
    if (FAILED(returned)) return SACCADE_ERROR_BACKEND;
    state.outstanding_capture_texture_ = nullptr;
    return SACCADE_OK;
}

} // namespace saccade::backend::d3d12

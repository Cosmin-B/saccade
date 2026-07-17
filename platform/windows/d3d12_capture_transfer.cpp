#include "platform/windows/d3d12_capture_transfer.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <new>

namespace saccade::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t transfer_slot_capacity = 16;

uint32_t next_generation(uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1U : value;
}

} // namespace

struct D3d12CaptureTransfer::Impl {
    struct Slot {
        ComPtr<ID3D11Texture2D> producer_{};
        ComPtr<ID3D12Resource> consumer_{};
        uint64_t byte_size_ = 0;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
        uint32_t generation_ = 1;
        bool leased_ = false;
    };

    SaccadeResult create_slot(Slot& slot, const D3D11_TEXTURE2D_DESC& source) noexcept {
        slot.producer_.Reset();
        slot.consumer_.Reset();
        slot.width_ = 0;
        slot.height_ = 0;
        slot.format_ = DXGI_FORMAT_UNKNOWN;
        slot.byte_size_ = 0;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC shared{};
        shared.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shared.Width = source.Width;
        shared.Height = source.Height;
        shared.DepthOrArraySize = 1;
        shared.MipLevels = 1;
        shared.Format = source.Format;
        shared.SampleDesc.Count = 1;
        shared.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        shared.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        HRESULT result = consumer_device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &shared,
                                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                   IID_PPV_ARGS(slot.consumer_.GetAddressOf()));
        if (FAILED(result)) {
            stats_.native_error = result;
            stats_.error_stage = D3d12CaptureTransferStage::create_texture;
            return SACCADE_ERROR_BACKEND;
        }

        HANDLE shared_handle = nullptr;
        result =
            consumer_device_->CreateSharedHandle(slot.consumer_.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle);
        if (FAILED(result)) {
            stats_.native_error = result;
            stats_.error_stage = D3d12CaptureTransferStage::share_texture;
            slot.consumer_.Reset();
            return SACCADE_ERROR_BACKEND;
        }
        result = producer_device_->OpenSharedResource1(shared_handle, IID_PPV_ARGS(slot.producer_.GetAddressOf()));
        (void)CloseHandle(shared_handle);
        if (FAILED(result)) {
            stats_.native_error = result;
            stats_.error_stage = D3d12CaptureTransferStage::open_texture;
            slot.producer_.Reset();
            slot.consumer_.Reset();
            return SACCADE_ERROR_BACKEND;
        }

        slot.width_ = source.Width;
        slot.height_ = source.Height;
        slot.format_ = source.Format;
        slot.byte_size_ = static_cast<uint64_t>(source.Width) * source.Height * 4U;
        slot.generation_ = next_generation(slot.generation_);
        stats_.committed_bytes += slot.byte_size_;
        stats_.high_water_bytes = std::max(stats_.high_water_bytes, stats_.committed_bytes);
        return SACCADE_OK;
    }

    DWORD owner_thread_ = 0;
    ComPtr<ID3D11Device5> producer_device_{};
    ComPtr<ID3D11DeviceContext> producer_context_{};
    ComPtr<ID3D11DeviceContext4> producer_context4_{};
    ComPtr<ID3D12Device> consumer_device_{};
    ComPtr<ID3D11Fence> producer_fence_{};
    ComPtr<ID3D12Fence> consumer_fence_{};
    std::array<Slot, transfer_slot_capacity> slots_{};
    D3d12CaptureTransferStats stats_{};
    uint64_t next_fence_value_ = 1;
    bool initialized_ = false;
};

static_assert(sizeof(D3d12CaptureTransfer::Impl) <= D3d12CaptureTransfer::storage_size);
static_assert(alignof(D3d12CaptureTransfer::Impl) <= 64);

D3d12CaptureTransfer::D3d12CaptureTransfer() noexcept {
    new (storage_.data()) Impl{};
}

D3d12CaptureTransfer::~D3d12CaptureTransfer() {
    (void)shutdown();
    impl().~Impl();
}

D3d12CaptureTransfer::Impl& D3d12CaptureTransfer::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const D3d12CaptureTransfer::Impl& D3d12CaptureTransfer::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult D3d12CaptureTransfer::initialize(ID3D11Device* producer_device, ID3D11DeviceContext* producer_context,
                                               ID3D12Device* consumer_device) noexcept {
    Impl& state = impl();
    if (state.initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (producer_device == nullptr || producer_context == nullptr || consumer_device == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (FAILED(producer_device->QueryInterface(IID_PPV_ARGS(state.producer_device_.GetAddressOf()))) ||
        FAILED(producer_context->QueryInterface(IID_PPV_ARGS(state.producer_context4_.GetAddressOf())))) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    state.producer_context_ = producer_context;
    state.consumer_device_ = consumer_device;
    HRESULT result = state.producer_device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                         IID_PPV_ARGS(state.producer_fence_.GetAddressOf()));
    HANDLE fence_handle = nullptr;
    if (SUCCEEDED(result)) {
        result = state.producer_fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
    }
    if (SUCCEEDED(result)) {
        result =
            state.consumer_device_->OpenSharedHandle(fence_handle, IID_PPV_ARGS(state.consumer_fence_.GetAddressOf()));
    }
    if (fence_handle != nullptr) (void)CloseHandle(fence_handle);
    if (FAILED(result)) return SACCADE_ERROR_BACKEND;
    state.owner_thread_ = GetCurrentThreadId();
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult D3d12CaptureTransfer::copy(ID3D11Texture2D* source, uint32_t width, uint32_t height,
                                         D3d12CaptureTransferFrame* output) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || state.owner_thread_ != GetCurrentThreadId()) return SACCADE_ERROR_STATE;
    if (source == nullptr || output == nullptr || width == 0 || height == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    if (source_desc.Width < width || source_desc.Height < height || source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        source_desc.SampleDesc.Count != 1) {
        return SACCADE_ERROR_UNSUPPORTED;
    }

    Impl::Slot* slot = nullptr;
    for (Impl::Slot& candidate : state.slots_) {
        if (!candidate.leased_ && candidate.width_ == source_desc.Width && candidate.height_ == source_desc.Height &&
            candidate.format_ == source_desc.Format) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        for (Impl::Slot& candidate : state.slots_) {
            if (!candidate.leased_) {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) return SACCADE_ERROR_CAPACITY;
    if (slot->width_ != source_desc.Width || slot->height_ != source_desc.Height ||
        slot->format_ != source_desc.Format) {
        state.stats_.committed_bytes -= slot->byte_size_;
        const SaccadeResult created = state.create_slot(*slot, source_desc);
        if (created != SACCADE_OK) {
            ++state.stats_.failures;
            return created;
        }
    }

    const uint64_t ready_value = state.next_fence_value_++;
    state.producer_context_->CopyResource(slot->producer_.Get(), source);
    HRESULT sync_result = state.producer_context4_->Signal(state.producer_fence_.Get(), ready_value);
    if (FAILED(sync_result)) {
        state.stats_.native_error = sync_result;
        state.stats_.error_stage = D3d12CaptureTransferStage::signal_copy;
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    state.producer_context_->Flush();

    slot->leased_ = true;
    slot->generation_ = next_generation(slot->generation_);
    output->texture = slot->consumer_.Get();
    output->ready_fence = state.consumer_fence_.Get();
    output->ready_value = ready_value;
    output->slot = static_cast<uint32_t>(slot - state.slots_.data());
    output->generation = slot->generation_;
    ++state.stats_.copies;
    state.stats_.copied_bytes += slot->byte_size_;
    return SACCADE_OK;
}

SaccadeResult D3d12CaptureTransfer::release(D3d12CaptureTransferFrame frame) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || state.owner_thread_ != GetCurrentThreadId()) return SACCADE_ERROR_STATE;
    if (frame.texture == nullptr || frame.slot >= state.slots_.size()) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl::Slot& slot = state.slots_[frame.slot];
    if (!slot.leased_ || slot.generation_ != frame.generation || slot.consumer_.Get() != frame.texture) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    slot.leased_ = false;
    ++state.stats_.releases;
    return SACCADE_OK;
}

SaccadeResult D3d12CaptureTransfer::shutdown() noexcept {
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_OK;
    if (state.owner_thread_ != GetCurrentThreadId()) return SACCADE_ERROR_STATE;
    for (const Impl::Slot& slot : state.slots_) {
        if (slot.leased_) return SACCADE_ERROR_BUSY;
    }
    state.producer_context_->Flush();
    state.~Impl();
    new (storage_.data()) Impl{};
    return SACCADE_OK;
}

D3d12CaptureTransferStats D3d12CaptureTransfer::stats() const noexcept {
    return impl().stats_;
}

} // namespace saccade::platform::windows

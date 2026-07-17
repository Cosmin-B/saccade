#include "platform/windows/overlay_surface.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace saccade::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t swapchain_buffer_count = 3;
constexpr wchar_t overlay_class_name[] = L"SaccadeOverlaySurface";

LRESULT CALLBACK overlay_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_NCHITTEST) {
        const auto* click_through = reinterpret_cast<const bool*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return click_through == nullptr || *click_through ? HTTRANSPARENT : HTCLIENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_overlay_class() noexcept {
    WNDCLASSEXW desc{};
    desc.cbSize = sizeof(desc);
    desc.lpfnWndProc = overlay_window_proc;
    desc.hInstance = GetModuleHandleW(nullptr);
    desc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    desc.lpszClassName = overlay_class_name;
    if (RegisterClassExW(&desc) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool display_valid(const geometry::DisplaySurface& display) noexcept {
    return display.display_id != 0 && display.backing_width != 0 && display.backing_height != 0 &&
           display.desktop_bounds.width > 0 && display.desktop_bounds.height > 0;
}

int whole_q8(int32_t value) noexcept {
    return value / geometry::coordinate_one;
}

} // namespace

struct OverlaySurface::Impl {
    backend::d3d12::OverlayRenderer renderer_{};
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<IDXGISwapChain3> swapchain_{};
    ComPtr<IDCompositionDevice> composition_{};
    ComPtr<IDCompositionTarget> composition_target_{};
    ComPtr<IDCompositionVisual> visual_{};
    ComPtr<ID3D12DescriptorHeap> views_{};
    std::array<ComPtr<ID3D12Resource>, swapchain_buffer_count> textures_{};
    geometry::DisplaySurface display_{};
    OverlaySurfaceCallbacks callbacks_{};
    OverlaySurfaceStats stats_{};
    HWND window_ = nullptr;
    HANDLE frame_latency_ = nullptr;
    DWORD owner_thread_ = 0;
    backend::d3d12::OverlaySubmission last_submission_{};
    UINT view_stride_ = 0;
    bool visible_ = false;
    bool click_through_ = true;
    bool color_managed_ = false;

    bool owns_thread() const noexcept { return GetCurrentThreadId() == owner_thread_; }

    HRESULT current_target(backend::d3d12::OverlayRenderTarget* output) noexcept {
        if (output == nullptr || views_ == nullptr) return E_INVALIDARG;
        const uint32_t index = swapchain_->GetCurrentBackBufferIndex();
        if (index >= swapchain_buffer_count || textures_[index] == nullptr) {
            return E_FAIL;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE view = views_->GetCPUDescriptorHandleForHeapStart();
        view.ptr += static_cast<SIZE_T>(view_stride_) * index;
        *output = {textures_[index].Get(), view, 0, display_.backing_width, display_.backing_height};
        return S_OK;
    }

    HRESULT warm_views() noexcept {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = swapchain_buffer_count;
        HRESULT result = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(views_.GetAddressOf()));
        if (FAILED(result)) return result;
        view_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE view = views_->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t index = 0; index < swapchain_buffer_count; ++index) {
            result = swapchain_->GetBuffer(index, IID_PPV_ARGS(textures_[index].GetAddressOf()));
            if (FAILED(result)) return result;
            device_->CreateRenderTargetView(textures_[index].Get(), nullptr, view);
            view.ptr += view_stride_;
        }
        return S_OK;
    }

    void release_views() noexcept {
        for (auto& texture : textures_) {
            texture.Reset();
        }
        views_.Reset();
        view_stride_ = 0;
    }

    void release_frame_latency() noexcept {
        if (frame_latency_ != nullptr) {
            (void)CloseHandle(frame_latency_);
            frame_latency_ = nullptr;
        }
    }

    SaccadeResult position_window() noexcept {
        const int x = whole_q8(display_.desktop_bounds.x);
        const int y = whole_q8(display_.desktop_bounds.y);
        const int width = whole_q8(display_.desktop_bounds.width);
        const int height = whole_q8(display_.desktop_bounds.height);
        return SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                            SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_NOREDRAW)) != FALSE
                   ? SACCADE_OK
                   : SACCADE_ERROR_BACKEND;
    }
};

OverlaySurface::OverlaySurface() noexcept = default;

OverlaySurface::~OverlaySurface() {
    if (!initialized_) {
        return;
    }
    Impl& state = impl();
    if (state.owns_thread()) {
        if (state.visible_) {
            (void)ShowWindow(state.window_, SW_HIDE);
        }
        if (state.last_submission_.sequence != 0) {
            (void)state.renderer_.wait(state.last_submission_, UINT64_C(1'000'000'000));
        }
        state.visual_.Reset();
        state.composition_target_.Reset();
        if (state.composition_) {
            (void)state.composition_->Commit();
        }
        state.release_views();
        state.release_frame_latency();
        state.swapchain_.Reset();
        state.composition_.Reset();
        if (state.window_ != nullptr) {
            (void)DestroyWindow(state.window_);
        }
    }
    state.~Impl();
}

OverlaySurface::Impl& OverlaySurface::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlaySurface::Impl& OverlaySurface::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlaySurface::initialize(const geometry::DisplaySurface& display, ID3D12Device* device,
                                         ID3D12CommandQueue* queue, const char* shader_directory,
                                         OverlaySurfaceCallbacks callbacks) noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    if (initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (!display_valid(display) || device == nullptr || queue == nullptr || shader_directory == nullptr ||
        callbacks.load_frame == nullptr || !register_overlay_class()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl* state = new (storage_.data()) Impl{};
    state->owner_thread_ = GetCurrentThreadId();
    state->display_ = display;
    state->callbacks_ = callbacks;
    state->device_ = device;
    state->queue_ = queue;
    state->window_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
        overlay_class_name, L"", WS_POPUP, whole_q8(display.desktop_bounds.x), whole_q8(display.desktop_bounds.y),
        whole_q8(display.desktop_bounds.width), whole_q8(display.desktop_bounds.height), nullptr, nullptr,
        GetModuleHandleW(nullptr), &state->click_through_);
    if (state->window_ == nullptr) {
        last_native_error_ = static_cast<int32_t>(HRESULT_FROM_WIN32(GetLastError()));
        last_native_stage_ = OverlaySurfaceNativeStage::window;
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    if (SetWindowDisplayAffinity(state->window_, WDA_EXCLUDEFROMCAPTURE) == FALSE) {
        last_native_error_ = static_cast<int32_t>(HRESULT_FROM_WIN32(GetLastError()));
        last_native_stage_ = OverlaySurfaceNativeStage::capture_exclusion;
        (void)DestroyWindow(state->window_);
        state->window_ = nullptr;
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    MARGINS margins{-1, -1, -1, -1};
    (void)DwmExtendFrameIntoClientArea(state->window_, &margins);
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swapchain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = display.backing_width;
    desc.Height = display.backing_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = swapchain_buffer_count;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    auto fail = [&](OverlaySurfaceNativeStage stage, HRESULT error) noexcept -> SaccadeResult {
        last_native_stage_ = stage;
        last_native_error_ = static_cast<int32_t>(error);
        const HWND window = state->window_;
        state->window_ = nullptr;
        state->release_frame_latency();
        state->~Impl();
        if (window != nullptr) {
            (void)DestroyWindow(window);
        }
        return SACCADE_ERROR_BACKEND;
    };
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        return fail(OverlaySurfaceNativeStage::dxgi_device, result);
    }
    result = factory->CreateSwapChainForComposition(queue, &desc, nullptr, swapchain.GetAddressOf());
    if (FAILED(result) || FAILED(result = swapchain.As(&state->swapchain_))) {
        return fail(OverlaySurfaceNativeStage::swapchain, result);
    }
    UINT color_space_support = 0;
    result = state->swapchain_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &color_space_support);
    if (FAILED(result) || (color_space_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0 ||
        FAILED(result = state->swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709))) {
        return fail(OverlaySurfaceNativeStage::color_space, FAILED(result) ? result : E_NOTIMPL);
    }
    state->color_managed_ = true;
    result = state->swapchain_->SetMaximumFrameLatency(1);
    state->frame_latency_ = SUCCEEDED(result) ? state->swapchain_->GetFrameLatencyWaitableObject() : nullptr;
    if (FAILED(result) || state->frame_latency_ == nullptr) {
        return fail(OverlaySurfaceNativeStage::frame_latency, FAILED(result) ? result : E_FAIL);
    }
    result = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                      reinterpret_cast<void**>(state->composition_.GetAddressOf()));
    if (FAILED(result)) {
        return fail(OverlaySurfaceNativeStage::composition_device, result);
    }
    result = state->composition_->CreateTargetForHwnd(state->window_, TRUE, state->composition_target_.GetAddressOf());
    if (FAILED(result)) {
        return fail(OverlaySurfaceNativeStage::composition_target, result);
    }
    result = state->composition_->CreateVisual(state->visual_.GetAddressOf());
    if (FAILED(result) || FAILED(result = state->visual_->SetContent(state->swapchain_.Get())) ||
        FAILED(result = state->composition_target_->SetRoot(state->visual_.Get())) ||
        FAILED(result = state->composition_->Commit())) {
        return fail(OverlaySurfaceNativeStage::composition_visual, result);
    }
    result = state->warm_views();
    if (FAILED(result)) {
        return fail(OverlaySurfaceNativeStage::render_views, result);
    }
    if (state->renderer_.initialize(device, queue, shader_directory) != SACCADE_OK) {
        return fail(OverlaySurfaceNativeStage::renderer, E_FAIL);
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    if (!initialized_ || !impl().owns_thread()) return SACCADE_ERROR_STATE;
    return impl().renderer_.set_glyph_atlas(atlas);
}

SaccadeResult OverlaySurface::update_display(const geometry::DisplaySurface& display) noexcept {
    if (!initialized_ || !display_valid(display) || display.display_id != impl().display_.display_id) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    if (!state.owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    const bool resize = display.backing_width != state.display_.backing_width ||
                        display.backing_height != state.display_.backing_height;
    state.display_ = display;
    if (resize) {
        if (state.last_submission_.sequence != 0) {
            const SaccadeResult waited = state.renderer_.wait(state.last_submission_, UINT64_C(1'000'000'000));
            if (waited != SACCADE_OK) return waited;
            state.last_submission_ = {};
        }
        state.release_views();
        const HRESULT result = state.swapchain_->ResizeBuffers(swapchain_buffer_count, display.backing_width,
                                                               display.backing_height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                               DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        const HRESULT views = SUCCEEDED(result) ? state.warm_views() : result;
        if (FAILED(views)) {
            return SACCADE_ERROR_BACKEND;
        }
    }
    return state.position_window();
}

SaccadeResult OverlaySurface::start() noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (state.visible_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    state.visible_ = true;
    if (state.position_window() != SACCADE_OK) {
        state.visible_ = false;
        return SACCADE_ERROR_BACKEND;
    }
    (void)ShowWindow(state.window_, SW_SHOWNOACTIVATE);
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::stop() noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.visible_) {
        return SACCADE_ERROR_STATE;
    }
    (void)ShowWindow(state.window_, SW_HIDE);
    state.visible_ = false;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::present(uint64_t now_ns) noexcept {
    if (!initialized_ || !impl().owns_thread() || now_ns == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    ++state.stats_.presentation_attempts;
    const DWORD pacing = WaitForSingleObjectEx(state.frame_latency_, 0, FALSE);
    if (pacing == WAIT_TIMEOUT) {
        ++state.stats_.pacing_not_ready;
        return SACCADE_ERROR_BUSY;
    }
    if (pacing != WAIT_OBJECT_0) {
        ++state.stats_.pacing_failures;
        ++state.stats_.failures;
        last_native_stage_ = OverlaySurfaceNativeStage::frame_latency;
        last_native_error_ = static_cast<int32_t>(HRESULT_FROM_WIN32(GetLastError()));
        return SACCADE_ERROR_BACKEND;
    }
    SaccadeOverlayFrameDesc frame{};
    const SaccadeResult loaded =
        state.callbacks_.load_frame(state.callbacks_.context, state.display_.display_id, &frame);
    if (loaded != SACCADE_OK) {
        state.stats_.no_frame_ticks += loaded == SACCADE_ERROR_NOT_FOUND ? 1U : 0U;
        state.stats_.failures += loaded != SACCADE_ERROR_NOT_FOUND ? 1U : 0U;
        if (state.callbacks_.observe_frame != nullptr) {
            state.callbacks_.observe_frame(state.callbacks_.context, state.display_.display_id, loaded, nullptr);
        }
        return loaded;
    }
    backend::d3d12::OverlayRenderTarget target{};
    const HRESULT target_result = state.current_target(&target);
    if (FAILED(target_result)) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    target.timestamp_ns = now_ns;
    backend::d3d12::OverlaySubmission submission{};
    const SaccadeResult rendered = state.renderer_.render(frame, target, &submission);
    SaccadeResult result = rendered;
    if (rendered != SACCADE_OK) {
        state.stats_.busy_frames += rendered == SACCADE_ERROR_BUSY ? 1U : 0U;
        state.stats_.failures += rendered != SACCADE_ERROR_BUSY ? 1U : 0U;
    } else {
        state.last_submission_ = submission;
        ++state.stats_.rendered_frames;
        const HRESULT presented = state.swapchain_->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
        if (presented == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++state.stats_.busy_frames;
            result = SACCADE_ERROR_BUSY;
        } else if (FAILED(presented)) {
            ++state.stats_.failures;
            result = SACCADE_ERROR_BACKEND;
        } else {
            ++state.stats_.presented_frames;
            state.stats_.last_scene_epoch = frame.scene_epoch;
            state.stats_.last_transform_epoch = frame.transform_epoch;
        }
    }
    if (state.callbacks_.observe_frame != nullptr) {
        state.callbacks_.observe_frame(state.callbacks_.context, state.display_.display_id, result,
                                       rendered == SACCADE_OK ? &submission : nullptr);
    }
    return result;
}

SaccadeResult OverlaySurface::set_click_through(bool enabled) noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    LONG_PTR style = GetWindowLongPtrW(state.window_, GWL_EXSTYLE);
    style = enabled ? style | WS_EX_TRANSPARENT : style & ~WS_EX_TRANSPARENT;
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(state.window_, GWL_EXSTYLE, style) == 0 && GetLastError() != ERROR_SUCCESS) {
        return SACCADE_ERROR_BACKEND;
    }
    state.click_through_ = enabled;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_info(OverlaySurfaceInfo* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl& state = impl();
    output->display_id = state.display_.display_id;
    output->window_handle = reinterpret_cast<uintptr_t>(state.window_);
    output->frame_latency_handle = reinterpret_cast<uintptr_t>(state.frame_latency_);
    output->drawable_width = state.display_.backing_width;
    output->drawable_height = state.display_.backing_height;
    output->buffer_count = swapchain_buffer_count;
    output->flags = overlay_surface_initialized | (state.visible_ ? overlay_surface_visible : 0U) |
                    (state.click_through_ ? overlay_surface_click_through : 0U) | overlay_surface_nonactivating |
                    overlay_surface_topmost | overlay_surface_excluded_from_capture |
                    (state.color_managed_ ? overlay_surface_color_managed : 0U) | overlay_surface_display_paced;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_stats(OverlaySurfaceStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_renderer_stats(backend::d3d12::OverlayStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().renderer_.stats();
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_memory_stats(OverlaySurfaceMemoryStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    output->renderer.struct_size = sizeof(output->renderer);
    output->renderer.api_version = SACCADE_API_VERSION;
    const SaccadeResult result = impl().renderer_.memory_stats(&output->renderer);
    if (result != SACCADE_OK) {
        return result;
    }
    output->swapchain_bytes_estimate = static_cast<uint64_t>(impl().display_.backing_width) *
                                       impl().display_.backing_height * 4U * swapchain_buffer_count;
    output->surface_host_bytes = sizeof(Impl);
    output->total_known_and_estimated =
        output->renderer.device_owned + output->swapchain_bytes_estimate + output->surface_host_bytes;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_last_native_error(int32_t* error, OverlaySurfaceNativeStage* stage) const noexcept {
    if (error == nullptr || stage == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *error = last_native_error_;
    *stage = last_native_stage_;
    return SACCADE_OK;
}

struct OverlaySurfaceSet::Impl {
    struct Slot {
        OverlaySurface surface_{};
        uint64_t display_id_ = 0;
        bool active_ = false;
    };

    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    std::array<char, shader_directory_capacity + 1U> shader_directory_{};
    OverlaySurfaceCallbacks callbacks_{};
    std::array<Slot, geometry::display_capacity> slots_{};
    overlay::GlyphAtlasStorage glyph_atlas_{};
    OverlaySurfaceSetStats stats_{};
    DWORD owner_thread_ = 0;
    bool click_through_ = true;
    bool has_glyph_atlas_ = false;

    bool owns_thread() const noexcept { return GetCurrentThreadId() == owner_thread_; }

    Slot* find(uint64_t display_id) noexcept {
        for (Slot& slot : slots_) {
            if (slot.active_ && slot.display_id_ == display_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    const Slot* find(uint64_t display_id) const noexcept { return const_cast<Impl*>(this)->find(display_id); }

    SaccadeResult remove(Slot& slot) noexcept {
        if (stats_.running != 0) {
            const SaccadeResult stopped = slot.surface_.stop();
            if (stopped != SACCADE_OK) {
                return stopped;
            }
        }
        slot.surface_.~OverlaySurface();
        new (&slot.surface_) OverlaySurface{};
        slot.display_id_ = 0;
        slot.active_ = false;
        --stats_.active_surfaces;
        ++stats_.surfaces_removed;
        return SACCADE_OK;
    }
};

OverlaySurfaceSet::OverlaySurfaceSet() noexcept = default;

OverlaySurfaceSet::~OverlaySurfaceSet() {
    if (initialized_) {
        impl().~Impl();
        initialized_ = false;
    }
}

OverlaySurfaceSet::Impl& OverlaySurfaceSet::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlaySurfaceSet::Impl& OverlaySurfaceSet::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlaySurfaceSet::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                            const char* shader_directory, OverlaySurfaceCallbacks callbacks) noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    if (initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (device == nullptr || queue == nullptr || shader_directory == nullptr || callbacks.load_frame == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t length = std::strlen(shader_directory);
    if (length == 0 || length > shader_directory_capacity) {
        return SACCADE_ERROR_CAPACITY;
    }
    Impl* state = new (storage_.data()) Impl{};
    state->device_ = device;
    state->queue_ = queue;
    std::memcpy(state->shader_directory_.data(), shader_directory, length + 1U);
    state->callbacks_ = callbacks;
    state->owner_thread_ = GetCurrentThreadId();
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::shutdown() noexcept {
    if (!initialized_ || !impl().owns_thread()) return SACCADE_ERROR_STATE;
    if (impl().stats_.running != 0) {
        const SaccadeResult stopped = stop();
        if (stopped != SACCADE_OK) return stopped;
    }
    impl().~Impl();
    initialized_ = false;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    if (!initialized_ || !impl().owns_thread()) return SACCADE_ERROR_STATE;
    if (!overlay::glyph_atlas_valid(atlas)) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();

    std::array<bool, geometry::display_capacity> updated{};
    for (size_t index = 0; index < state.slots_.size(); ++index) {
        Impl::Slot& slot = state.slots_[index];
        if (!slot.active_) continue;
        const SaccadeResult result = slot.surface_.set_glyph_atlas(atlas);
        if (result != SACCADE_OK) {
            if (state.has_glyph_atlas_) {
                for (size_t rollback = 0; rollback < index; ++rollback) {
                    if (updated[rollback])
                        (void)state.slots_[rollback].surface_.set_glyph_atlas(state.glyph_atlas_.view());
                }
            }
            return result;
        }
        updated[index] = true;
    }
    std::memcpy(state.glyph_atlas_.pixels.data(), atlas.pixels, overlay::glyph_atlas_bytes);
    std::memcpy(state.glyph_atlas_.symbols.data(), atlas.symbols,
                static_cast<size_t>(atlas.glyph_count) * sizeof(uint16_t));
    state.glyph_atlas_.glyph_count = atlas.glyph_count;
    state.has_glyph_atlas_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::synchronize(const geometry::DisplaySnapshot& snapshot) noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    ++state.stats_.synchronize_attempts;
    if (snapshot.epoch == 0 || snapshot.count == 0 || snapshot.count > geometry::display_capacity ||
        snapshot.epoch < state.stats_.topology_epoch) {
        ++state.stats_.failures;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (snapshot.epoch == state.stats_.topology_epoch) {
        return SACCADE_OK;
    }
    for (Impl::Slot& slot : state.slots_) {
        if (!slot.active_) {
            continue;
        }
        bool retained = false;
        for (uint32_t index = 0; index < snapshot.count; ++index) {
            retained |= snapshot.displays[index].display_id == slot.display_id_;
        }
        if (!retained) {
            const SaccadeResult result = state.remove(slot);
            if (result != SACCADE_OK) {
                ++state.stats_.failures;
                return result;
            }
        }
    }
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        Impl::Slot* slot = state.find(display.display_id);
        if (slot != nullptr) {
            const SaccadeResult updated = slot->surface_.update_display(display);
            if (updated != SACCADE_OK) {
                ++state.stats_.failures;
                return updated;
            }
            ++state.stats_.surfaces_updated;
            continue;
        }
        for (Impl::Slot& candidate : state.slots_) {
            if (!candidate.active_) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            ++state.stats_.failures;
            return SACCADE_ERROR_CAPACITY;
        }
        SaccadeResult result = slot->surface_.initialize(display, state.device_.Get(), state.queue_.Get(),
                                                         state.shader_directory_.data(), state.callbacks_);
        if (result == SACCADE_OK && state.has_glyph_atlas_) {
            result = slot->surface_.set_glyph_atlas(state.glyph_atlas_.view());
        }
        if (result == SACCADE_OK) {
            result = slot->surface_.set_click_through(state.click_through_);
        }
        if (result == SACCADE_OK && state.stats_.running != 0) {
            result = slot->surface_.start();
        }
        if (result != SACCADE_OK) {
            slot->surface_.~OverlaySurface();
            new (&slot->surface_) OverlaySurface{};
            ++state.stats_.failures;
            return result;
        }
        slot->display_id_ = display.display_id;
        slot->active_ = true;
        ++state.stats_.active_surfaces;
        ++state.stats_.surfaces_added;
    }
    state.stats_.topology_epoch = snapshot.epoch;
    ++state.stats_.topology_changes;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::start() noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (state.stats_.running != 0) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_ && slot.surface_.start() != SACCADE_OK) {
            ++state.stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
    }
    state.stats_.running = 1;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::stop() noexcept {
    if (!initialized_ || !impl().owns_thread() || impl().stats_.running == 0) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_ && slot.surface_.stop() != SACCADE_OK) {
            ++state.stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
    }
    state.stats_.running = 0;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::present(uint64_t display_id, uint64_t now_ns) noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface_.present(now_ns);
}

SaccadeResult OverlaySurfaceSet::set_click_through(bool enabled) noexcept {
    if (!initialized_ || !impl().owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_ && slot.surface_.set_click_through(enabled) != SACCADE_OK) {
            return SACCADE_ERROR_BACKEND;
        }
    }
    state.click_through_ = enabled;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::read_stats(OverlaySurfaceSetStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::read_surface_info(uint64_t display_id, OverlaySurfaceInfo* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface_.read_info(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_stats(uint64_t display_id, OverlaySurfaceStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface_.read_stats(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_memory_stats(uint64_t display_id,
                                                           OverlaySurfaceMemoryStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) return SACCADE_ERROR_INVALID_ARGUMENT;
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface_.read_memory_stats(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_renderer_stats(uint64_t display_id,
                                                             backend::d3d12::OverlayStats* output) const noexcept {
    if (!initialized_ || output == nullptr || !impl().owns_thread()) return SACCADE_ERROR_INVALID_ARGUMENT;
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface_.read_renderer_stats(output);
}

} // namespace saccade::platform::windows

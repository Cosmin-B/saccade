#include "platform/windows/screen_capture.hpp"

#include "platform/windows/display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace saccade::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace directx = winrt::Windows::Graphics::DirectX;

constexpr uint32_t maximum_sources = 256;
constexpr uint32_t maximum_streams = 16;
constexpr uint32_t maximum_damage_rects = 64;
constexpr uint64_t display_source_prefix = UINT64_C(1) << 56U;
constexpr uint64_t window_source_prefix = UINT64_C(2) << 56U;
constexpr uint64_t source_value_mask = UINT64_C(0x00FFFFFFFFFFFFFF);
constexpr char provider_name[] = "Windows Graphics Capture";

void preserve_first_error(int32_t error, int32_t* first_error) noexcept {
    if (*first_error == 0 && error != 0) {
        *first_error = error;
    }
}

template <typename Operation> int32_t invoke_cleanup(Operation&& operation) noexcept {
    try {
        operation();
        return 0;
    } catch (const winrt::hresult_error& error) {
        return error.code().value;
    } catch (...) {
        return static_cast<int32_t>(E_FAIL);
    }
}

template <typename Object> void close_capture_object(Object& object, int32_t* first_error) noexcept {
    if (!object) {
        return;
    }

    preserve_first_error(invoke_cleanup([&object] { object.Close(); }), first_error);
    object = nullptr;
}

template <typename T> SaccadeResult read_input(const T* input, T* output) noexcept {
    if (input == nullptr || output == nullptr || input->struct_size < sizeof(uint32_t) * 2U ||
        input->struct_size > sizeof(T) || input->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    std::memcpy(output, input, input->struct_size);
    return SACCADE_OK;
}

template <typename T> SaccadeResult write_output(T* output, const T& value) noexcept {
    if (output == nullptr || output->struct_size < sizeof(uint32_t) * 2U || output->struct_size > sizeof(T) ||
        output->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t size = output->struct_size;
    T result = value;
    result.struct_size = size;
    result.api_version = SACCADE_API_VERSION;
    std::memcpy(output, &result, size);
    return SACCADE_OK;
}

uint64_t source_id(uint64_t display_id) noexcept {
    return display_source_prefix | (display_id & source_value_mask);
}

uint64_t window_source_id(HWND window) noexcept {
    return window_source_prefix | (reinterpret_cast<uintptr_t>(window) & source_value_mask);
}

uint32_t next_generation(uint32_t generation) noexcept {
    ++generation;
    return generation == 0 ? 1U : generation;
}

uint64_t stream_handle(uint32_t slot, uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32U) | (slot + 1U);
}

bool decode_handle(uint64_t handle, uint32_t* slot, uint32_t* generation) noexcept {
    const uint32_t low = static_cast<uint32_t>(handle);
    if (low == 0 || low > maximum_streams) {
        return false;
    }
    *slot = low - 1U;
    *generation = static_cast<uint32_t>(handle >> 32U);
    return *generation != 0;
}

uint64_t frame_handle(uint32_t stream_slot, uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32U) | (stream_slot + 1U);
}

uint64_t luid_value(const LUID& luid) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32U) | luid.LowPart;
}

} // namespace

struct ScreenCaptureProvider::Impl {
    struct Source {
        uint64_t stable_id_ = 0;
        void* native_ = nullptr;
        SaccadeRectI32 bounds_{};
        uint32_t kind_ = 0;
        std::array<char, 128> name_{};
    };

    struct Stream {
        capture::GraphicsCaptureItem item_{nullptr};
        capture::Direct3D11CaptureFramePool pool_{nullptr};
        capture::GraphicsCaptureSession session_{nullptr};
        capture::Direct3D11CaptureFrame leased_frame_{nullptr};
        ComPtr<ID3D11Texture2D> leased_texture_{};
        std::array<SaccadeRectI32, maximum_damage_rects> damage_{};
        ScreenCaptureStats stats_{};
        winrt::event_token closed_token_{};
        uint64_t source_id_ = 0;
        uint64_t next_frame_id_ = 1;
        uint64_t transform_epoch_ = 1;
        uint64_t adapter_luid_ = 0;
        uint64_t imported_bytes_ = 0;
        uint32_t generation_ = 1;
        uint32_t frame_generation_ = 1;
        uint32_t queue_capacity_ = 0;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t damage_count_ = 0;
        uint32_t slot_ = 0;
        bool active_ = false;
        bool started_ = false;
        bool leased_ = false;
        bool recreate_pending_ = false;
        bool closed_registered_ = false;
        bool closed_reported_ = false;
    };

    struct ClosureSignal {
        std::atomic<uint32_t> generation_{0};
        std::atomic<bool> closed_{false};
    };

    DWORD owner_thread_ = 0;
    bool ro_initialized_ = false;
    uint64_t adapter_luid_ = 0;
    int32_t last_native_error_ = 0;
    ComPtr<ID3D11Device> d3d_device_{};
    ComPtr<ID3D11DeviceContext> d3d_context_{};
    direct3d::IDirect3DDevice winrt_device_{nullptr};
    std::array<Source, maximum_sources> sources_{};
    uint32_t source_count_ = 0;
    std::array<Stream, maximum_streams> streams_{};
    std::array<ClosureSignal, maximum_streams> closure_signals_{};

    bool owns_thread() const noexcept { return GetCurrentThreadId() == owner_thread_; }

    static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
        auto* self = reinterpret_cast<Impl*>(parameter);
        if (self->source_count_ == maximum_sources) {
            return FALSE;
        }
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) == FALSE) {
            return FALSE;
        }
        Source& source = self->sources_[self->source_count_++];
        source.stable_id_ = source_id(stable_display_id(info.szDevice));
        source.native_ = monitor;
        source.kind_ = SACCADE_CAPTURE_SOURCE_DISPLAY;
        source.bounds_ = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                          info.rcMonitor.bottom - info.rcMonitor.top};
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info.szDevice, -1, source.name_.data(),
                                               static_cast<int>(source.name_.size()), nullptr, nullptr);
        if (length <= 0) {
            source.name_[0] = '\0';
        }
        return TRUE;
    }

    static BOOL CALLBACK collect_window(HWND window, LPARAM parameter) {
        auto* self = reinterpret_cast<Impl*>(parameter);
        if (self->source_count_ == maximum_sources) {
            return FALSE;
        }
        if (IsWindowVisible(window) == FALSE || GetAncestor(window, GA_ROOT) != window) {
            return TRUE;
        }
        DWORD process_id = 0;
        (void)GetWindowThreadProcessId(window, &process_id);
        if (process_id == GetCurrentProcessId()) {
            return TRUE;
        }
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != FALSE) {
            return TRUE;
        }
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
            GetWindowRect(window, &bounds) == FALSE) {
            return TRUE;
        }
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return TRUE;
        }
        std::array<wchar_t, 256> title{};
        const int title_length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        if (title_length <= 0) {
            return TRUE;
        }
        Source& source = self->sources_[self->source_count_++];
        source.stable_id_ = window_source_id(window);
        source.native_ = window;
        source.kind_ = SACCADE_CAPTURE_SOURCE_WINDOW;
        source.bounds_ = {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
        const int length =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, title.data(), title_length, source.name_.data(),
                                static_cast<int>(source.name_.size() - 1U), nullptr, nullptr);
        source.name_[length > 0 ? static_cast<size_t>(length) : 0] = '\0';
        return TRUE;
    }

    SaccadeResult refresh_sources() noexcept {
        source_count_ = 0;
        sources_ = {};
        if (EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(this)) == FALSE ||
            source_count_ == 0) {
            return source_count_ == maximum_sources ? SACCADE_ERROR_CAPACITY : SACCADE_ERROR_BACKEND;
        }
        const BOOL windows = EnumWindows(collect_window, reinterpret_cast<LPARAM>(this));
        if (windows == FALSE && source_count_ == maximum_sources) {
            return SACCADE_ERROR_CAPACITY;
        }
        return SACCADE_OK;
    }

    Source* find_source(uint64_t id) noexcept {
        for (uint32_t index = 0; index < source_count_; ++index) {
            if (sources_[index].stable_id_ == id) {
                return &sources_[index];
            }
        }
        return nullptr;
    }

    Stream* find_stream(uint64_t handle) noexcept {
        uint32_t slot = 0;
        uint32_t generation = 0;
        if (!decode_handle(handle, &slot, &generation)) {
            return nullptr;
        }
        Stream& stream = streams_[slot];
        return stream.active_ && stream.generation_ == generation ? &stream : nullptr;
    }

    const Stream* find_stream(uint64_t handle) const noexcept { return const_cast<Impl*>(this)->find_stream(handle); }

    uint32_t stream_slot(const Stream& stream) const noexcept {
        return static_cast<uint32_t>(&stream - streams_.data());
    }

    void close_lease(Stream& stream, int32_t* first_error) noexcept {
        stream.leased_texture_.Reset();
        close_capture_object(stream.leased_frame_, first_error);
        stream.leased_ = false;
        stream.imported_bytes_ = 0;
        stream.stats_.imported_bytes = 0;
    }

    SaccadeResult close_stream(Stream& stream, int32_t first_error = 0) noexcept {
        ClosureSignal& signal = closure_signals_[stream.slot_];
        signal.generation_.store(0, std::memory_order_release);

        if (stream.closed_registered_ && stream.item_) {
            preserve_first_error(invoke_cleanup([&stream] { stream.item_.Closed(stream.closed_token_); }),
                                 &first_error);
        }
        stream.closed_registered_ = false;

        close_lease(stream, &first_error);
        close_capture_object(stream.session_, &first_error);
        close_capture_object(stream.pool_, &first_error);
        stream.item_ = nullptr;

        const uint32_t generation = next_generation(stream.generation_);
        stream.~Stream();
        new (&stream) Stream{};
        stream.generation_ = generation;

        if (first_error != 0) {
            last_native_error_ = first_error;
            return SACCADE_ERROR_BACKEND;
        }
        return SACCADE_OK;
    }
};

static_assert(sizeof(ScreenCaptureProvider::Impl) <= ScreenCaptureProvider::storage_size);

namespace {

using Impl = ScreenCaptureProvider::Impl;

Impl* provider(void* context) noexcept {
    return static_cast<Impl*>(context);
}

SaccadeResult SACCADE_CALL enumerate_sources(void* context, uint32_t index, SaccadeCaptureSourceInfo* output) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread() || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index == 0 || state->source_count_ == 0) {
        const SaccadeResult result = state->refresh_sources();
        if (result != SACCADE_OK) {
            return result;
        }
    }
    if (index >= state->source_count_) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    const Impl::Source& source = state->sources_[index];
    SaccadeCaptureSourceInfo value{};
    value.stable_id = source.stable_id_;
    value.kind = source.kind_;
    value.capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                            SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_DAMAGE;
    value.desktop_bounds = source.bounds_;
    value.name = {reinterpret_cast<const uint8_t*>(source.name_.data()), std::strlen(source.name_.data())};
    return write_output(output, value);
}

SaccadeResult SACCADE_CALL create_stream(void* context, const SaccadeCaptureStreamDesc* input,
                                         SaccadeCaptureStreamHandle* output) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread() || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    SaccadeCaptureStreamDesc desc{};
    const SaccadeResult read = read_input(input, &desc);
    if (read != SACCADE_OK) {
        return read;
    }
    if (desc.source_id == 0 || desc.pixel_format != SACCADE_FORMAT_BGRA8 || desc.queue_capacity < 2 ||
        desc.queue_capacity > 3 || desc.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (desc.max_width != 0 || desc.max_height != 0) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    if (state->source_count_ == 0) {
        const SaccadeResult refreshed = state->refresh_sources();
        if (refreshed != SACCADE_OK) {
            return refreshed;
        }
    }
    Impl::Source* source = state->find_source(desc.source_id);
    if (source == nullptr) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    uint32_t slot = maximum_streams;
    for (uint32_t index = 0; index < maximum_streams; ++index) {
        if (!state->streams_[index].active_) {
            slot = index;
            break;
        }
    }
    if (slot == maximum_streams) {
        return SACCADE_ERROR_CAPACITY;
    }
    Impl::Stream* stream = nullptr;
    try {
        auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        capture::GraphicsCaptureItem item{nullptr};
        if (source->kind_ == SACCADE_CAPTURE_SOURCE_DISPLAY) {
            winrt::check_hresult(factory->CreateForMonitor(static_cast<HMONITOR>(source->native_),
                                                           winrt::guid_of<capture::GraphicsCaptureItem>(),
                                                           winrt::put_abi(item)));
        } else {
            winrt::check_hresult(factory->CreateForWindow(static_cast<HWND>(source->native_),
                                                          winrt::guid_of<capture::GraphicsCaptureItem>(),
                                                          winrt::put_abi(item)));
        }
        const winrt::Windows::Graphics::SizeInt32 size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            return SACCADE_ERROR_BACKEND;
        }
        stream = &state->streams_[slot];
        stream->item_ = item;
        stream->slot_ = slot;
        stream->source_id_ = desc.source_id;
        stream->queue_capacity_ = desc.queue_capacity;
        stream->adapter_luid_ = state->adapter_luid_;
        stream->width_ = static_cast<uint32_t>(size.Width);
        stream->height_ = static_cast<uint32_t>(size.Height);
        Impl::ClosureSignal& signal = state->closure_signals_[slot];
        signal.closed_.store(false, std::memory_order_relaxed);
        signal.generation_.store(stream->generation_, std::memory_order_release);
        const uint32_t generation = stream->generation_;
        stream->closed_token_ = stream->item_.Closed([state, slot, generation](const auto&, const auto&) noexcept {
            Impl::ClosureSignal& callback_signal = state->closure_signals_[slot];
            if (callback_signal.generation_.load(std::memory_order_acquire) == generation)
                callback_signal.closed_.store(true, std::memory_order_release);
        });
        stream->closed_registered_ = true;
        stream->active_ = true;
        *output = stream_handle(slot, stream->generation_);
        return SACCADE_OK;
    } catch (const winrt::hresult_error& error) {
        const int32_t native_error = error.code().value;
        if (stream != nullptr) {
            (void)state->close_stream(*stream, native_error);
        } else {
            state->last_native_error_ = native_error;
        }
        return SACCADE_ERROR_BACKEND;
    } catch (...) {
        constexpr int32_t native_error = static_cast<int32_t>(E_FAIL);
        if (stream != nullptr) {
            (void)state->close_stream(*stream, native_error);
        } else {
            state->last_native_error_ = native_error;
        }
        return SACCADE_ERROR_BACKEND;
    }
}

SaccadeResult SACCADE_CALL destroy_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (stream->started_ || stream->leased_) {
        return SACCADE_ERROR_BUSY;
    }
    return state->close_stream(*stream);
}

SaccadeResult SACCADE_CALL start_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (stream->started_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    try {
        const winrt::Windows::Graphics::SizeInt32 size{static_cast<int32_t>(stream->width_),
                                                       static_cast<int32_t>(stream->height_)};
        stream->pool_ = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            state->winrt_device_, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            static_cast<int32_t>(stream->queue_capacity_), size);
        stream->session_ = stream->pool_.CreateCaptureSession(stream->item_);
        stream->session_.IsCursorCaptureEnabled(false);
        stream->session_.DirtyRegionMode(capture::GraphicsCaptureDirtyRegionMode::ReportOnly);
        stream->session_.StartCapture();
        stream->started_ = true;
        return SACCADE_OK;
    } catch (const winrt::hresult_error& error) {
        int32_t native_error = error.code().value;
        ++stream->stats_.start_failures;
        close_capture_object(stream->session_, &native_error);
        close_capture_object(stream->pool_, &native_error);
        state->last_native_error_ = native_error;
        return SACCADE_ERROR_BACKEND;
    } catch (...) {
        int32_t native_error = static_cast<int32_t>(E_FAIL);
        ++stream->stats_.start_failures;
        close_capture_object(stream->session_, &native_error);
        close_capture_object(stream->pool_, &native_error);
        state->last_native_error_ = native_error;
        return SACCADE_ERROR_BACKEND;
    }
}

SaccadeResult SACCADE_CALL stop_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!stream->started_ || stream->leased_) {
        return SACCADE_ERROR_STATE;
    }
    int32_t native_error = 0;
    close_capture_object(stream->session_, &native_error);
    close_capture_object(stream->pool_, &native_error);
    stream->started_ = false;

    if (native_error != 0) {
        state->last_native_error_ = native_error;
        ++stream->stats_.stop_failures;
        return SACCADE_ERROR_BACKEND;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL acquire_frame(void* context, SaccadeCaptureStreamHandle handle, uint64_t timeout_ns,
                                         SaccadeCapturedFrame* output) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread() || output == nullptr || timeout_ns != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!stream->started_ || stream->leased_) {
        return SACCADE_ERROR_STATE;
    }
    Impl::ClosureSignal& signal = state->closure_signals_[stream->slot_];
    if (signal.generation_.load(std::memory_order_acquire) == stream->generation_ &&
        signal.closed_.load(std::memory_order_acquire)) {
        if (!stream->closed_reported_) {
            stream->closed_reported_ = true;
            stream->stats_.source_closed = 1;
        }
        return SACCADE_ERROR_BACKEND;
    }
    try {
        capture::Direct3D11CaptureFrame frame = stream->pool_.TryGetNextFrame();
        if (!frame) {
            ++stream->stats_.empty_acquires;
            return SACCADE_ERROR_BUSY;
        }
        while (true) {
            capture::Direct3D11CaptureFrame newer = stream->pool_.TryGetNextFrame();
            if (!newer) {
                break;
            }
            frame.Close();
            frame = newer;
            ++stream->stats_.replaced;
        }
        const auto access =
            frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(
            access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.GetAddressOf())));
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture->GetDesc(&texture_desc);
        const winrt::Windows::Graphics::SizeInt32 content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) {
            frame.Close();
            return SACCADE_ERROR_BACKEND;
        }
        const uint32_t content_width = static_cast<uint32_t>(content.Width);
        const uint32_t content_height = static_cast<uint32_t>(content.Height);
        if (content_width != stream->width_ || content_height != stream->height_) {
            stream->width_ = content_width;
            stream->height_ = content_height;
            ++stream->transform_epoch_;
            ++stream->stats_.resize_events;
            stream->recreate_pending_ = true;
        }
        stream->damage_count_ = 0;
        for (const auto& rect : frame.DirtyRegions()) {
            if (stream->damage_count_ == maximum_damage_rects) {
                break;
            }
            stream->damage_[stream->damage_count_++] = {rect.X, rect.Y, rect.Width, rect.Height};
        }
        stream->leased_frame_ = frame;
        stream->leased_texture_ = texture;
        stream->leased_ = true;
        stream->imported_bytes_ = static_cast<uint64_t>(texture_desc.Width) * texture_desc.Height * 4U;
        stream->stats_.imported_bytes = stream->imported_bytes_;
        stream->stats_.imported_high_water = std::max(stream->stats_.imported_high_water, stream->imported_bytes_);
        const uint64_t id = stream->next_frame_id_++;
        const int64_t timestamp_100ns = frame.SystemRelativeTime().count();
        const uint64_t timestamp_ns = timestamp_100ns > 0 ? static_cast<uint64_t>(timestamp_100ns) * 100U : 0;
        SaccadeCapturedFrame value{};
        stream->frame_generation_ = next_generation(stream->frame_generation_);
        value.frame = frame_handle(state->stream_slot(*stream), stream->frame_generation_);
        value.source_id = stream->source_id_;
        value.frame_id = id;
        value.transform_epoch = stream->transform_epoch_;
        value.timestamp_ns = timestamp_ns;
        value.width = std::min(content_width, texture_desc.Width);
        value.height = std::min(content_height, texture_desc.Height);
        value.pixel_format = SACCADE_FORMAT_BGRA8;
        value.damage_count = stream->damage_count_;
        const SaccadeResult written = write_output(output, value);
        if (written != SACCADE_OK) {
            int32_t native_error = 0;
            state->close_lease(*stream, &native_error);
            if (native_error != 0) {
                state->last_native_error_ = native_error;
                return SACCADE_ERROR_BACKEND;
            }
            return written;
        }
        ++stream->stats_.acquired;
        stream->stats_.last_frame_id = id;
        stream->stats_.last_timestamp_ns = timestamp_ns;
        return SACCADE_OK;
    } catch (const winrt::hresult_error& error) {
        int32_t native_error = error.code().value;
        if (stream->leased_) {
            state->close_lease(*stream, &native_error);
        }
        state->last_native_error_ = native_error;
        return SACCADE_ERROR_BACKEND;
    } catch (...) {
        int32_t native_error = static_cast<int32_t>(E_FAIL);
        if (stream->leased_) {
            state->close_lease(*stream, &native_error);
        }
        state->last_native_error_ = native_error;
        return SACCADE_ERROR_BACKEND;
    }
}

SaccadeResult SACCADE_CALL copy_damage(void* context, SaccadeCaptureStreamHandle handle, SaccadeFrameHandle frame,
                                       SaccadeRectI32* output, uint32_t capacity, uint32_t* count) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread() || count == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr || !stream->leased_ ||
        frame != frame_handle(state->stream_slot(*stream), stream->frame_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *count = stream->damage_count_;
    if (capacity < stream->damage_count_) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (stream->damage_count_ != 0 && output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (stream->damage_count_ != 0) {
        std::copy_n(stream->damage_.data(), stream->damage_count_, output);
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL release_frame(void* context, SaccadeCaptureStreamHandle handle, SaccadeFrameHandle frame) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr || !stream->leased_ ||
        frame != frame_handle(state->stream_slot(*stream), stream->frame_generation_)) {
        if (stream != nullptr) {
            ++stream->stats_.stale_releases;
        }
        return SACCADE_ERROR_STALE_HANDLE;
    }
    int32_t native_error = 0;
    state->close_lease(*stream, &native_error);
    ++stream->stats_.released;

    if (stream->recreate_pending_) {
        try {
            const winrt::Windows::Graphics::SizeInt32 size{static_cast<int32_t>(stream->width_),
                                                           static_cast<int32_t>(stream->height_)};
            stream->pool_.Recreate(state->winrt_device_, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                   static_cast<int32_t>(stream->queue_capacity_), size);
            stream->recreate_pending_ = false;
        } catch (const winrt::hresult_error& error) {
            preserve_first_error(error.code().value, &native_error);
        } catch (...) {
            preserve_first_error(static_cast<int32_t>(E_FAIL), &native_error);
        }
    }

    if (native_error != 0) {
        state->last_native_error_ = native_error;
        return SACCADE_ERROR_BACKEND;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL synchronize_capture(void* context, SaccadeCaptureStreamHandle handle, uint64_t) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread()) {
        return SACCADE_ERROR_STATE;
    }
    return state->find_stream(handle) == nullptr ? SACCADE_ERROR_STALE_HANDLE : SACCADE_OK;
}

SaccadeResult SACCADE_CALL memory_stats(void* context, SaccadeCaptureStreamHandle handle, SaccadeMemoryStats* output) {
    Impl* state = provider(context);
    if (state == nullptr || !state->owns_thread() || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Stream* stream = state->find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeMemoryStats value{};
    value.device_imported = stream->stats_.imported_bytes;
    value.framework_opaque = static_cast<uint64_t>(stream->width_) * stream->height_ * 4U * stream->queue_capacity_;
    value.high_water_bytes = stream->stats_.imported_high_water;
    return write_output(output, value);
}

} // namespace

ScreenCaptureProvider::ScreenCaptureProvider() noexcept {
    new (storage_.data()) Impl{};
}

ScreenCaptureProvider::~ScreenCaptureProvider() {
    Impl& state = impl();
    if (initialized_ && state.owns_thread()) {
        for (Impl::Stream& stream : state.streams_) {
            if (stream.active_) {
                state.close_stream(stream);
            }
        }
        state.winrt_device_ = nullptr;
        state.d3d_context_.Reset();
        state.d3d_device_.Reset();
        if (state.ro_initialized_) {
            RoUninitialize();
        }
    }
    state.~Impl();
}

ScreenCaptureProvider::Impl& ScreenCaptureProvider::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const ScreenCaptureProvider::Impl& ScreenCaptureProvider::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult ScreenCaptureProvider::initialize() noexcept {
    return initialize_native(0);
}

SaccadeResult ScreenCaptureProvider::initialize_native(uint64_t adapter_luid) noexcept {
    return initialize_impl(nullptr, adapter_luid);
}

SaccadeResult ScreenCaptureProvider::initialize(ID3D11Device* capture_device) noexcept {
    return initialize_impl(capture_device, 0);
}

SaccadeResult ScreenCaptureProvider::initialize_impl(ID3D11Device* capture_device, uint64_t adapter_luid) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    Impl& state = impl();
    state.owner_thread_ = GetCurrentThreadId();
    const HRESULT ro_result = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(ro_result) && ro_result != RPC_E_CHANGED_MODE) {
        return SACCADE_ERROR_BACKEND;
    }
    state.ro_initialized_ = SUCCEEDED(ro_result);
    try {
        if (!capture::GraphicsCaptureSession::IsSupported()) {
            if (state.ro_initialized_) {
                RoUninitialize();
                state.ro_initialized_ = false;
            }
            return SACCADE_ERROR_UNSUPPORTED;
        }
    } catch (const winrt::hresult_error&) {
        if (state.ro_initialized_) {
            RoUninitialize();
            state.ro_initialized_ = false;
        }
        return SACCADE_ERROR_BACKEND;
    }
    if (capture_device != nullptr) {
        state.d3d_device_ = capture_device;
        capture_device->GetImmediateContext(state.d3d_context_.GetAddressOf());
    } else {
        constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        ComPtr<IDXGIAdapter1> adapter;
        if (adapter_luid != 0) {
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return SACCADE_ERROR_BACKEND;
            for (UINT index = 0;
                 factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && luid_value(desc.AdapterLuid) == adapter_luid) break;
                adapter.Reset();
            }
            if (adapter == nullptr) return SACCADE_ERROR_NOT_FOUND;
        }
        const HRESULT device_result = D3D11CreateDevice(
            adapter.Get(), adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
            state.d3d_device_.GetAddressOf(), &selected, state.d3d_context_.GetAddressOf());
        if (FAILED(device_result)) {
            if (state.ro_initialized_) {
                RoUninitialize();
                state.ro_initialized_ = false;
            }
            return SACCADE_ERROR_BACKEND;
        }
    }
    if (state.d3d_context_ == nullptr) {
        state.d3d_device_.Reset();
        if (state.ro_initialized_) {
            RoUninitialize();
            state.ro_initialized_ = false;
        }
        return SACCADE_ERROR_BACKEND;
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(state.d3d_device_.As(&dxgi)) ||
        FAILED(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(state.winrt_device_))))) {
        state.d3d_context_.Reset();
        state.d3d_device_.Reset();
        if (state.ro_initialized_) {
            RoUninitialize();
            state.ro_initialized_ = false;
        }
        return SACCADE_ERROR_BACKEND;
    }
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())) && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        state.adapter_luid_ = luid_value(adapter_desc.AdapterLuid);
    }
    initialized_ = true;
    return SACCADE_OK;
}

ID3D11Device* ScreenCaptureProvider::device() const noexcept {
    return initialized_ ? impl().d3d_device_.Get() : nullptr;
}

ID3D11DeviceContext* ScreenCaptureProvider::context() const noexcept {
    return initialized_ ? impl().d3d_context_.Get() : nullptr;
}

uint64_t ScreenCaptureProvider::adapter_luid() const noexcept {
    return initialized_ ? impl().adapter_luid_ : 0;
}

SaccadeCaptureProviderDesc ScreenCaptureProvider::descriptor() noexcept {
    SaccadeCaptureProviderDesc desc{};
    if (!initialized_) {
        return desc;
    }
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.info.struct_size = sizeof(desc.info);
    desc.info.api_version = SACCADE_API_VERSION;
    desc.info.family = SACCADE_PROVIDER_FAMILY_CAPTURE;
    desc.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_DAMAGE;
    desc.info.stable_id = UINT64_C(0x5747435f44334431);
    desc.info.name = {reinterpret_cast<const uint8_t*>(provider_name), sizeof(provider_name) - 1U};
    desc.context = &impl();
    desc.ops.struct_size = sizeof(desc.ops);
    desc.ops.api_version = SACCADE_API_VERSION;
    desc.ops.enumerate_sources = enumerate_sources;
    desc.ops.create = create_stream;
    desc.ops.destroy = destroy_stream;
    desc.ops.start = start_stream;
    desc.ops.stop = stop_stream;
    desc.ops.acquire = acquire_frame;
    desc.ops.copy_damage = copy_damage;
    desc.ops.release = release_frame;
    desc.ops.synchronize = synchronize_capture;
    desc.ops.memory_stats = memory_stats;
    return desc;
}

SaccadeResult ScreenCaptureProvider::read_stats(SaccadeCaptureStreamHandle handle,
                                                ScreenCaptureStats* output) const noexcept {
    const Impl& state = impl();
    if (!initialized_ || !state.owns_thread() || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Stream* stream = state.find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *output = stream->stats_;
    return SACCADE_OK;
}

SaccadeResult ScreenCaptureProvider::read_native_frame(SaccadeCaptureStreamHandle handle, SaccadeFrameHandle frame,
                                                       NativeCapturedFrame* output) const noexcept {
    const Impl& state = impl();
    if (!initialized_ || !state.owns_thread() || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Stream* stream = state.find_stream(handle);
    if (stream == nullptr || !stream->leased_ ||
        frame != frame_handle(state.stream_slot(*stream), stream->frame_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    D3D11_TEXTURE2D_DESC desc{};
    stream->leased_texture_->GetDesc(&desc);
    *output = {stream->leased_texture_.Get(), 0, SACCADE_FORMAT_BGRA8, desc.Width, desc.Height, stream->adapter_luid_};
    return SACCADE_OK;
}

SaccadeResult ScreenCaptureProvider::read_last_native_error(int32_t* output) const noexcept {
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().last_native_error_;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

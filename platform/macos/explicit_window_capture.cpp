#include "platform/macos/explicit_window_capture.hpp"

#include <limits>

namespace saccade::platform::macos {
namespace {

constexpr uint32_t identity_flags = explicit_window_visible | explicit_window_current_space;
constexpr uint32_t stream_queue_capacity = 3;

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = static_cast<uint32_t>(sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    return value;
}

bool same_rect(const SaccadeRectI32& left, const SaccadeRectI32& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

bool same_identity(const ExplicitWindowIdentity& left, const ExplicitWindowIdentity& right) noexcept {
    return left.process_id == right.process_id && left.window_id == right.window_id && left.capture_source_id == right.capture_source_id &&
           same_rect(left.bounds, right.bounds) && left.flags == right.flags && left.reserved == right.reserved;
}

bool valid_identity(const ExplicitWindowIdentity& identity) noexcept {
    return identity.process_id != 0 && identity.window_id != 0 && identity.capture_source_id != 0 && identity.bounds.width > 0 &&
           identity.bounds.height > 0 && identity.flags == identity_flags && identity.reserved == 0;
}

uint32_t next_generation(uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1U : value;
}

} // namespace

ExplicitWindowCapture::~ExplicitWindowCapture() {
    if (initialized_ && owns_thread()) {
        // Destruction ends every frame borrow from this object. Consume the
        // provider lease before retiring the stream so native resources do
        // not remain live after the release callback becomes unreachable.
        if (leased_)
            (void)release_internal_frame();
        (void)retire(ExplicitWindowRetirementReason::shutdown);
    }
}

bool ExplicitWindowCapture::owns_thread() const noexcept {
    return initialized_ && pthread_equal(owner_, pthread_self()) != 0;
}

SaccadeResult ExplicitWindowCapture::initialize(SaccadeCaptureProviderDesc provider, void* native_context,
                                                ReadNativeFrameFn read_native_frame, uint32_t max_width, uint32_t max_height) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (provider.context == nullptr || native_context == nullptr || read_native_frame == nullptr || (max_width == 0) != (max_height == 0) ||
        provider.ops.enumerate_sources == nullptr || provider.ops.create == nullptr || provider.ops.destroy == nullptr ||
        provider.ops.start == nullptr || provider.ops.stop == nullptr || provider.ops.acquire == nullptr ||
        provider.ops.release == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    backend_ = provider;
    native_context_ = native_context;
    read_native_frame_ = read_native_frame;
    max_width_ = max_width;
    max_height_ = max_height;
    owner_ = pthread_self();
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::find_source(const ExplicitWindowIdentity& identity, SaccadeCaptureSourceInfo* output) noexcept {
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0;; ++index) {
        SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
        const SaccadeResult result = backend_.ops.enumerate_sources(backend_.context, index, &source);
        if (result != SACCADE_OK)
            return result;
        if (source.stable_id != identity.capture_source_id) {
            if (index == std::numeric_limits<uint32_t>::max())
                return SACCADE_ERROR_CAPACITY;
            continue;
        }
        if (source.kind != SACCADE_CAPTURE_SOURCE_WINDOW || !same_rect(source.desktop_bounds, identity.bounds))
            return SACCADE_ERROR_STALE_HANDLE;
        *output = source;
        return SACCADE_OK;
    }
}

SaccadeResult ExplicitWindowCapture::select(const ExplicitWindowIdentity& identity, uint64_t session_epoch) noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (!valid_identity(identity) || session_epoch == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (active_)
        return same_identity(identity_, identity) && session_epoch_ == session_epoch ? SACCADE_OK : SACCADE_ERROR_BUSY;
    if (retiring_ || stream_ != 0)
        return SACCADE_ERROR_BUSY;
    if (session_epoch <= last_session_epoch_)
        return SACCADE_ERROR_STALE_HANDLE;

    SaccadeCaptureSourceInfo source{};
    SaccadeResult result = find_source(identity, &source);
    if (result != SACCADE_OK)
        return result;

    SaccadeCaptureStreamDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.source_id = identity.capture_source_id;
    desc.pixel_format = SACCADE_FORMAT_BGRA8;
    desc.queue_capacity = stream_queue_capacity;
    desc.max_width = max_width_;
    desc.max_height = max_height_;
    SaccadeCaptureStreamHandle stream = 0;
    result = backend_.ops.create(backend_.context, &desc, &stream);
    if (result != SACCADE_OK)
        return result;
    if (stream == 0)
        return SACCADE_ERROR_STATE;

    stream_ = stream;
    result = backend_.ops.start(backend_.context, stream_);
    if (result != SACCADE_OK) {
        retiring_ = true;
        retirement_reason_ = ExplicitWindowRetirementReason::disconnected;
        const SaccadeResult cleanup = finalize_retirement();
        return cleanup == SACCADE_OK ? result : cleanup;
    }

    identity_ = identity;
    session_epoch_ = session_epoch;
    last_session_epoch_ = session_epoch;
    retirement_reason_ = ExplicitWindowRetirementReason::none;
    running_ = true;
    active_ = true;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::synchronize(const ExplicitWindowIdentity& identity) noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (!active_)
        return SACCADE_ERROR_STALE_HANDLE;
    if (!same_identity(identity_, identity)) {
        const SaccadeResult retired = retire(ExplicitWindowRetirementReason::identity_changed);
        return retired == SACCADE_OK || retired == SACCADE_ERROR_BUSY ? SACCADE_ERROR_STALE_HANDLE : retired;
    }

    SaccadeCaptureSourceInfo source{};
    const SaccadeResult result = find_source(identity_, &source);
    if (result == SACCADE_OK)
        return SACCADE_OK;
    if (result == SACCADE_ERROR_NOT_FOUND) {
        const SaccadeResult retired = retire(ExplicitWindowRetirementReason::disappeared);
        return retired == SACCADE_OK || retired == SACCADE_ERROR_BUSY ? SACCADE_ERROR_NOT_FOUND : retired;
    }
    if (result == SACCADE_ERROR_STALE_HANDLE) {
        const SaccadeResult retired = retire(ExplicitWindowRetirementReason::identity_changed);
        return retired == SACCADE_OK || retired == SACCADE_ERROR_BUSY ? SACCADE_ERROR_STALE_HANDLE : retired;
    }
    return result;
}

SaccadeResult ExplicitWindowCapture::acquire(SceneCaptureFrame* output) noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!active_ || retiring_)
        return SACCADE_ERROR_STALE_HANDLE;
    if (leased_)
        return SACCADE_ERROR_BUSY;

    SaccadeCapturedFrame frame = output_structure<SaccadeCapturedFrame>();
    SaccadeResult result = backend_.ops.acquire(backend_.context, stream_, 0, &frame);
    if (result != SACCADE_OK)
        return result;
    leased_ = true;
    leased_frame_ = frame.frame;
    lease_published_ = false;
    if (frame.frame == 0 || frame.source_id != identity_.capture_source_id || frame.width == 0 || frame.height == 0 ||
        frame.pixel_format != SACCADE_FORMAT_BGRA8) {
        active_ = false;
        retiring_ = true;
        retirement_reason_ = ExplicitWindowRetirementReason::identity_changed;
        const SaccadeResult cleanup = finalize_retirement();
        return cleanup == SACCADE_OK ? SACCADE_ERROR_STALE_HANDLE : cleanup;
    }

    NativeCapturedFrame native{};
    result = read_native_frame_(native_context_, stream_, frame.frame, &native);
    if (result != SACCADE_OK) {
        const SaccadeResult released = release_internal_frame();
        if (released != SACCADE_OK) {
            active_ = false;
            retiring_ = true;
            retirement_reason_ = ExplicitWindowRetirementReason::disconnected;
            return released;
        }
        return result;
    }

    output->frame = frame;
    output->native = native;
    output->display_id = identity_.window_id;
    output->topology_epoch = session_epoch_;
    output->slot = 0;
    output->generation = generation_;
    lease_published_ = true;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::release_internal_frame() noexcept {
    if (!leased_)
        return SACCADE_OK;
    const SaccadeResult result = backend_.ops.release(backend_.context, stream_, leased_frame_);
    if (result != SACCADE_OK)
        return result;
    leased_frame_ = 0;
    leased_ = false;
    lease_published_ = false;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::release(const SceneCaptureFrame& frame) noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (!leased_ || !lease_published_ || frame.slot != 0 || frame.generation != generation_ || frame.topology_epoch != session_epoch_ ||
        frame.display_id != identity_.window_id || frame.frame.source_id != identity_.capture_source_id ||
        frame.frame.frame != leased_frame_) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult result = release_internal_frame();
    if (result != SACCADE_OK)
        return result;
    if (retiring_)
        (void)finalize_retirement();
    // The provider frame lease has been consumed exactly once. Native stop or
    // destroy failures remain represented by retiring_ and are drained by the
    // owner separately; returning them here would make generic frame owners
    // retry an already-consumed lease with a stale handle.
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::finalize_retirement() noexcept {
    if (!retiring_)
        return SACCADE_OK;
    if (leased_) {
        if (lease_published_)
            return SACCADE_ERROR_BUSY;
        const SaccadeResult released = release_internal_frame();
        if (released != SACCADE_OK)
            return released;
    }
    if (running_) {
        const SaccadeResult stopped = backend_.ops.stop(backend_.context, stream_);
        if (stopped != SACCADE_OK)
            return stopped;
        running_ = false;
    }
    if (stream_ != 0) {
        const SaccadeResult destroyed = backend_.ops.destroy(backend_.context, stream_);
        if (destroyed != SACCADE_OK)
            return destroyed;
    }
    stream_ = 0;
    identity_ = {};
    session_epoch_ = 0;
    generation_ = next_generation(generation_);
    retiring_ = false;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowCapture::retire(ExplicitWindowRetirementReason reason) noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (reason == ExplicitWindowRetirementReason::none)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (active_) {
        active_ = false;
        retiring_ = true;
        retirement_reason_ = reason;
    }
    return finalize_retirement();
}

SaccadeResult ExplicitWindowCapture::drain_retirement() noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    return finalize_retirement();
}

SaccadeResult ExplicitWindowCapture::shutdown() noexcept {
    if (!initialized_ || !owns_thread())
        return SACCADE_ERROR_STATE;
    if (active_) {
        const SaccadeResult result = retire(ExplicitWindowRetirementReason::shutdown);
        if (result != SACCADE_OK)
            return result;
    } else if (retiring_) {
        const SaccadeResult result = finalize_retirement();
        if (result != SACCADE_OK)
            return result;
    }
    backend_ = {};
    native_context_ = nullptr;
    read_native_frame_ = nullptr;
    owner_ = {};
    max_width_ = 0;
    max_height_ = 0;
    initialized_ = false;
    retirement_reason_ = ExplicitWindowRetirementReason::none;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

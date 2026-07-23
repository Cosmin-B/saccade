#include "platform/macos/screen_capture.hpp"

#include "core/stack_string_builder.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CVPixelBufferIOSurface.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <sched.h>
#include <string_view>
#include <time.h>

namespace {

constexpr uint64_t provider_id = UINT64_C(0x5343434150540001);
constexpr uint64_t source_kind_shift = 56;
constexpr uint64_t source_value_mask = UINT64_C(0x00FFFFFFFFFFFFFF);
constexpr uint32_t maximum_sources = 256;
constexpr uint32_t maximum_streams = 16;
constexpr uint32_t frame_slots = 3;
constexpr uint32_t maximum_damage_rects = 64;
constexpr uint32_t source_name_capacity = 128;
constexpr int32_t active_capture_hz = 120;
constexpr uint64_t cold_wait_ns = UINT64_C(5'000'000'000);

constexpr uint32_t frame_free = 0;
constexpr uint32_t frame_published = 1;
constexpr uint32_t frame_acquired = 2;

uint64_t source_id(uint32_t kind, uint64_t value) noexcept {
    return (static_cast<uint64_t>(kind) << source_kind_shift) | (value & source_value_mask);
}

uint64_t stream_handle(uint32_t slot, uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32U) | static_cast<uint64_t>(slot + 1U);
}

uint64_t frame_handle(uint32_t stream_slot, uint32_t frame_slot, uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32U) | (static_cast<uint64_t>(stream_slot + 1U) << 8U) |
           static_cast<uint64_t>(frame_slot + 1U);
}

bool decode_stream(uint64_t handle, uint32_t* slot, uint32_t* generation) noexcept {
    const uint32_t encoded_slot = static_cast<uint32_t>(handle);
    const uint32_t encoded_generation = static_cast<uint32_t>(handle >> 32U);
    if (slot == nullptr || generation == nullptr || encoded_slot == 0 || encoded_slot > maximum_streams ||
        encoded_generation == 0) {
        return false;
    }
    *slot = encoded_slot - 1U;
    *generation = encoded_generation;
    return true;
}

bool decode_frame(uint64_t handle, uint32_t* stream_slot, uint32_t* frame_slot, uint32_t* generation) noexcept {
    const uint32_t low = static_cast<uint32_t>(handle);
    const uint32_t encoded_frame = low & UINT32_C(0xFF);
    const uint32_t encoded_stream = (low >> 8U) & UINT32_C(0xFF);
    const uint32_t encoded_generation = static_cast<uint32_t>(handle >> 32U);
    if (stream_slot == nullptr || frame_slot == nullptr || generation == nullptr || encoded_stream == 0 ||
        encoded_stream > maximum_streams || encoded_frame == 0 || encoded_frame > frame_slots ||
        encoded_generation == 0) {
        return false;
    }
    *stream_slot = encoded_stream - 1U;
    *frame_slot = encoded_frame - 1U;
    *generation = encoded_generation;
    return true;
}

uint32_t next_generation(uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1U : value;
}

uint64_t saturating_add(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

uint64_t timestamp_ns(uint64_t ticks, mach_timebase_info_data_t timebase) noexcept {
    if (ticks == 0 || timebase.numer == 0 || timebase.denom == 0) return 0;
    const uint64_t whole = ticks / timebase.denom;
    const uint64_t remainder = ticks % timebase.denom;
    return whole * timebase.numer + remainder * timebase.numer / timebase.denom;
}

template <size_t Capacity>
void copy_source_name(std::array<char, Capacity>* destination, std::string_view value) noexcept {
    static_assert(Capacity > 0);
    saccade::core::StackStringBuilder<Capacity - 1U> name;
    (void)name.append(value);
    std::memcpy(destination->data(), name.c_str(), name.size() + 1U);
}

template <typename Structure> bool valid_output(const Structure* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    uint32_t size = 0;
    uint32_t version = 0;
    std::memcpy(&size, output, sizeof(size));
    std::memcpy(&version,
                static_cast<const uint8_t*>(static_cast<const void*>(output)) + offsetof(Structure, api_version),
                sizeof(version));
    return static_cast<size_t>(size) >= offsetof(Structure, reserved) &&
           (version >> 16U) == (SACCADE_API_VERSION >> 16U);
}

template <typename Structure> SaccadeResult write_output(Structure* output, Structure value) noexcept {
    if (!valid_output(output)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t size = 0;
    std::memcpy(&size, output, sizeof(size));
    const size_t copy_size = std::min(static_cast<size_t>(size), sizeof(Structure));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(output, &value, copy_size);
    return SACCADE_OK;
}

template <typename Structure> SaccadeResult read_input(const Structure* input, Structure* value) noexcept {
    if (input == nullptr || value == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t size = 0;
    std::memcpy(&size, input, sizeof(size));
    if (static_cast<size_t>(size) < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *value = {};
    std::memcpy(value, input, std::min(static_cast<size_t>(size), sizeof(Structure)));
    if ((value->api_version >> 16U) != (SACCADE_API_VERSION >> 16U)) {
        return SACCADE_ERROR_VERSION;
    }
    const size_t reserved_offset = offsetof(Structure, reserved);
    const size_t available = std::min(static_cast<size_t>(size), sizeof(Structure));
    const auto* bytes = reinterpret_cast<const uint8_t*>(value);
    for (size_t index = reserved_offset; index < available; ++index) {
        if (bytes[index] != 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }
    return SACCADE_OK;
}

dispatch_time_t wait_time(uint64_t timeout_ns) noexcept {
    const uint64_t bounded = std::min(timeout_ns, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    return dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(bounded));
}

} // namespace

@interface SaccadeScreenCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
  @public
    void* owner_;
    uint32_t stream_slot_;
}
@end

namespace saccade::platform::macos {

struct ScreenCaptureProvider::Impl {
    struct AtomicStats {
        std::atomic<uint64_t> callbacks{0};
        std::atomic<uint64_t> published{0};
        std::atomic<uint64_t> acquired{0};
        std::atomic<uint64_t> released{0};
        std::atomic<uint64_t> replaced{0};
        std::atomic<uint64_t> dropped_capacity{0};
        std::atomic<uint64_t> dropped_status{0};
        std::atomic<uint64_t> import_failures{0};
        std::atomic<uint64_t> stale_releases{0};
        std::atomic<uint64_t> start_failures{0};
        std::atomic<uint64_t> stop_failures{0};
        std::atomic<uint64_t> imported_bytes{0};
        std::atomic<uint64_t> imported_high_water{0};
        std::atomic<uint64_t> last_frame_id{0};
        std::atomic<uint64_t> last_display_time{0};
        std::atomic<uint64_t> callback_sequence_{0};
        std::atomic<uint64_t> status_sequence_{0};
        std::atomic<uint64_t> did_stop_with_error_{0};

        void reset() noexcept {
            callbacks.store(0, std::memory_order_relaxed);
            published.store(0, std::memory_order_relaxed);
            acquired.store(0, std::memory_order_relaxed);
            released.store(0, std::memory_order_relaxed);
            replaced.store(0, std::memory_order_relaxed);
            dropped_capacity.store(0, std::memory_order_relaxed);
            dropped_status.store(0, std::memory_order_relaxed);
            import_failures.store(0, std::memory_order_relaxed);
            stale_releases.store(0, std::memory_order_relaxed);
            start_failures.store(0, std::memory_order_relaxed);
            stop_failures.store(0, std::memory_order_relaxed);
            imported_bytes.store(0, std::memory_order_relaxed);
            imported_high_water.store(0, std::memory_order_relaxed);
            last_frame_id.store(0, std::memory_order_relaxed);
            last_display_time.store(0, std::memory_order_relaxed);
            callback_sequence_.store(0, std::memory_order_relaxed);
            status_sequence_.store(0, std::memory_order_relaxed);
            did_stop_with_error_.store(0, std::memory_order_relaxed);
        }

        ScreenCaptureStats read() const noexcept {
            ScreenCaptureStats value{};
            value.callbacks = callbacks.load(std::memory_order_relaxed);
            value.published = published.load(std::memory_order_relaxed);
            value.acquired = acquired.load(std::memory_order_relaxed);
            value.released = released.load(std::memory_order_relaxed);
            value.replaced = replaced.load(std::memory_order_relaxed);
            value.dropped_capacity = dropped_capacity.load(std::memory_order_relaxed);
            value.dropped_status = dropped_status.load(std::memory_order_relaxed);
            value.import_failures = import_failures.load(std::memory_order_relaxed);
            value.stale_releases = stale_releases.load(std::memory_order_relaxed);
            value.start_failures = start_failures.load(std::memory_order_relaxed);
            value.stop_failures = stop_failures.load(std::memory_order_relaxed);
            value.imported_bytes = imported_bytes.load(std::memory_order_relaxed);
            value.imported_high_water = imported_high_water.load(std::memory_order_relaxed);
            value.last_frame_id = last_frame_id.load(std::memory_order_relaxed);
            value.last_display_time = last_display_time.load(std::memory_order_relaxed);
            value.latest_callback_sequence = callback_sequence_.load(std::memory_order_relaxed);
            value.latest_status_sequence = status_sequence_.load(std::memory_order_relaxed);
            value.did_stop_with_error = did_stop_with_error_.load(std::memory_order_relaxed);
            return value;
        }
    };

    struct Source {
        uint64_t stable_id_ = 0;
        uint32_t kind_ = 0;
        SaccadeRectI32 bounds_{};
        std::array<char, source_name_capacity> name_{};
        __strong SCDisplay* display_ = nil;
        __strong SCWindow* window_ = nil;
    };

    struct Frame {
        std::atomic<uint32_t> state_{frame_free};
        uint32_t generation_ = 1;
        CVPixelBufferRef pixel_buffer_ = nullptr;
        CVMetalTextureRef metal_texture_ = nullptr;
        IOSurfaceRef iosurface_ = nullptr;
        uint64_t iosurface_id_ = 0;
        uint64_t frame_id_ = 0;
        uint64_t display_time_ = 0;
        uint64_t timestamp_ns_ = 0;
        uint64_t transform_epoch_ = 0;
        uint64_t imported_bytes_ = 0;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t pixel_format_ = 0;
        uint32_t damage_count_ = 0;
        std::array<SaccadeRectI32, maximum_damage_rects> damage_{};
    };

    struct Stream {
        bool active_ = false;
        std::atomic<bool> started_{false};
        std::atomic<bool> failed_{false};
        uint32_t generation_ = 1;
        uint32_t slot_ = 0;
        uint64_t source_id_ = 0;
        uint64_t next_frame_id_ = 1;
        uint64_t transform_epoch_ = 1;
        std::atomic<uint32_t> pending_{0};
        std::array<Frame, frame_slots> frames_{};
        dispatch_semaphore_t available_ = nullptr;
        dispatch_queue_t callback_queue_ = nullptr;
        __strong SCStream* stream_ = nil;
        __strong SaccadeScreenCaptureOutput* output_ = nil;
        AtomicStats stats_{};
    };

    explicit Impl(id<MTLDevice> device) noexcept : device_(device) {
        if (mach_timebase_info(&timebase_) != KERN_SUCCESS) timebase_ = {};
    }

    ~Impl() noexcept {
        for (Stream& stream : streams_) {
            destroy(stream);
        }
        if (texture_cache_ != nullptr) {
            CFRelease(texture_cache_);
        }
    }

    void release_frame(Frame& frame) noexcept {
        if (frame.metal_texture_ != nullptr) {
            CFRelease(frame.metal_texture_);
            frame.metal_texture_ = nullptr;
        }
        if (frame.pixel_buffer_ != nullptr) {
            CVPixelBufferRelease(frame.pixel_buffer_);
            frame.pixel_buffer_ = nullptr;
        }
        frame.iosurface_ = nullptr;
        frame.iosurface_id_ = 0;
        frame.imported_bytes_ = 0;
        frame.damage_count_ = 0;
        frame.generation_ = next_generation(frame.generation_);
        frame.state_.store(frame_free, std::memory_order_release);
    }

    void discard_pending(Stream& stream) noexcept {
        const uint32_t pending = stream.pending_.exchange(0, std::memory_order_acq_rel);
        if (pending != 0) {
            Frame& frame = stream.frames_[pending - 1U];
            update_imported(stream, frame.imported_bytes_, 0);
            release_frame(frame);
        }
        while (dispatch_semaphore_wait(stream.available_, DISPATCH_TIME_NOW) == 0) {}
    }

    void destroy(Stream& stream) noexcept {
        if (!stream.active_) {
            return;
        }
        if (stream.started_.load(std::memory_order_acquire)) {
            dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
            [stream.stream_ stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(stopped); }];
            (void)dispatch_semaphore_wait(stopped, wait_time(cold_wait_ns));
            stream.started_.store(false, std::memory_order_release);
        }
        if (stream.callback_queue_ != nullptr) {
            dispatch_sync(stream.callback_queue_, ^{});
        }
        discard_pending(stream);
        for (Frame& frame : stream.frames_) {
            if (frame.state_.load(std::memory_order_acquire) != frame_free) {
                release_frame(frame);
            }
        }
        stream.stream_ = nil;
        stream.output_ = nil;
        stream.callback_queue_ = nullptr;
        stream.available_ = nullptr;
        stream.source_id_ = 0;
        stream.active_ = false;
        stream.generation_ = next_generation(stream.generation_);
    }

    SaccadeResult refresh_sources() noexcept {
        if (!CGPreflightScreenCaptureAccess()) return SACCADE_ERROR_PERMISSION;

        __block SCShareableContent* content = nil;
        __block NSError* failure = nil;
        dispatch_semaphore_t complete = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                                   onScreenWindowsOnly:YES
                                                     completionHandler:^(SCShareableContent* value, NSError* error) {
                                                       content = value;
                                                       failure = error;
                                                       dispatch_semaphore_signal(complete);
                                                     }];
        if (dispatch_semaphore_wait(complete, wait_time(cold_wait_ns)) != 0 || failure != nil || content == nil) {
            return SACCADE_ERROR_BACKEND;
        }

        source_count_ = 0;
        for (SCDisplay* display in content.displays) {
            if (source_count_ == maximum_sources) {
                break;
            }
            Source& source = sources_[source_count_++];
            source = {};
            source.kind_ = SACCADE_CAPTURE_SOURCE_DISPLAY;
            source.stable_id_ = source_id(source.kind_, display.displayID);
            source.bounds_ = {
                static_cast<int32_t>(display.frame.origin.x), static_cast<int32_t>(display.frame.origin.y),
                static_cast<int32_t>(display.frame.size.width), static_cast<int32_t>(display.frame.size.height)};
            saccade::core::StackStringBuilder<source_name_capacity - 1U> name;
            (void)name.append("Display ");
            (void)name.append_unsigned(display.displayID);
            copy_source_name(&source.name_, name.view());
            source.display_ = display;
        }
        for (SCWindow* window in content.windows) {
            if (source_count_ == maximum_sources) {
                break;
            }
            if (window.owningApplication.processID == NSProcessInfo.processInfo.processIdentifier) {
                continue;
            }
            Source& source = sources_[source_count_++];
            source = {};
            source.kind_ = SACCADE_CAPTURE_SOURCE_WINDOW;
            source.stable_id_ = source_id(source.kind_, window.windowID);
            source.bounds_ = {static_cast<int32_t>(window.frame.origin.x), static_cast<int32_t>(window.frame.origin.y),
                              static_cast<int32_t>(window.frame.size.width),
                              static_cast<int32_t>(window.frame.size.height)};
            NSString* title = window.title.length != 0 ? window.title : @"Window";
            const char* utf8 = title.UTF8String;
            if (utf8 != nullptr) {
                copy_source_name(&source.name_, utf8);
            }
            source.window_ = window;
        }
        shareable_content_ = content;
        return SACCADE_OK;
    }

    Source* find_source(uint64_t stable_id) noexcept {
        for (uint32_t index = 0; index < source_count_; ++index) {
            if (sources_[index].stable_id_ == stable_id) {
                return &sources_[index];
            }
        }
        return nullptr;
    }

    Stream* find_stream(uint64_t handle) noexcept {
        uint32_t slot = 0;
        uint32_t generation = 0;
        if (!decode_stream(handle, &slot, &generation)) {
            return nullptr;
        }
        Stream& stream = streams_[slot];
        return stream.active_ && stream.generation_ == generation ? &stream : nullptr;
    }

    const Stream* find_stream(uint64_t handle) const noexcept { return const_cast<Impl*>(this)->find_stream(handle); }

    void update_imported(Stream& stream, uint64_t removed, uint64_t added) noexcept {
        uint64_t current = stream.stats_.imported_bytes.load(std::memory_order_relaxed);
        current = current >= removed ? current - removed : 0;
        current = saturating_add(current, added);
        stream.stats_.imported_bytes.store(current, std::memory_order_relaxed);
        const uint64_t high = stream.stats_.imported_high_water.load(std::memory_order_relaxed);
        if (current > high) {
            stream.stats_.imported_high_water.store(current, std::memory_order_relaxed);
        }
    }

    void publish(uint32_t stream_slot, CMSampleBufferRef sample) noexcept {
        if (stream_slot >= maximum_streams) {
            return;
        }
        Stream& stream = streams_[stream_slot];
        if (!stream.active_ || !stream.started_.load(std::memory_order_acquire)) {
            return;
        }
        stream.stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
        const uint64_t callback_sequence =
            stream.stats_.callback_sequence_.fetch_add(1, std::memory_order_relaxed) + 1U;

        NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
        NSDictionary* metadata = attachments.count != 0 ? attachments[0] : nil;
        NSNumber* status_value = metadata[SCStreamFrameInfoStatus];
        if (![status_value isKindOfClass:[NSNumber class]]) {
            stream.stats_.dropped_status.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const SCFrameStatus status = static_cast<SCFrameStatus>(status_value.integerValue);
        if (status != SCFrameStatusComplete && status != SCFrameStatusStarted) {
            stream.stats_.dropped_status.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        stream.stats_.status_sequence_.store(callback_sequence, std::memory_order_relaxed);
        CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(CMSampleBufferGetImageBuffer(sample));
        IOSurfaceRef surface = pixel_buffer != nullptr ? CVPixelBufferGetIOSurface(pixel_buffer) : nullptr;
        if (pixel_buffer == nullptr || surface == nullptr) {
            stream.stats_.import_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        uint32_t frame_slot = frame_slots;
        for (uint32_t index = 0; index < frame_slots; ++index) {
            if (stream.frames_[index].state_.load(std::memory_order_acquire) == frame_free) {
                frame_slot = index;
                break;
            }
        }
        if (frame_slot == frame_slots) {
            stream.stats_.dropped_capacity.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Frame& frame = stream.frames_[frame_slot];
        CVPixelBufferRetain(pixel_buffer);
        CVMetalTextureRef metal_texture = nullptr;
        const size_t width = CVPixelBufferGetWidth(pixel_buffer);
        const size_t height = CVPixelBufferGetHeight(pixel_buffer);
        const CVReturn import_result =
            CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texture_cache_, pixel_buffer, nullptr,
                                                      MTLPixelFormatBGRA8Unorm, width, height, 0, &metal_texture);
        if (import_result != kCVReturnSuccess || metal_texture == nullptr) {
            CVPixelBufferRelease(pixel_buffer);
            stream.stats_.import_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        frame.pixel_buffer_ = pixel_buffer;
        frame.metal_texture_ = metal_texture;
        frame.iosurface_ = surface;
        frame.iosurface_id_ = IOSurfaceGetID(surface);
        frame.frame_id_ = stream.next_frame_id_++;
        frame.display_time_ = [metadata[SCStreamFrameInfoDisplayTime] unsignedLongLongValue];
        frame.timestamp_ns_ = timestamp_ns(frame.display_time_, timebase_);
        if (frame.timestamp_ns_ == 0) {
            const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
            if (CMTIME_IS_NUMERIC(presentation) && presentation.value >= 0) {
                const CMTime nanoseconds =
                    CMTimeConvertScale(presentation, INT32_C(1'000'000'000), kCMTimeRoundingMethod_Default);
                frame.timestamp_ns_ = static_cast<uint64_t>(nanoseconds.value);
            }
        }
        frame.transform_epoch_ = stream.transform_epoch_;
        frame.width_ = static_cast<uint32_t>(width);
        frame.height_ = static_cast<uint32_t>(height);
        frame.pixel_format_ = SACCADE_FORMAT_BGRA8;
        frame.imported_bytes_ = IOSurfaceGetAllocSize(surface);
        frame.damage_count_ = 0;
        NSArray* damage = metadata[SCStreamFrameInfoDirtyRects];
        for (id value in damage) {
            if (frame.damage_count_ == maximum_damage_rects) {
                break;
            }
            CGRect rect = CGRectZero;
            if ([value isKindOfClass:[NSValue class]]) {
                rect = [static_cast<NSValue*>(value) rectValue];
            } else if ([value isKindOfClass:[NSDictionary class]]) {
                NSDictionary* dictionary = static_cast<NSDictionary*>(value);
                if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)dictionary, &rect)) {
                    continue;
                }
            } else {
                continue;
            }
            frame.damage_[frame.damage_count_++] = {
                static_cast<int32_t>(rect.origin.x), static_cast<int32_t>(rect.origin.y),
                static_cast<int32_t>(rect.size.width), static_cast<int32_t>(rect.size.height)};
        }
        update_imported(stream, 0, frame.imported_bytes_);
        frame.state_.store(frame_published, std::memory_order_release);

        const uint32_t replaced = stream.pending_.exchange(frame_slot + 1U, std::memory_order_acq_rel);
        if (replaced != 0) {
            Frame& old = stream.frames_[replaced - 1U];
            update_imported(stream, old.imported_bytes_, 0);
            release_frame(old);
            stream.stats_.replaced.fetch_add(1, std::memory_order_relaxed);
        }
        stream.stats_.last_frame_id.store(frame.frame_id_, std::memory_order_relaxed);
        stream.stats_.last_display_time.store(frame.display_time_, std::memory_order_relaxed);
        stream.stats_.published.fetch_add(1, std::memory_order_relaxed);
        dispatch_semaphore_signal(stream.available_);
    }

    void record_callback_failure(uint32_t stream_slot) noexcept {
        if (stream_slot < maximum_streams) {
            streams_[stream_slot].stats_.import_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void record_stop(uint32_t stream_slot, bool with_error) noexcept {
        if (with_error && stream_slot < maximum_streams) {
            Stream& stream = streams_[stream_slot];
            stream.stats_.did_stop_with_error_.store(1, std::memory_order_relaxed);
            stream.started_.store(false, std::memory_order_release);
            stream.failed_.store(true, std::memory_order_release);
            dispatch_semaphore_signal(stream.available_);
        }
    }

    id<MTLDevice> device_ = nil;
    CVMetalTextureCacheRef texture_cache_ = nullptr;
    mach_timebase_info_data_t timebase_{};
    __strong SCShareableContent* shareable_content_ = nil;
    std::array<Source, maximum_sources> sources_{};
    uint32_t source_count_ = 0;
    std::array<Stream, maximum_streams> streams_{};
};

} // namespace saccade::platform::macos

@implementation SaccadeScreenCaptureOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type == SCStreamOutputTypeScreen && owner_ != nullptr) {
        auto* state = static_cast<saccade::platform::macos::ScreenCaptureProvider::Impl*>(owner_);
        @try {
            state->publish(stream_slot_, sampleBuffer);
        } @catch (NSException*) {
            state->record_callback_failure(stream_slot_);
        }
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    if (owner_ != nullptr) {
        auto* state = static_cast<saccade::platform::macos::ScreenCaptureProvider::Impl*>(owner_);
        state->record_stop(stream_slot_, error != nil);
    }
}

@end

namespace saccade::platform::macos {
namespace {

using Impl = ScreenCaptureProvider::Impl;

ScreenCaptureProvider::Impl* provider(void* context) noexcept {
    return static_cast<ScreenCaptureProvider::Impl*>(context);
}

SaccadeResult SACCADE_CALL enumerate_sources(void* context, uint32_t index, SaccadeCaptureSourceInfo* output) {
    Impl* state = provider(context);
    if (state == nullptr || output == nullptr) {
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
    if (state == nullptr || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    SaccadeCaptureStreamDesc desc{};
    const SaccadeResult read_result = read_input(input, &desc);
    if (read_result != SACCADE_OK) {
        return read_result;
    }
    if (desc.source_id == 0 || desc.pixel_format != SACCADE_FORMAT_BGRA8 || desc.queue_capacity != frame_slots ||
        (desc.flags & ~(static_cast<uint64_t>(capture_source_cursor) | capture_source_audio)) != 0 ||
        (desc.flags & capture_source_audio) != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state->source_count_ == 0) {
        const SaccadeResult result = state->refresh_sources();
        if (result != SACCADE_OK) {
            return result;
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

    Impl::Stream& stream = state->streams_[slot];
    stream.active_ = true;
    stream.slot_ = slot;
    stream.source_id_ = desc.source_id;
    stream.next_frame_id_ = 1;
    stream.transform_epoch_ = 1;
    stream.failed_.store(false, std::memory_order_relaxed);
    stream.stats_.reset();
    stream.available_ = dispatch_semaphore_create(0);
    stream.callback_queue_ = dispatch_queue_create("dev.saccade.capture.output", DISPATCH_QUEUE_SERIAL);
    stream.output_ = [SaccadeScreenCaptureOutput new];
    stream.output_->owner_ = state;
    stream.output_->stream_slot_ = slot;

    SCContentFilter* filter = nil;
    if (source->kind_ == SACCADE_CAPTURE_SOURCE_DISPLAY) {
        NSMutableArray<SCRunningApplication*>* excluded = [NSMutableArray array];
        const pid_t process = NSProcessInfo.processInfo.processIdentifier;
        for (SCRunningApplication* app in state->shareable_content_.applications) {
            if (app.processID == process) {
                [excluded addObject:app];
            }
        }
        filter = [[SCContentFilter alloc] initWithDisplay:source->display_
                                    excludingApplications:excluded
                                         exceptingWindows:@[]];
        if (@available(macOS 14.2, *)) {
            filter.includeMenuBar = YES;
        }
    } else {
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:source->window_];
    }

    SCStreamConfiguration* config = [SCStreamConfiguration new];
    const double point_scale = std::max(static_cast<double>(filter.pointPixelScale), 1.0);
    const size_t source_width =
        static_cast<size_t>(std::ceil(std::max(static_cast<double>(filter.contentRect.size.width), 1.0) * point_scale));
    const size_t source_height = static_cast<size_t>(
        std::ceil(std::max(static_cast<double>(filter.contentRect.size.height), 1.0) * point_scale));
    double fit_scale = 1.0;
    if (desc.max_width != 0) {
        fit_scale = std::min(fit_scale, static_cast<double>(desc.max_width) / static_cast<double>(source_width));
    }
    if (desc.max_height != 0) {
        fit_scale = std::min(fit_scale, static_cast<double>(desc.max_height) / static_cast<double>(source_height));
    }
    config.width = std::max(static_cast<size_t>(1),
                            static_cast<size_t>(std::floor(static_cast<double>(source_width) * fit_scale)));
    config.height = std::max(static_cast<size_t>(1),
                             static_cast<size_t>(std::floor(static_cast<double>(source_height) * fit_scale)));
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.minimumFrameInterval = CMTimeMake(1, active_capture_hz);
    config.queueDepth = 3;
    config.captureResolution = SCCaptureResolutionBest;
    config.showsCursor = (desc.flags & capture_source_cursor) != 0;
    config.capturesAudio = NO;
    if (@available(macOS 15.0, *)) {
        config.captureMicrophone = NO;
        config.captureDynamicRange = SCCaptureDynamicRangeSDR;
    }
    config.scalesToFit = fit_scale < 1.0 ? YES : NO;
    config.preservesAspectRatio = YES;
    config.shouldBeOpaque = YES;
    config.ignoreShadowsDisplay = YES;
    config.ignoreShadowsSingleWindow = YES;
    if (@available(macOS 14.2, *)) {
        config.includeChildWindows = YES;
    }
    config.streamName = @"Saccade perception";

    stream.stream_ = [[SCStream alloc] initWithFilter:filter configuration:config delegate:stream.output_];
    NSError* error = nil;
    const BOOL added = [stream.stream_ addStreamOutput:stream.output_
                                                  type:SCStreamOutputTypeScreen
                                    sampleHandlerQueue:stream.callback_queue_
                                                 error:&error];
    if (!added || error != nil) {
        state->destroy(stream);
        return SACCADE_ERROR_BACKEND;
    }
    *output = stream_handle(slot, stream.generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL destroy_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->destroy(*stream);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL start_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (stream->started_.load(std::memory_order_acquire)) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    stream->failed_.store(false, std::memory_order_release);
    __block NSError* failure = nil;
    dispatch_semaphore_t complete = dispatch_semaphore_create(0);
    [stream->stream_ startCaptureWithCompletionHandler:^(NSError* error) {
      failure = error;
      dispatch_semaphore_signal(complete);
    }];
    if (dispatch_semaphore_wait(complete, wait_time(cold_wait_ns)) != 0 || failure != nil) {
        stream->stats_.start_failures.fetch_add(1, std::memory_order_relaxed);
        return SACCADE_ERROR_BACKEND;
    }
    stream->started_.store(true, std::memory_order_release);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL stop_stream(void* context, SaccadeCaptureStreamHandle handle) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!stream->started_.load(std::memory_order_acquire)) {
        dispatch_sync(stream->callback_queue_, ^{});
        state->discard_pending(*stream);
        return SACCADE_OK;
    }
    __block NSError* failure = nil;
    dispatch_semaphore_t complete = dispatch_semaphore_create(0);
    [stream->stream_ stopCaptureWithCompletionHandler:^(NSError* error) {
      failure = error;
      dispatch_semaphore_signal(complete);
    }];
    if (dispatch_semaphore_wait(complete, wait_time(cold_wait_ns)) != 0 || failure != nil) {
        stream->stats_.stop_failures.fetch_add(1, std::memory_order_relaxed);
        return SACCADE_ERROR_BACKEND;
    }
    stream->started_.store(false, std::memory_order_release);
    dispatch_sync(stream->callback_queue_, ^{});
    state->discard_pending(*stream);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL acquire_frame(void* context, SaccadeCaptureStreamHandle handle, uint64_t timeout_ns,
                                         SaccadeCapturedFrame* output) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!valid_output(output)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!stream->started_.load(std::memory_order_acquire)) return SACCADE_ERROR_STATE;
    if (stream->failed_.load(std::memory_order_acquire)) return SACCADE_ERROR_BACKEND;

    uint32_t pending = stream->pending_.exchange(0, std::memory_order_acq_rel);
    if (pending == 0 && timeout_ns != 0) {
        (void)dispatch_semaphore_wait(stream->available_, wait_time(timeout_ns));
        if (stream->failed_.load(std::memory_order_acquire)) return SACCADE_ERROR_BACKEND;
        pending = stream->pending_.exchange(0, std::memory_order_acq_rel);
    }
    if (pending == 0) {
        return timeout_ns == 0 ? SACCADE_ERROR_BUSY : SACCADE_ERROR_TIMEOUT;
    }

    Impl::Frame& frame = stream->frames_[pending - 1U];
    frame.state_.store(frame_acquired, std::memory_order_release);
    SaccadeCapturedFrame value{};
    value.frame = frame_handle(stream->slot_, pending - 1U, frame.generation_);
    value.source_id = stream->source_id_;
    value.frame_id = frame.frame_id_;
    value.transform_epoch = frame.transform_epoch_;
    value.timestamp_ns = frame.timestamp_ns_;
    value.width = frame.width_;
    value.height = frame.height_;
    value.pixel_format = frame.pixel_format_;
    value.damage_count = frame.damage_count_;
    const SaccadeResult result = write_output(output, value);
    if (result != SACCADE_OK) {
        state->update_imported(*stream, frame.imported_bytes_, 0);
        state->release_frame(frame);
        return result;
    }
    stream->stats_.acquired.fetch_add(1, std::memory_order_relaxed);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL copy_damage(void* context, SaccadeCaptureStreamHandle handle,
                                       SaccadeFrameHandle frame_handle_value, SaccadeRectI32* output, uint32_t capacity,
                                       uint32_t* required) {
    Impl* state = provider(context);
    const Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr || required == nullptr) {
        return stream == nullptr ? SACCADE_ERROR_STALE_HANDLE : SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t stream_slot = 0;
    uint32_t frame_slot = 0;
    uint32_t generation = 0;
    if (!decode_frame(frame_handle_value, &stream_slot, &frame_slot, &generation) || stream_slot != stream->slot_) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const Impl::Frame& frame = stream->frames_[frame_slot];
    if (frame.generation_ != generation || frame.state_.load(std::memory_order_acquire) != frame_acquired) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *required = frame.damage_count_;
    if (capacity < frame.damage_count_) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (frame.damage_count_ != 0 && output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::copy_n(frame.damage_.data(), frame.damage_count_, output);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL release_frame(void* context, SaccadeCaptureStreamHandle handle,
                                         SaccadeFrameHandle frame_handle_value) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    uint32_t stream_slot = 0;
    uint32_t frame_slot = 0;
    uint32_t generation = 0;
    if (!decode_frame(frame_handle_value, &stream_slot, &frame_slot, &generation) || stream_slot != stream->slot_) {
        stream->stats_.stale_releases.fetch_add(1, std::memory_order_relaxed);
        return SACCADE_ERROR_STALE_HANDLE;
    }
    Impl::Frame& frame = stream->frames_[frame_slot];
    if (frame.generation_ != generation || frame.state_.load(std::memory_order_acquire) != frame_acquired) {
        stream->stats_.stale_releases.fetch_add(1, std::memory_order_relaxed);
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->update_imported(*stream, frame.imported_bytes_, 0);
    state->release_frame(frame);
    stream->stats_.released.fetch_add(1, std::memory_order_relaxed);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL synchronize_capture(void* context, SaccadeCaptureStreamHandle handle, uint64_t timeout_ns) {
    Impl* state = provider(context);
    Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        bool busy = stream->pending_.load(std::memory_order_acquire) != 0;
        for (const Impl::Frame& frame : stream->frames_) {
            busy = busy || frame.state_.load(std::memory_order_acquire) != frame_free;
        }
        if (!busy) {
            return SACCADE_OK;
        }
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start >= timeout_ns) {
            return SACCADE_ERROR_TIMEOUT;
        }
        sched_yield();
    }
}

SaccadeResult SACCADE_CALL capture_memory(void* context, SaccadeCaptureStreamHandle handle,
                                          SaccadeMemoryStats* output) {
    Impl* state = provider(context);
    const Impl::Stream* stream = state != nullptr ? state->find_stream(handle) : nullptr;
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeMemoryStats value{};
    value.host_committed = sizeof(Impl::Stream);
    value.host_reserved = sizeof(Impl::Stream);
    value.device_imported = stream->stats_.imported_bytes.load(std::memory_order_relaxed);
    value.high_water_bytes = stream->stats_.imported_high_water.load(std::memory_order_relaxed);
    return write_output(output, value);
}

} // namespace

ScreenCaptureProvider::ScreenCaptureProvider() noexcept = default;

ScreenCaptureProvider::~ScreenCaptureProvider() {
    if (initialized_) {
        impl().~Impl();
    }
}

ScreenCaptureProvider::Impl& ScreenCaptureProvider::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const ScreenCaptureProvider::Impl& ScreenCaptureProvider::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult ScreenCaptureProvider::initialize(void* metal_device) noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    if (metal_device == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
    Impl* state = ::new (static_cast<void*>(storage_.data())) Impl(device);
    const CVReturn result =
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &state->texture_cache_);
    if (result != kCVReturnSuccess || state->texture_cache_ == nullptr) {
        state->~Impl();
        std::memset(storage_.data(), 0, storage_.size());
        return SACCADE_ERROR_BACKEND;
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeCaptureProviderDesc ScreenCaptureProvider::descriptor() noexcept {
    SaccadeCaptureProviderDesc desc{};
    if (!initialized_) {
        return desc;
    }
    Impl& state = impl();
    static const uint8_t name[] = "ScreenCaptureKit";
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info.struct_size = static_cast<uint32_t>(sizeof(desc.info));
    desc.info.api_version = SACCADE_API_VERSION;
    desc.info.family = SACCADE_PROVIDER_FAMILY_CAPTURE;
    desc.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_DAMAGE;
    desc.info.stable_id = provider_id;
    desc.info.name = {name, sizeof(name) - 1};
    desc.context = &state;
    desc.ops.struct_size = static_cast<uint32_t>(sizeof(desc.ops));
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
    desc.ops.memory_stats = capture_memory;
    return desc;
}

SaccadeResult ScreenCaptureProvider::read_stats(SaccadeCaptureStreamHandle handle,
                                                ScreenCaptureStats* output) const noexcept {
    if (!initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Stream* stream = impl().find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *output = stream->stats_.read();
    return SACCADE_OK;
}

SaccadeResult ScreenCaptureProvider::read_native_frame(SaccadeCaptureStreamHandle handle,
                                                       SaccadeFrameHandle frame_handle_value,
                                                       NativeCapturedFrame* output) const noexcept {
    if (!initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    const Impl::Stream* stream = impl().find_stream(handle);
    if (stream == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    uint32_t stream_slot = 0;
    uint32_t frame_slot = 0;
    uint32_t generation = 0;
    if (!decode_frame(frame_handle_value, &stream_slot, &frame_slot, &generation) || stream_slot != stream->slot_) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const Impl::Frame& frame = stream->frames_[frame_slot];
    if (frame.generation_ != generation || frame.state_.load(std::memory_order_acquire) != frame_acquired) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    output->pixel_buffer = frame.pixel_buffer_;
    output->iosurface = frame.iosurface_;
    output->metal_texture = (__bridge void*)CVMetalTextureGetTexture(frame.metal_texture_);
    output->iosurface_id = frame.iosurface_id_;
    output->pixel_format = frame.pixel_format_;
    output->width = frame.width_;
    output->height = frame.height_;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

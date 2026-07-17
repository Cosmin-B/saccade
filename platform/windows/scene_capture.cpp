#include "platform/windows/scene_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>

namespace saccade::platform::windows {
namespace {

constexpr uint64_t display_source_prefix = UINT64_C(1) << 56U;
constexpr uint64_t source_value_mask = UINT64_C(0x00FFFFFFFFFFFFFF);

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

uint64_t display_source_id(uint64_t display_id) noexcept {
    return display_source_prefix | (display_id & source_value_mask);
}

uint32_t next_generation(uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1U : value;
}

bool snapshot_valid(const geometry::DisplaySnapshot& snapshot) noexcept {
    if (snapshot.epoch == 0 || snapshot.count > geometry::display_capacity) {
        return false;
    }
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        if (snapshot.displays[index].display_id == 0) return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (snapshot.displays[previous].display_id == snapshot.displays[index].display_id) return false;
        }
    }
    return true;
}

bool snapshot_contains(const geometry::DisplaySnapshot& snapshot, uint64_t display_id) noexcept {
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        if (snapshot.displays[index].display_id == display_id) return true;
    }
    return false;
}

} // namespace

SceneCaptureSet::~SceneCaptureSet() {
    if (!initialized_ || !owns_thread()) return;
    for (StreamSlot& slot : streams_) {
        if (slot.active_ && !slot.leased_) (void)remove(slot);
    }
}

bool SceneCaptureSet::owns_thread() const noexcept {
    return initialized_ && owner_thread_ == GetCurrentThreadId();
}

SceneCaptureSet::StreamSlot* SceneCaptureSet::find(uint64_t display_id) noexcept {
    for (StreamSlot& slot : streams_) {
        if (slot.active_ && slot.display_id_ == display_id) return &slot;
    }
    return nullptr;
}

const SceneCaptureSet::StreamSlot* SceneCaptureSet::find(uint64_t display_id) const noexcept {
    for (const StreamSlot& slot : streams_) {
        if (slot.active_ && slot.display_id_ == display_id) return &slot;
    }
    return nullptr;
}

SaccadeResult SceneCaptureSet::initialize(ScreenCaptureProvider* provider, uint32_t max_width,
                                          uint32_t max_height) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (provider == nullptr || (max_width == 0) != (max_height == 0)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (max_width != 0) return SACCADE_ERROR_UNSUPPORTED;
    const SaccadeCaptureProviderDesc descriptor = provider->descriptor();
    if (descriptor.context == nullptr || descriptor.ops.enumerate_sources == nullptr ||
        descriptor.ops.create == nullptr || descriptor.ops.start == nullptr || descriptor.ops.acquire == nullptr ||
        descriptor.ops.release == nullptr || descriptor.ops.stop == nullptr || descriptor.ops.destroy == nullptr) {
        return SACCADE_ERROR_STATE;
    }
    provider_ = provider;
    backend_ = descriptor;
    max_width_ = max_width;
    max_height_ = max_height;
    owner_thread_ = GetCurrentThreadId();
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::remove(StreamSlot& slot) noexcept {
    if (slot.leased_) return SACCADE_ERROR_BUSY;
    if (slot.running_) {
        const SaccadeResult stopped = backend_.ops.stop(backend_.context, slot.stream_);
        if (stopped != SACCADE_OK && stopped != SACCADE_ERROR_STATE) return stopped;
    }
    const SaccadeResult destroyed = backend_.ops.destroy(backend_.context, slot.stream_);
    if (destroyed != SACCADE_OK) return destroyed;
    slot.display_id_ = 0;
    slot.source_id_ = 0;
    slot.stream_ = 0;
    slot.active_ = false;
    slot.running_ = false;
    slot.generation_ = next_generation(slot.generation_);
    ++stats_.streams_removed;
    --stats_.active_streams;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::synchronize(const geometry::DisplaySnapshot& snapshot) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    ++stats_.synchronize_calls;
    if (!snapshot_valid(snapshot) || snapshot.epoch < stats_.topology_epoch) {
        ++stats_.failures;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (snapshot.epoch == stats_.topology_epoch) return SACCADE_OK;
    SaccadeCaptureSourceInfo first = output_structure<SaccadeCaptureSourceInfo>();
    const SaccadeResult refreshed = backend_.ops.enumerate_sources(backend_.context, 0, &first);
    if (refreshed != SACCADE_OK) {
        ++stats_.failures;
        return refreshed;
    }
    for (const StreamSlot& slot : streams_) {
        if (slot.active_ && slot.leased_ && !snapshot_contains(snapshot, slot.display_id_)) {
            ++stats_.failures;
            return SACCADE_ERROR_BUSY;
        }
    }
    for (StreamSlot& slot : streams_) {
        if (slot.active_ && !snapshot_contains(snapshot, slot.display_id_)) {
            const SaccadeResult removed = remove(slot);
            if (removed != SACCADE_OK) {
                ++stats_.failures;
                return removed;
            }
        }
    }
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const uint64_t display_id = snapshot.displays[index].display_id;
        if (find(display_id) != nullptr) continue;
        StreamSlot* free_slot = nullptr;
        for (StreamSlot& slot : streams_) {
            if (!slot.active_) {
                free_slot = &slot;
                break;
            }
        }
        if (free_slot == nullptr) {
            ++stats_.failures;
            return SACCADE_ERROR_CAPACITY;
        }
        SaccadeCaptureStreamDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.api_version = SACCADE_API_VERSION;
        desc.source_id = display_source_id(display_id);
        desc.pixel_format = SACCADE_FORMAT_BGRA8;
        desc.queue_capacity = 3;
        desc.max_width = max_width_;
        desc.max_height = max_height_;
        SaccadeCaptureStreamHandle stream = 0;
        SaccadeResult result = backend_.ops.create(backend_.context, &desc, &stream);
        if (result == SACCADE_OK && running_) {
            result = backend_.ops.start(backend_.context, stream);
        }
        if (result != SACCADE_OK) {
            if (stream != 0) (void)backend_.ops.destroy(backend_.context, stream);
            ++stats_.failures;
            return result;
        }
        free_slot->display_id_ = display_id;
        free_slot->source_id_ = desc.source_id;
        free_slot->stream_ = stream;
        free_slot->active_ = true;
        free_slot->running_ = running_;
        ++stats_.streams_added;
        ++stats_.active_streams;
    }
    stats_.topology_epoch = snapshot.epoch;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::set_running(bool enabled) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (running_ == enabled) return SACCADE_OK;
    if (!enabled) {
        for (const StreamSlot& slot : streams_) {
            if (slot.active_ && slot.leased_) return SACCADE_ERROR_BUSY;
        }
    }
    uint32_t changed = 0;
    for (StreamSlot& slot : streams_) {
        if (!slot.active_) continue;
        const SaccadeResult result = enabled ? backend_.ops.start(backend_.context, slot.stream_)
                                             : backend_.ops.stop(backend_.context, slot.stream_);
        if (result != SACCADE_OK) {
            for (StreamSlot& rollback : streams_) {
                if (!rollback.active_ || changed == 0) continue;
                (void)(enabled ? backend_.ops.stop(backend_.context, rollback.stream_)
                               : backend_.ops.start(backend_.context, rollback.stream_));
                rollback.running_ = !enabled;
                --changed;
            }
            ++stats_.failures;
            return result;
        }
        slot.running_ = enabled;
        ++changed;
    }
    running_ = enabled;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::acquire(uint64_t display_id, SceneCaptureFrame* output) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (output == nullptr || display_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    StreamSlot* slot = find(display_id);
    if (slot == nullptr) return SACCADE_ERROR_NOT_FOUND;
    if (!slot->running_) return SACCADE_ERROR_STATE;
    if (slot->leased_) return SACCADE_ERROR_BUSY;
    SaccadeCapturedFrame frame = output_structure<SaccadeCapturedFrame>();
    const SaccadeResult acquired = backend_.ops.acquire(backend_.context, slot->stream_, 0, &frame);
    if (acquired == SACCADE_ERROR_BUSY) {
        ++stats_.empty_acquires;
        return acquired;
    }
    if (acquired != SACCADE_OK) {
        ++stats_.failures;
        return acquired;
    }
    NativeCapturedFrame native{};
    const SaccadeResult read = provider_->read_native_frame(slot->stream_, frame.frame, &native);
    if (read != SACCADE_OK) {
        (void)backend_.ops.release(backend_.context, slot->stream_, frame.frame);
        ++stats_.failures;
        return read;
    }
    slot->leased_ = true;
    ++stats_.leased_frames;
    ++stats_.frames_acquired;
    output->frame = frame;
    output->native = native;
    output->display_id = display_id;
    output->topology_epoch = stats_.topology_epoch;
    output->slot = static_cast<uint32_t>(slot - streams_.data());
    output->generation = slot->generation_;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::release(const SceneCaptureFrame& frame) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (frame.slot >= streams_.size()) return SACCADE_ERROR_STALE_HANDLE;
    StreamSlot& slot = streams_[frame.slot];
    if (!slot.active_ || !slot.leased_ || slot.generation_ != frame.generation ||
        slot.display_id_ != frame.display_id) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult released = backend_.ops.release(backend_.context, slot.stream_, frame.frame.frame);
    if (released != SACCADE_OK) {
        ++stats_.failures;
        return released;
    }
    slot.leased_ = false;
    --stats_.leased_frames;
    ++stats_.frames_released;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::shutdown() noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    for (const StreamSlot& slot : streams_) {
        if (slot.active_ && slot.leased_) return SACCADE_ERROR_BUSY;
    }
    for (StreamSlot& slot : streams_) {
        if (!slot.active_) continue;
        const SaccadeResult result = remove(slot);
        if (result != SACCADE_OK) return result;
    }
    provider_ = nullptr;
    backend_ = {};
    max_width_ = 0;
    max_height_ = 0;
    owner_thread_ = 0;
    initialized_ = false;
    running_ = false;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::display_at(uint32_t index, uint64_t* output) const noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t active_index = 0;
    for (const StreamSlot& slot : streams_) {
        if (!slot.active_) continue;
        if (active_index++ == index) {
            *output = slot.display_id_;
            return SACCADE_OK;
        }
    }
    return SACCADE_ERROR_NOT_FOUND;
}

SaccadeResult SceneCaptureSet::read_stats(SceneCaptureStats* output) const noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = stats_;
    return SACCADE_OK;
}

SaccadeResult SceneCaptureSet::read_memory_stats(SaccadeMemoryStats* output) const noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = SACCADE_API_VERSION;
    for (const StreamSlot& slot : streams_) {
        if (!slot.active_) continue;
        SaccadeMemoryStats current{};
        current.struct_size = sizeof(current);
        current.api_version = SACCADE_API_VERSION;
        const SaccadeResult result = backend_.ops.memory_stats(backend_.context, slot.stream_, &current);
        if (result != SACCADE_OK) return result;
        output->host_committed += current.host_committed;
        output->host_reserved += current.host_reserved;
        output->device_imported += current.device_imported;
        output->device_owned += current.device_owned;
        output->framework_opaque += current.framework_opaque;
        output->copied_bytes += current.copied_bytes;
        output->high_water_bytes += current.high_water_bytes;
    }
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

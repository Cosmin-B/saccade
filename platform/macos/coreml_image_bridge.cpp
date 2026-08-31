#include "platform/macos/coreml_image_bridge.hpp"

#include "geometry/scope_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace saccade::platform::macos {
namespace {

bool q8(uint32_t value, int32_t* output) noexcept {
    if (output == nullptr || value > (static_cast<uint32_t>(INT32_MAX) >> 8U)) {
        return false;
    }
    *output = static_cast<int32_t>(value << 8U);
    return true;
}

bool frame_matches_display(const SceneCaptureFrame& frame, const geometry::DisplaySurface& display) noexcept {
    return frame.frame.frame != 0 && frame.frame.frame_id != 0 && frame.frame.source_id != 0 && frame.frame.transform_epoch != 0 &&
           frame.native.metal_texture != nullptr && frame.native.iosurface_id != 0 && frame.native.pixel_format == SACCADE_FORMAT_BGRA8 &&
           frame.native.width == frame.frame.width && frame.native.height == frame.frame.height && frame.display_id != 0 &&
           frame.display_id == display.display_id && frame.topology_epoch != 0 && geometry::rect_valid(display.desktop_bounds);
}

bool same_rect(const geometry::RectQ8& left, const geometry::RectQ8& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

} // namespace

CoreMlImageBridge::~CoreMlImageBridge() {
    (void)shutdown();
}

SaccadeResult CoreMlImageBridge::initialize(CoreMlImageBridgeConfig config) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.runtime == 0 || config.metal_device == nullptr || config.metallib_path == nullptr || config.metallib_path[0] == '\0' ||
        config.input_width == 0 || config.input_height == 0 || config.reserved != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    backend::metal::TensorSpec spec{};
    spec.width = config.input_width;
    spec.height = config.input_height;
    spec.format = backend::metal::TensorFormat::image_bgra8;
    spec.letterbox_rgb = config.letterbox_rgb;
    const SaccadeResult result = preprocessor_.initialize(config.metal_device, config.metallib_path, config.path, spec);
    if (result != SACCADE_OK)
        return result;
    config_ = config;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult CoreMlImageBridge::begin(SceneCaptureSet* captures, const SceneCaptureFrame& frame,
                                       const geometry::DisplaySurface& display) noexcept {
    return begin_scope(captures, &frame, &display, 1, display.desktop_bounds, frame.frame.source_id);
}

SaccadeResult CoreMlImageBridge::begin_scope(SceneCaptureSet* captures, const SceneCaptureFrame* frames,
                                             const geometry::DisplaySurface* displays, uint32_t display_count, geometry::RectQ8 scope,
                                             uint64_t source_id) noexcept {
    static_assert(backend::metal::atlas_source_capacity == geometry::display_capacity);
    if (!initialized_ || frames == nullptr || displays == nullptr || display_count == 0 ||
        display_count > geometry::display_capacity || !geometry::rect_valid(scope) || source_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (preprocessing_ || output_in_use_) {
        ++stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }

    std::array<geometry::AtlasSurface, geometry::display_capacity> surfaces{};
    uint64_t topology_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t transform_epoch = 0;
    for (uint32_t index = 0; index < display_count; ++index) {
        if (captures == nullptr && frames[index].release == nullptr)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!frame_matches_display(frames[index], displays[index]) ||
            (topology_epoch != 0 && frames[index].topology_epoch != topology_epoch)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        topology_epoch = frames[index].topology_epoch;
        frame_id = std::max(frame_id, frames[index].frame.frame_id);
        transform_epoch = std::max(transform_epoch, frames[index].frame.transform_epoch);
        surfaces[index] = {displays[index].desktop_bounds, frames[index].native.width, frames[index].native.height};
    }

    geometry::ScopeAtlasLayout layout{};
    SaccadeResult result =
        geometry::make_scope_atlas_layout(scope, config_.input_width, config_.input_height, surfaces.data(), display_count, &layout);
    if (result != SACCADE_OK)
        return result;

    std::array<backend::metal::AtlasSource, backend::metal::atlas_source_capacity> atlas_sources{};
    for (uint32_t index = 0; index < layout.count; ++index) {
        const geometry::AtlasPlacement& placement = layout.placements[index];
        const SceneCaptureFrame& frame = frames[placement.surface_index];
        atlas_sources[index] = {
            frame.native.metal_texture,
            frame.native.width,
            frame.native.height,
            {placement.source.x, placement.source.y, placement.source.width, placement.source.height},
            {placement.destination.x, placement.destination.y, placement.destination.width, placement.destination.height},
        };
    }

    if (next_output_frame_id_ == std::numeric_limits<uint64_t>::max())
        return SACCADE_ERROR_CAPACITY;
    const bool preserve_atlas = atlas_matches(scope, topology_epoch);
    auto capture_stamps = preserve_atlas ? atlas_capture_stamps_ : std::array<AtlasCaptureStamp, geometry::display_capacity>{};
    uint32_t capture_stamp_count = preserve_atlas ? atlas_capture_stamp_count_ : 0;
    for (uint32_t index = 0; index < display_count; ++index) {
        uint32_t stamp_index = 0;
        while (stamp_index < capture_stamp_count && capture_stamps[stamp_index].display_id != displays[index].display_id)
            ++stamp_index;
        if (stamp_index == capture_stamp_count) {
            if (capture_stamp_count == capture_stamps.size())
                return SACCADE_ERROR_CAPACITY;
            ++capture_stamp_count;
        }
        capture_stamps[stamp_index] = {displays[index].display_id, frames[index].frame.timestamp_ns};
    }
    uint64_t atlas_capture_time_ns = std::numeric_limits<uint64_t>::max();
    for (uint32_t index = 0; index < capture_stamp_count; ++index)
        atlas_capture_time_ns = std::min(atlas_capture_time_ns, capture_stamps[index].capture_time_ns);

    backend::metal::PreprocessSubmission submission{};
    const backend::metal::AtlasLoad load = preserve_atlas ? backend::metal::AtlasLoad::preserve : backend::metal::AtlasLoad::clear;
    result = preprocessor_.submit_atlas(atlas_sources.data(), layout.count,
                                        {layout.content.x, layout.content.y, layout.content.width, layout.content.height}, load, frame_id,
                                        transform_epoch, &submission);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }
    capture_set_ = captures;
    std::copy_n(frames, display_count, capture_frames_.begin());
    capture_count_ = display_count;
    scope_ = scope;
    source_id_ = source_id;
    topology_epoch_ = topology_epoch;
    frame_id_ = next_output_frame_id_++;
    transform_epoch_ = transform_epoch;
    capture_time_ns_ = atlas_capture_time_ns;
    atlas_capture_stamps_ = capture_stamps;
    atlas_capture_stamp_count_ = capture_stamp_count;
    atlas_capture_time_ns_ = atlas_capture_time_ns;
    atlas_scope_ = scope;
    atlas_topology_epoch_ = topology_epoch;
    atlas_ready_ = true;
    submission_ = submission;
    replay_pending_ = false;
    preprocessing_ = true;
    ++stats_.submissions;
    return SACCADE_OK;
}

SaccadeResult CoreMlImageBridge::begin_cached() noexcept {
    if (!initialized_ || !atlas_ready_ || atlas_capture_time_ns_ == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (preprocessing_ || output_in_use_) {
        ++stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    if (next_output_frame_id_ == std::numeric_limits<uint64_t>::max())
        return SACCADE_ERROR_CAPACITY;
    frame_id_ = next_output_frame_id_++;
    capture_time_ns_ = atlas_capture_time_ns_;
    replay_pending_ = true;
    preprocessing_ = true;
    ++stats_.submissions;
    ++stats_.cached_replays;
    return SACCADE_OK;
}

bool CoreMlImageBridge::atlas_matches(geometry::RectQ8 scope, uint64_t topology_epoch) const noexcept {
    return atlas_ready_ && topology_epoch != 0 && topology_epoch == atlas_topology_epoch_ && same_rect(scope, atlas_scope_);
}

SaccadeResult CoreMlImageBridge::release_captures() noexcept {
    if (capture_set_ == nullptr && capture_count_ == 0)
        return SACCADE_OK;
    for (uint32_t index = 0; index < capture_count_; ++index) {
        SceneCaptureFrame& frame = capture_frames_[index];
        if (frame.frame.frame == 0)
            continue;
        const SaccadeResult result = frame.release != nullptr ? frame.release(frame.release_context, frame)
                                                              : capture_set_->release(frame);
        if (result != SACCADE_OK)
            return result;
        frame = {};
        ++stats_.capture_releases;
    }
    capture_set_ = nullptr;
    capture_count_ = 0;
    return SACCADE_OK;
}

SaccadeResult CoreMlImageBridge::poll(scheduler::NeuralFrame* output, bool* ready) noexcept {
    if (!initialized_ || output == nullptr || ready == nullptr || !preprocessing_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    *ready = false;
    SaccadeResult result = SACCADE_OK;
    if (!replay_pending_) {
        bool complete = false;
        result = preprocessor_.poll(submission_, &complete);
        if (result != SACCADE_OK) {
            ++stats_.failures;
            return result;
        }
        if (!complete)
            return SACCADE_OK;
    }

    backend::metal::ImageView image{};
    result = preprocessor_.image(submission_, &image);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }
    geometry::TransformDesc transform{};
    if (!q8(image.content.x, &transform.source.x) || !q8(image.content.y, &transform.source.y) ||
        !q8(image.content.width, &transform.source.width) || !q8(image.content.height, &transform.source.height)) {
        ++stats_.failures;
        return SACCADE_ERROR_CAPACITY;
    }
    transform.destination = scope_;
    transform.epoch = transform_epoch_;
    transform.source_space = geometry::CoordinateSpace::capture;
    transform.destination_space = geometry::CoordinateSpace::desktop;
    geometry::CoordinateTransform source_to_desktop;
    result = source_to_desktop.initialize(transform);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }

    SaccadeIOSurfaceFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.iosurface_id = image.iosurface_id;
    frame.pixel_format = image.pixel_format;
    frame.width = image.width;
    frame.height = image.height;
    frame.frame_id = frame_id_;
    frame.transform_epoch = transform_epoch_;
    SaccadeFrameHandle imported = 0;
    result = saccade_frame_import_iosurface(config_.runtime, &frame, &imported);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }
    result = release_captures();
    if (result != SACCADE_OK) {
        (void)saccade_frame_release(config_.runtime, imported);
        ++stats_.failures;
        return result;
    }
    output->frame = imported;
    output->source_id = source_id_;
    output->topology_epoch = topology_epoch_;
    output->transform_epoch = frame.transform_epoch;
    output->capture_time_ns = capture_time_ns_;
    output->width = frame.width;
    output->height = frame.height;
    output->source_to_desktop = source_to_desktop;
    output->retire_context = this;
    output->retire = &CoreMlImageBridge::retire_callback;
    output_frame_ = imported;
    preprocessing_ = false;
    replay_pending_ = false;
    output_in_use_ = true;
    ++stats_.completions;
    ++stats_.runtime_imports;
    *ready = true;
    return SACCADE_OK;
}

void CoreMlImageBridge::retire(SaccadeFrameHandle frame) noexcept {
    if (!output_in_use_ || frame != output_frame_) {
        ++stats_.failures;
        return;
    }
    output_frame_ = 0;
    output_in_use_ = false;
    ++stats_.output_retires;
}

void CoreMlImageBridge::retire_callback(void* context, SaccadeFrameHandle frame) noexcept {
    if (context != nullptr) {
        static_cast<CoreMlImageBridge*>(context)->retire(frame);
    }
}

SaccadeResult CoreMlImageBridge::discard() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    if (output_in_use_)
        return SACCADE_ERROR_BUSY;
    if (!preprocessing_)
        return SACCADE_OK;
    SaccadeResult result = replay_pending_ ? SACCADE_OK : preprocessor_.wait(submission_, UINT64_MAX);
    if (result != SACCADE_OK)
        return result;
    scheduler::NeuralFrame frame{};
    bool ready = false;
    result = poll(&frame, &ready);
    if (result != SACCADE_OK)
        return result;
    if (!ready)
        return SACCADE_ERROR_BACKEND;
    result = saccade_frame_release(config_.runtime, frame.frame);
    if (result == SACCADE_OK)
        retire(frame.frame);
    return result;
}

SaccadeResult CoreMlImageBridge::read_memory_stats(SaccadeMemoryStats* output) const noexcept {
    return initialized_ ? preprocessor_.memory_stats(output) : SACCADE_ERROR_STATE;
}

SaccadeResult CoreMlImageBridge::shutdown() noexcept {
    if (!initialized_)
        return SACCADE_OK;
    if (output_in_use_)
        return SACCADE_ERROR_BUSY;
    SaccadeResult result = SACCADE_OK;
    if (preprocessing_) {
        result = replay_pending_ ? SACCADE_OK : preprocessor_.wait(submission_, UINT64_C(1'000'000'000));
        if (result != SACCADE_OK)
            return result;
        const SaccadeResult released = release_captures();
        result = released;
    }
    preprocessing_ = false;
    replay_pending_ = false;
    atlas_ready_ = false;
    atlas_capture_stamps_ = {};
    atlas_capture_stamp_count_ = 0;
    atlas_capture_time_ns_ = 0;
    atlas_scope_ = {};
    atlas_topology_epoch_ = 0;
    next_output_frame_id_ = 1;
    initialized_ = false;
    return result;
}

} // namespace saccade::platform::macos

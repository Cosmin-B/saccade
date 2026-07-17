#include "platform/windows/neural_bridge.hpp"

#include <cstdint>

namespace saccade::platform::windows {
namespace {

bool q8(uint32_t value, int32_t* output) noexcept {
    if (value > (static_cast<uint32_t>(INT32_MAX) >> 8U)) return false;
    *output = static_cast<int32_t>(value << 8U);
    return true;
}

bool frame_matches_display(const SceneCaptureFrame& frame, const geometry::DisplaySurface& display,
                           uint64_t scene_transform_epoch) noexcept {
    return frame.frame.frame != 0 && frame.frame.frame_id != 0 && frame.frame.source_id != 0 &&
           frame.frame.transform_epoch != 0 && frame.native.d3d11_texture != nullptr &&
           frame.native.pixel_format == SACCADE_FORMAT_BGRA8 && frame.native.width >= frame.frame.width &&
           frame.native.height >= frame.frame.height && frame.display_id == display.display_id &&
           frame.topology_epoch != 0 && scene_transform_epoch != 0 && geometry::rect_valid(display.desktop_bounds);
}

} // namespace

NeuralBridge::~NeuralBridge() {
    (void)shutdown();
}

SaccadeResult NeuralBridge::initialize(SaccadeRuntimeHandle runtime) noexcept {
    if (runtime_ != 0) return SACCADE_ERROR_ALREADY_EXISTS;
    if (runtime == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    runtime_ = runtime;
    return SACCADE_OK;
}

SaccadeResult NeuralBridge::initialize(const NeuralBridgeConfig& config) noexcept {
    if (runtime_ != 0) return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.runtime == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result =
        transfer_.initialize(config.producer_device, config.producer_context, config.consumer_device);
    if (result != SACCADE_OK) return result;
    runtime_ = config.runtime;
    transfer_initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult NeuralBridge::import(SceneCaptureSet* captures, const SceneCaptureFrame& capture,
                                   const geometry::DisplaySurface& display, uint64_t scene_transform_epoch,
                                   scheduler::DesktopNeuralFrame* output) noexcept {
    if (runtime_ == 0 || captures == nullptr || output == nullptr ||
        !frame_matches_display(capture, display, scene_transform_epoch)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    RetirementSlot* retirement = nullptr;
    for (RetirementSlot& slot : retirements_) {
        if (slot.frame == 0) {
            retirement = &slot;
            break;
        }
    }
    if (retirement == nullptr) {
        ++stats_.failures;
        return SACCADE_ERROR_CAPACITY;
    }
    geometry::TransformDesc transform{};
    if (!q8(capture.frame.width, &transform.source.width) || !q8(capture.frame.height, &transform.source.height)) {
        ++stats_.failures;
        return SACCADE_ERROR_CAPACITY;
    }
    transform.destination = display.desktop_bounds;
    transform.epoch = capture.frame.transform_epoch;
    transform.source_space = geometry::CoordinateSpace::capture;
    transform.destination_space = geometry::CoordinateSpace::desktop;
    geometry::CoordinateTransform source_to_desktop;
    SaccadeResult result = source_to_desktop.initialize(transform);
    if (result != SACCADE_OK) {
        ++stats_.failures;
        return result;
    }
    SaccadeWin32CaptureFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.texture = capture.native.d3d11_texture;
    frame.subresource = capture.native.subresource;
    frame.pixel_format = capture.native.pixel_format;
    frame.width = capture.frame.width;
    frame.height = capture.frame.height;
    frame.frame_id = capture.frame.frame_id;
    frame.transform_epoch = capture.frame.transform_epoch;
    D3d12CaptureTransferFrame transfer{};
    if (transfer_initialized_) {
        result = transfer_.copy(static_cast<ID3D11Texture2D*>(capture.native.d3d11_texture), frame.width, frame.height,
                                &transfer);
        if (result != SACCADE_OK) {
            ++stats_.failures;
            return result;
        }
        frame.texture = transfer.texture;
        frame.ready_fence = transfer.ready_fence;
        frame.ready_value = transfer.ready_value;
    }
    SaccadeFrameHandle imported = 0;
    result = saccade_frame_import_win32_capture(runtime_, &frame, &imported);
    if (result != SACCADE_OK) {
        if (transfer.texture != nullptr) (void)transfer_.release(transfer);
        ++stats_.failures;
        return result;
    }
    *retirement = {this, captures, capture, transfer, imported};
    output->frame = imported;
    output->source_id = capture.frame.source_id;
    output->topology_epoch = capture.topology_epoch;
    output->transform_epoch = capture.frame.transform_epoch;
    output->capture_time_ns = capture.frame.timestamp_ns;
    output->scene_transform_epoch = scene_transform_epoch;
    output->width = frame.width;
    output->height = frame.height;
    output->source_to_desktop = source_to_desktop;
    output->retire_context = retirement;
    output->retire = retire_callback;
    ++stats_.imports;
    if (transfer.texture != nullptr) {
        const D3d12CaptureTransferStats transfer_stats = transfer_.stats();
        stats_.transfer_copies = transfer_stats.copies;
        stats_.copied_bytes = transfer_stats.copied_bytes;
    }
    return SACCADE_OK;
}

void NeuralBridge::retire(RetirementSlot* retirement, SaccadeFrameHandle frame) noexcept {
    if (retirement == nullptr || retirement->owner != this || retirement->captures == nullptr ||
        retirement->frame != frame) {
        ++stats_.failures;
        return;
    }
    if (retirement->transfer.texture != nullptr) {
        if (transfer_.release(retirement->transfer) == SACCADE_OK) {
            ++stats_.transfer_releases;
        } else {
            ++stats_.failures;
        }
    }
    if (retirement->captures->release(retirement->capture) == SACCADE_OK) {
        ++stats_.capture_releases;
    } else {
        ++stats_.failures;
    }
    *retirement = {};
}

SaccadeResult NeuralBridge::shutdown() noexcept {
    if (runtime_ == 0) return SACCADE_OK;
    for (const RetirementSlot& retirement : retirements_) {
        if (retirement.frame != 0) return SACCADE_ERROR_BUSY;
    }
    const SaccadeResult result = transfer_initialized_ ? transfer_.shutdown() : SACCADE_OK;
    if (result != SACCADE_OK) return result;
    runtime_ = 0;
    transfer_initialized_ = false;
    return SACCADE_OK;
}

void NeuralBridge::retire_callback(void* context, SaccadeFrameHandle frame) noexcept {
    auto* retirement = static_cast<RetirementSlot*>(context);
    if (retirement != nullptr && retirement->owner != nullptr) {
        retirement->owner->retire(retirement, frame);
    }
}

} // namespace saccade::platform::windows

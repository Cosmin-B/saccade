#include "platform/windows/overlay_surface.hpp"
#include "backends/d3d12/graphics_device.hpp"

#include <wrl/client.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t packet_size =
    sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);

struct CallbackState {
    alignas(8) std::array<uint8_t, packet_size> packet{};
    uint64_t display_id = 0;
    uint32_t loads = 0;
    uint32_t observations = 0;
    SaccadeResult last_result = SACCADE_ERROR_STATE;
    saccade::backend::d3d12::OverlaySubmission last_submission{};
};

void make_packet(CallbackState* state) noexcept {
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = 1;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = 1;
    header.transform_epoch = 1;
    header.targets_offset = sizeof(header);
    header.styles_offset = sizeof(header) + sizeof(SaccadeOverlayTarget);
    std::memcpy(state->packet.data(), &header, sizeof(header));
    SaccadeOverlayTarget target{};
    target.target_id = 1;
    target.x_q3 = 64;
    target.y_q3 = 64;
    target.width_q3 = 256;
    target.height_q3 = 128;
    target.label_x_q3 = 64;
    target.label_y_q3 = 200;
    target.confidence_q16 = UINT16_MAX;
    target.glyphs[0] = 0;
    target.glyphs[1] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyph_count = 1;
    std::memcpy(state->packet.data() + header.targets_offset, &target, sizeof(target));
    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = UINT32_C(0xff3030ff);
    style.label_background_rgba8 = UINT32_C(0x101010e0);
    style.label_foreground_rgba8 = UINT32_C(0xffffffff);
    style.active_fill_rgba8 = UINT32_C(0x20a0ff60);
    style.active_outline_rgba8 = UINT32_C(0x20a0ffff);
    style.target_stroke_q3 = 8;
    style.target_radius_q3 = 8;
    style.label_height_q3 = 80;
    style.label_radius_q3 = 8;
    style.label_padding_x_q3 = 8;
    style.glyph_width_q3 = 40;
    style.glyph_height_q3 = 56;
    style.glyph_advance_q3 = 48;
    style.active_stroke_q3 = 8;
    std::memcpy(state->packet.data() + header.styles_offset, &style, sizeof(style));
}

SaccadeResult load_frame(void* context, uint64_t display_id, SaccadeOverlayFrameDesc* output) noexcept {
    auto* state = static_cast<CallbackState*>(context);
    if (display_id != state->display_id || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    ++state->loads;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = SACCADE_API_VERSION;
    output->scene_epoch = 1;
    output->transform_epoch = 1;
    output->packet = {state->packet.data(), state->packet.size()};
    output->flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
    output->active_target_index = 0;
    return SACCADE_OK;
}

void observe_frame(void* context, uint64_t display_id, SaccadeResult result,
                   const saccade::backend::d3d12::OverlaySubmission* submission) noexcept {
    auto* state = static_cast<CallbackState*>(context);
    if (display_id != state->display_id) {
        return;
    }
    ++state->observations;
    state->last_result = result;
    if (submission != nullptr) {
        state->last_submission = *submission;
    }
}

bool detached_from_console() noexcept {
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE && session != WTSGetActiveConsoleSessionId();
}

void report_native_error(const saccade::platform::windows::OverlaySurface& surface) noexcept {
    int32_t error = 0;
    saccade::platform::windows::OverlaySurfaceNativeStage stage{};
    if (surface.read_last_native_error(&error, &stage) != SACCADE_OK) {
        return;
    }
    std::array<char, 64> text{'S', 'T', 'A', 'G', 'E', '='};
    auto stage_text = std::to_chars(text.data() + 6, text.data() + 30, static_cast<uint32_t>(stage));
    if (stage_text.ec != std::errc{}) {
        return;
    }
    *stage_text.ptr++ = ' ';
    *stage_text.ptr++ = 'H';
    *stage_text.ptr++ = 'R';
    *stage_text.ptr++ = '=';
    *stage_text.ptr++ = '0';
    *stage_text.ptr++ = 'x';
    auto error_text = std::to_chars(stage_text.ptr, text.data() + 62, static_cast<uint32_t>(error), 16);
    if (error_text.ec != std::errc{}) {
        return;
    }
    *error_text.ptr++ = '\n';
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(error_text.ptr - text.data()),
                    &written, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) {
        return 2;
    }
    saccade::backend::d3d12::GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return 77;
    }
    saccade::geometry::DisplaySurface display{};
    display.display_id = 1;
    display.desktop_bounds = {0, 0, 512 * 256, 512 * 256};
    display.work_bounds = display.desktop_bounds;
    display.backing_width = 512;
    display.backing_height = 512;
    display.maximum_fps = 120;
    display.flags = saccade::geometry::display_surface_main | saccade::geometry::display_surface_active;
    CallbackState callbacks{};
    callbacks.display_id = display.display_id;
    make_packet(&callbacks);
    saccade::platform::windows::OverlaySurface surface;
    const SaccadeResult initialized = surface.initialize(display, graphics.device(), graphics.queue(), argv[1],
                                                         {&callbacks, load_frame, observe_frame});
    if (initialized != SACCADE_OK) {
        int32_t error = 0;
        saccade::platform::windows::OverlaySurfaceNativeStage stage{};
        (void)surface.read_last_native_error(&error, &stage);
        const bool session_stage = stage == saccade::platform::windows::OverlaySurfaceNativeStage::capture_exclusion ||
                                   stage == saccade::platform::windows::OverlaySurfaceNativeStage::swapchain ||
                                   stage == saccade::platform::windows::OverlaySurfaceNativeStage::composition_device ||
                                   stage == saccade::platform::windows::OverlaySurfaceNativeStage::composition_target ||
                                   stage == saccade::platform::windows::OverlaySurfaceNativeStage::composition_visual;
        if (detached_from_console() && session_stage) {
            return 77;
        }
        report_native_error(surface);
        return 3;
    }
    saccade::platform::windows::OverlaySurfaceInfo info{};
    if (surface.read_info(&info) != SACCADE_OK || info.window_handle == 0 || info.frame_latency_handle == 0 ||
        info.buffer_count != 3 ||
        (info.flags & (saccade::platform::windows::overlay_surface_initialized |
                       saccade::platform::windows::overlay_surface_click_through |
                       saccade::platform::windows::overlay_surface_nonactivating |
                       saccade::platform::windows::overlay_surface_topmost |
                       saccade::platform::windows::overlay_surface_excluded_from_capture |
                       saccade::platform::windows::overlay_surface_color_managed |
                       saccade::platform::windows::overlay_surface_display_paced)) !=
            (saccade::platform::windows::overlay_surface_initialized |
             saccade::platform::windows::overlay_surface_click_through |
             saccade::platform::windows::overlay_surface_nonactivating |
             saccade::platform::windows::overlay_surface_topmost |
             saccade::platform::windows::overlay_surface_excluded_from_capture |
             saccade::platform::windows::overlay_surface_color_managed |
             saccade::platform::windows::overlay_surface_display_paced)) {
        return 4;
    }
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(info.window_handle));
    DWORD affinity = 0;
    if (GetWindowDisplayAffinity(window, &affinity) == FALSE || affinity != WDA_EXCLUDEFROMCAPTURE ||
        SendMessageW(window, WM_NCHITTEST, 0, 0) != HTTRANSPARENT || surface.set_click_through(false) != SACCADE_OK ||
        SendMessageW(window, WM_NCHITTEST, 0, 0) != HTCLIENT || surface.set_click_through(true) != SACCADE_OK) {
        return 5;
    }
    if (surface.start() != SACCADE_OK) {
        return 6;
    }
    SaccadeResult presented = SACCADE_ERROR_BUSY;
    for (uint32_t attempt = 0; attempt < 10 && presented == SACCADE_ERROR_BUSY; ++attempt) {
        (void)WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(info.frame_latency_handle)), 1000);
        presented = surface.present(static_cast<uint64_t>(attempt) + 1U);
    }
    saccade::platform::windows::OverlaySurfaceStats stats{};
    saccade::backend::d3d12::OverlayStats renderer_stats{};
    saccade::platform::windows::OverlaySurfaceMemoryStats memory{};
    if (presented != SACCADE_OK || callbacks.loads == 0 || callbacks.observations == 0 ||
        callbacks.last_result != SACCADE_OK || callbacks.last_submission.sequence == 0 ||
        surface.read_stats(&stats) != SACCADE_OK || stats.rendered_frames == 0 || stats.presented_frames == 0 ||
        surface.read_renderer_stats(&renderer_stats) != SACCADE_OK || renderer_stats.draw_calls == 0 ||
        surface.read_memory_stats(&memory) != SACCADE_OK ||
        memory.swapchain_bytes_estimate != UINT64_C(512) * 512U * 4U * 3U || surface.stop() != SACCADE_OK) {
        return 7;
    }
    (void)SetThreadDpiAwarenessContext(previous);
    return 0;
}

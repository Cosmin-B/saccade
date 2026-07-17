#include "backends/metal/overlay_expander.hpp"

#include <saccade/saccade_overlay.h>

#import <Metal/Metal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    unsupported = 77,
    native_device = 2,
    texture = 3,
    initial_render = 4,
    initial_pixels = 5,
    active_render = 6,
    active_pixels = 7,
    stats = 8,
    invalid_target = 9,
    animated_render = 10,
    animated_clear = 11,
    animated_render_again = 12,
    animated_pixels = 13,
    metal4_initialize = 14,
    metal4_render = 15,
    metal4_pixels = 16,
    metal4_warmup = 17,
    metal4_allocator = 18,
    metal4_allocator_limit = 19,
    rotating_texture = 20,
    rotating_render = 21,
    resident_targets = 22,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t width = 64;
constexpr uint32_t height = 64;
constexpr size_t packet_size =
    sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);

alignas(64) std::array<uint8_t, packet_size> packet{};
alignas(64) std::array<uint8_t, width * height * 4U> pixels{};

template <class Record> void store(size_t offset, const Record& record) noexcept {
    std::memcpy(packet.data() + offset, &record, sizeof(record));
}

SaccadeOverlayFrameDesc make_frame(bool active, bool animated = false, uint64_t scene_epoch = 0) noexcept {
    constexpr size_t targets_offset = sizeof(SaccadeOverlayPacketHeader);
    constexpr size_t styles_offset = targets_offset + sizeof(SaccadeOverlayTarget);

    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = 1;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene_epoch != 0 ? scene_epoch : active ? 2 : 1;
    header.transform_epoch = 1;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store(0, header);

    SaccadeOverlayTarget target{};
    target.target_id = 1;
    target.x_q3 = 8U * 8U;
    target.y_q3 = 8U * 8U;
    target.width_q3 = 24U * 8U;
    target.height_q3 = 16U * 8U;
    target.label_x_q3 = 8U * 8U;
    target.label_y_q3 = 32U * 8U;
    target.confidence_q16 = UINT16_MAX;
    target.glyphs[0] = 0;
    target.glyphs[1] = 1;
    target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyph_count = 2;
    store(targets_offset, target);

    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = UINT32_C(0xFF0000FF);
    style.label_background_rgba8 = UINT32_C(0x00FF00FF);
    style.label_foreground_rgba8 = UINT32_C(0xFFFFFFFF);
    style.active_fill_rgba8 = UINT32_C(0x0000FF80);
    style.active_outline_rgba8 = UINT32_C(0xFFFFFFFF);
    style.target_stroke_q3 = 2U * 8U;
    style.target_radius_q3 = 3U * 8U;
    style.label_height_q3 = 16U * 8U;
    style.label_radius_q3 = 3U * 8U;
    style.label_padding_x_q3 = 2U * 8U;
    style.glyph_width_q3 = 5U * 8U;
    style.glyph_height_q3 = 7U * 8U;
    style.glyph_advance_q3 = 8U * 8U;
    style.active_stroke_q3 = 2U * 8U;
    style.flags = animated ? SACCADE_OVERLAY_STYLE_ANIMATED : 0;
    store(styles_offset, style);

    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = header.scene_epoch;
    frame.transform_epoch = header.transform_epoch;
    frame.packet = {packet.data(), packet.size()};
    if (active) {
        frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
        frame.active_target_index = 0;
    }
    return frame;
}

const uint8_t* pixel(uint32_t x, uint32_t y) noexcept {
    return pixels.data() + (static_cast<size_t>(y) * width + x) * 4U;
}

bool transparent(uint32_t x, uint32_t y) noexcept {
    const uint8_t* value = pixel(x, y);
    return value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0;
}

bool white_glyph(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) noexcept {
    for (uint32_t y = top; y < bottom; ++y) {
        for (uint32_t x = left; x < right; ++x) {
            const uint8_t* value = pixel(x, y);
            if (value[0] > 240 && value[1] > 240 && value[2] > 240 && value[3] > 240) return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return to_process_exit_code(ExitCode::invalid_arguments);
    }

    @autoreleasepool {
        saccade::backend::metal::OverlayExpander renderer;
        if (renderer.initialize(argv[1], saccade::backend::metal::PathPreference::metal3) ==
            SACCADE_ERROR_UNSUPPORTED) {
            return to_process_exit_code(ExitCode::unsupported);
        }
        if (renderer.native_device() == nullptr) {
            return to_process_exit_code(ExitCode::native_device);
        }
        id<MTLDevice> device = (__bridge id<MTLDevice>)renderer.native_device();
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        if (texture == nil) {
            return to_process_exit_code(ExitCode::texture);
        }

        saccade::backend::metal::RenderTarget target{};
        target.texture = (__bridge void*)texture;
        target.width = width;
        target.height = height;
        saccade::backend::metal::Submission submission{};
        SaccadeOverlayFrameDesc frame = make_frame(false);
        if (renderer.render(frame, target, &submission) != SACCADE_OK ||
            renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
            return to_process_exit_code(ExitCode::initial_render);
        }
        [texture getBytes:pixels.data()
              bytesPerRow:width * 4U
               fromRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0];
        const uint8_t* outline = pixel(12, 8);
        const uint8_t* label = pixel(9, 34);
        bool glyph = white_glyph(10, 36, 15, 43);
        if (!transparent(63, 63) || outline[0] != 0 || outline[1] != 0 || outline[2] < 250 || outline[3] < 250 ||
            label[0] != 0 || label[1] < 250 || label[2] != 0 || label[3] < 250 || !glyph) {
            return to_process_exit_code(ExitCode::initial_pixels);
        }

        frame = make_frame(true);
        if (renderer.render(frame, target, &submission) != SACCADE_OK ||
            renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
            return to_process_exit_code(ExitCode::active_render);
        }
        [texture getBytes:pixels.data()
              bytesPerRow:width * 4U
               fromRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0];
        const uint8_t* active = pixel(20, 16);
        if (active[0] < 120 || active[0] > 136 || active[1] != 0 || active[2] != 0 || active[3] < 120 ||
            active[3] > 136) {
            return to_process_exit_code(ExitCode::active_pixels);
        }

        const auto stats = renderer.stats();
        if (stats.rendered_frames != 2 || stats.draw_calls != 2 || stats.presented_frames != 0 ||
            stats.render_failures != 0) {
            return to_process_exit_code(ExitCode::stats);
        }
        target.texture = nullptr;
        if (renderer.render(frame, target, &submission) != SACCADE_ERROR_INVALID_ARGUMENT) {
            return to_process_exit_code(ExitCode::invalid_target);
        }

        target.texture = (__bridge void*)texture;
        target.target_presentation_time = 10.0;
        frame = make_frame(false, true, 3);
        if (renderer.render(frame, target, &submission) != SACCADE_OK ||
            renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
            return to_process_exit_code(ExitCode::animated_render);
        }
        [texture getBytes:pixels.data()
              bytesPerRow:width * 4U
               fromRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0];
        if (!transparent(12, 8)) {
            return to_process_exit_code(ExitCode::animated_clear);
        }

        target.target_presentation_time = 10.2;
        if (renderer.render(frame, target, &submission) != SACCADE_OK ||
            renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
            return to_process_exit_code(ExitCode::animated_render_again);
        }
        [texture getBytes:pixels.data()
              bytesPerRow:width * 4U
               fromRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0];
        outline = pixel(12, 8);
        if (outline[2] < 250 || outline[3] < 250) {
            return to_process_exit_code(ExitCode::animated_pixels);
        }
        target.target_presentation_time = 0.0;

        saccade::backend::metal::OverlayExpander metal4_renderer;
        const SaccadeResult metal4_initialized =
            metal4_renderer.initialize(argv[1], saccade::backend::metal::PathPreference::metal4);
        if (metal4_initialized != SACCADE_ERROR_UNSUPPORTED) {
            if (metal4_initialized != SACCADE_OK ||
                metal4_renderer.stats().path != saccade::backend::metal::Path::metal4) {
                return to_process_exit_code(ExitCode::metal4_initialize);
            }
            id<MTLDevice> metal4_device = (__bridge id<MTLDevice>)metal4_renderer.native_device();
            id<MTLTexture> metal4_texture = [metal4_device newTextureWithDescriptor:descriptor];
            target.texture = (__bridge void*)metal4_texture;
            frame = make_frame(false);
            if (metal4_texture == nil || metal4_renderer.render(frame, target, &submission) != SACCADE_OK ||
                metal4_renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
                return to_process_exit_code(ExitCode::metal4_render);
            }
            [metal4_texture getBytes:pixels.data()
                         bytesPerRow:width * 4U
                          fromRegion:MTLRegionMake2D(0, 0, width, height)
                         mipmapLevel:0];
            outline = pixel(12, 8);
            glyph = white_glyph(10, 36, 15, 43);
            if (!transparent(63, 63) || outline[2] < 250 || outline[3] < 250 || !glyph) {
                return to_process_exit_code(ExitCode::metal4_pixels);
            }
            for (uint32_t iteration = 0; iteration < 64; ++iteration) {
                if (metal4_renderer.render(frame, target, &submission) != SACCADE_OK ||
                    metal4_renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
                    return to_process_exit_code(ExitCode::metal4_warmup);
                }
            }
            const uint64_t allocator_plateau = metal4_renderer.stats().command_allocator_bytes;
            for (uint32_t iteration = 0; iteration < 256; ++iteration) {
                if (metal4_renderer.render(frame, target, &submission) != SACCADE_OK ||
                    metal4_renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
                    return to_process_exit_code(ExitCode::metal4_allocator);
                }
            }
            const uint64_t final_allocator_bytes = metal4_renderer.stats().command_allocator_bytes;
            if (allocator_plateau == 0 || final_allocator_bytes > allocator_plateau * 2U ||
                final_allocator_bytes > 32U * 1024U * 1024U) {
                return to_process_exit_code(ExitCode::metal4_allocator_limit);
            }

            std::array<id<MTLTexture>, 8> rotating_textures{};
            for (uint32_t index = 0; index < rotating_textures.size(); ++index) {
                rotating_textures[index] = [metal4_device newTextureWithDescriptor:descriptor];
                if (rotating_textures[index] == nil) {
                    return to_process_exit_code(ExitCode::rotating_texture);
                }
            }
            for (uint32_t iteration = 0; iteration < rotating_textures.size(); ++iteration) {
                target.texture = (__bridge void*)rotating_textures[iteration];
                if (metal4_renderer.render(frame, target, &submission) != SACCADE_OK ||
                    metal4_renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK ||
                    metal4_renderer.submit(frame, &submission) != SACCADE_OK ||
                    metal4_renderer.wait(submission, UINT64_C(1000000000)) != SACCADE_OK) {
                    return to_process_exit_code(ExitCode::rotating_render);
                }
            }
            if (metal4_renderer.stats().resident_render_targets > 3) {
                return to_process_exit_code(ExitCode::resident_targets);
            }
        }
    }
    return to_process_exit_code(ExitCode::success);
}

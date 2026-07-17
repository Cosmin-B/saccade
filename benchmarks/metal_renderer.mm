#include "backends/metal/overlay_expander.hpp"
#include "core/stack_string_builder.hpp"

#include <saccade/saccade_overlay.h>

#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    automatic_path_failed = 2,
    metal3_path_failed = 3,
    output_truncated = 4,
    output_failed = 5
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

constexpr uint32_t target_count = 10000;
constexpr uint32_t drawable_width = 3840;
constexpr uint32_t drawable_height = 2160;
constexpr size_t packet_capacity =
    sizeof(SaccadeOverlayPacketHeader) + target_count * sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);

alignas(64) std::array<uint8_t, packet_capacity> packet{};
uint64_t benchmark_sink = 0;

template <class Record> void store(size_t offset, const Record& value) noexcept {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}

SaccadeSpanU8 build_packet(uint64_t scene_epoch) noexcept {
    constexpr size_t targets_offset = sizeof(SaccadeOverlayPacketHeader);
    constexpr size_t styles_offset = targets_offset + target_count * sizeof(SaccadeOverlayTarget);

    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene_epoch;
    header.transform_epoch = 1;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store(0, header);

    for (uint32_t index = 0; index < target_count; ++index) {
        const uint32_t column = index % 100U;
        const uint32_t row = index / 100U;
        SaccadeOverlayTarget target{};
        target.target_id = static_cast<uint64_t>(index) + 1U;
        target.x_q3 = static_cast<uint16_t>((column * 38U + 4U) * 8U);
        target.y_q3 = static_cast<uint16_t>((row * 21U + 3U) * 8U);
        target.width_q3 = 24U * 8U;
        target.height_q3 = 14U * 8U;
        target.label_x_q3 = target.x_q3;
        target.label_y_q3 = target.y_q3;
        target.confidence_q16 = UINT16_MAX;
        target.glyphs[0] = static_cast<uint8_t>(index % 26U);
        target.glyphs[1] = static_cast<uint8_t>((index / 26U) % 26U);
        target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyph_count = 2;
        store(targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }

    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = UINT32_C(0x36D399D9);
    style.label_background_rgba8 = UINT32_C(0x101820F2);
    style.label_foreground_rgba8 = UINT32_C(0xFFFFFFFF);
    style.active_fill_rgba8 = UINT32_C(0x36D39950);
    style.active_outline_rgba8 = UINT32_C(0xFFFFFFFF);
    style.target_stroke_q3 = 8;
    style.target_radius_q3 = 16;
    style.label_height_q3 = 12U * 8U;
    style.label_radius_q3 = 16;
    style.label_padding_x_q3 = 8;
    style.glyph_width_q3 = 5U * 8U;
    style.glyph_height_q3 = 7U * 8U;
    style.glyph_advance_q3 = 7U * 8U;
    style.active_stroke_q3 = 16;
    store(styles_offset, style);
    return {packet.data(), packet.size()};
}

void set_scene_epoch(uint64_t epoch) noexcept {
    SaccadeOverlayPacketHeader header{};
    std::memcpy(&header, packet.data(), sizeof(header));
    header.scene_epoch = epoch;
    store(0, header);
}

const char* path_name(saccade::backend::metal::Path path) noexcept {
    switch (path) {
    case saccade::backend::metal::Path::metal3:
        return "metal3";
    case saccade::backend::metal::Path::metal4:
        return "metal4";
    case saccade::backend::metal::Path::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

bool append_result(saccade::core::StackStringBuilder<2048>* text, saccade::backend::metal::Path path,
                   const char* operation, uint32_t iterations, uint64_t elapsed) noexcept {
    return elapsed != 0 && text->append("path=") && text->append(path_name(path)) && text->append(" targets=") &&
           text->append_unsigned(target_count) && text->append(" resolution=") &&
           text->append_unsigned(drawable_width) && text->append("x") && text->append_unsigned(drawable_height) &&
           text->append(" operation=") && text->append(operation) && text->append(" iterations=") &&
           text->append_unsigned(iterations) && text->append(" total_ns=") && text->append_unsigned(elapsed) &&
           text->append(" ns_per_frame=") && text->append_unsigned(elapsed / iterations) && text->append('\n');
}

bool run_path(const char* metallib, saccade::backend::metal::PathPreference preference,
              saccade::core::StackStringBuilder<2048>* text, saccade::backend::metal::Path* selected) noexcept {
    using namespace saccade::backend::metal;
    OverlayExpander renderer;
    const SaccadeResult initialized = renderer.initialize(metallib, preference);
    if (initialized != SACCADE_OK) {
        return false;
    }
    *selected = renderer.stats().path;
    id<MTLDevice> device = (__bridge id<MTLDevice>)renderer.native_device();
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:drawable_width
                                                                                         height:drawable_height
                                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget;
    std::array<id<MTLTexture>, 3> textures{[device newTextureWithDescriptor:descriptor],
                                           [device newTextureWithDescriptor:descriptor],
                                           [device newTextureWithDescriptor:descriptor]};
    if (textures[0] == nil || textures[1] == nil || textures[2] == nil) {
        return false;
    }

    SaccadeSpanU8 bytes = build_packet(1);
    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = 1;
    frame.transform_epoch = 1;
    frame.packet = bytes;
    std::array<RenderTarget, 3> targets{};
    for (uint32_t index = 0; index < targets.size(); ++index) {
        targets[index].texture = (__bridge void*)textures[index];
        targets[index].width = drawable_width;
        targets[index].height = drawable_height;
    }

    for (uint32_t index = 0; index < 3; ++index) {
        Submission warmup{};
        if (renderer.render(frame, targets[index], &warmup) != SACCADE_OK ||
            renderer.wait(warmup, UINT64_C(1000000000)) != SACCADE_OK) {
            return false;
        }
    }

    constexpr uint32_t active_iterations = 240;
    uint32_t completed = 0;
    const auto active_begin = std::chrono::steady_clock::now();
    for (uint32_t base = 0; base < active_iterations; base += 3U) {
        @autoreleasepool {
            std::array<Submission, 3> submissions{};
            for (uint32_t lane = 0; lane < submissions.size(); ++lane) {
                frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
                frame.active_target_index = (base + lane) % target_count;
                if (renderer.render(frame, targets[lane], &submissions[lane]) != SACCADE_OK) {
                    return false;
                }
            }
            if (renderer.wait(submissions.back(), UINT64_C(1000000000)) != SACCADE_OK) {
                return false;
            }
            completed += 3;
        }
    }
    const uint64_t active_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - active_begin).count());

    constexpr uint32_t scene_iterations = 60;
    const auto scene_begin = std::chrono::steady_clock::now();
    for (uint32_t base = 0; base < scene_iterations; base += 3U) {
        @autoreleasepool {
            std::array<Submission, 3> submissions{};
            for (uint32_t lane = 0; lane < submissions.size(); ++lane) {
                const uint64_t epoch = UINT64_C(1000) + base + lane;
                set_scene_epoch(epoch);
                frame.scene_epoch = epoch;
                frame.flags = 0;
                if (renderer.render(frame, targets[lane], &submissions[lane]) != SACCADE_OK) {
                    return false;
                }
            }
            if (renderer.wait(submissions.back(), UINT64_C(1000000000)) != SACCADE_OK) {
                return false;
            }
        }
    }
    const uint64_t scene_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - scene_begin).count());
    benchmark_sink = completed + renderer.stats().rendered_frames;
    const Stats stats = renderer.stats();
    return benchmark_sink != 0 && append_result(text, *selected, "active_render", active_iterations, active_ns) &&
           append_result(text, *selected, "scene_render", scene_iterations, scene_ns) && text->append("path=") &&
           text->append(path_name(*selected)) && text->append(" allocator_bytes=") &&
           text->append_unsigned(stats.command_allocator_bytes) && text->append(" rendered_frames=") &&
           text->append_unsigned(stats.rendered_frames) && text->append('\n');
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return exit_code(ExitCode::invalid_arguments);
    }
    @autoreleasepool {
        saccade::core::StackStringBuilder<2048> text;
        saccade::backend::metal::Path path = saccade::backend::metal::Path::unavailable;
        if (!run_path(argv[1], saccade::backend::metal::PathPreference::automatic, &text, &path)) {
            return exit_code(ExitCode::automatic_path_failed);
        }
        if (path == saccade::backend::metal::Path::metal4 &&
            !run_path(argv[1], saccade::backend::metal::PathPreference::metal3, &text, &path)) {
            return exit_code(ExitCode::metal3_path_failed);
        }
        if (text.truncated()) {
            return exit_code(ExitCode::output_truncated);
        }
        return exit_code(std::fwrite(text.view().data(), 1, text.view().size(), stdout) == text.view().size()
                             ? ExitCode::success
                             : ExitCode::output_failed);
    }
}

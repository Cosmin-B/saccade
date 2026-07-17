#include "backends/metal/preprocessor.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    invalid_arguments = 1,
    fp16_initialize = 1,
    fp16_submit = 2,
    fp16_tensor = 3,
    fp16_values = 4,
    fp16_memory = 5,
    fp16_stats = 6,
    int8_initialize = 7,
    int8_submit = 8,
    int8_tensor = 9,
    int8_values = 10,
    int8_crop = 11,
    int8_cropped_values = 12,
    direct_initialize = 13,
    direct_view = 14,
    image_initialize = 15,
    image_submit = 16,
    image_view = 17,
    image_lock = 18,
    image_pixels = 19,
    image_memory = 20,
    atlas_initialize = 21,
    atlas_submit = 22,
    atlas_view = 23,
    atlas_lock = 24,
    atlas_pixels = 25,
    atlas_update = 26,
    atlas_stats = 27,
    unsupported = 77,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

using saccade::backend::metal::AtlasLoad;
using saccade::backend::metal::AtlasSource;
using saccade::backend::metal::DirectTextureView;
using saccade::backend::metal::ImagePreprocessor;
using saccade::backend::metal::ImageView;
using saccade::backend::metal::Path;
using saccade::backend::metal::PathPreference;
using saccade::backend::metal::PreprocessSubmission;
using saccade::backend::metal::SourceRegion;
using saccade::backend::metal::TensorFormat;
using saccade::backend::metal::TensorSpec;
using saccade::backend::metal::TensorView;

id<MTLTexture> make_texture(id<MTLDevice> device) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:2
                                                                                         height:2
                                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    const std::array<uint8_t, 16> pixels{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:8];
    return texture;
}

bool near(float left, float right) noexcept {
    return std::fabs(left - right) <= 0.002F;
}

ExitCode run_fp16(id<MTLDevice> device, const char* library, PathPreference preference) {
    TensorSpec spec{};
    spec.width = 2;
    spec.height = 2;
    spec.format = TensorFormat::planar_fp16;
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize((__bridge void*)device, library, preference, spec) != SACCADE_OK) {
        return preference == PathPreference::metal4 ? ExitCode::unsupported : ExitCode::fp16_initialize;
    }
    id<MTLTexture> texture = make_texture(device);
    PreprocessSubmission submission{};
    if (preprocessor.submit((__bridge void*)texture, 2, 2, {}, 1, 2, &submission) != SACCADE_OK ||
        preprocessor.wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return ExitCode::fp16_submit;
    }
    TensorView view{};
    if (preprocessor.tensor(submission, &view) != SACCADE_OK || view.buffer == nullptr || view.byte_size != 24 ||
        view.plane_stride_bytes != 8 || view.channels != 3 || view.format != TensorFormat::planar_fp16) {
        return ExitCode::fp16_tensor;
    }
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)view.buffer;
    const auto* values = static_cast<const _Float16*>(buffer.contents);
    const std::array<float, 12> expected{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    for (size_t index = 0; index < expected.size(); ++index) {
        if (!near(static_cast<float>(values[index]), expected[index])) {
            return ExitCode::fp16_values;
        }
    }
    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    if (preprocessor.memory_stats(&memory) != SACCADE_OK || memory.device_owned < view.byte_size ||
        memory.copied_bytes != 0) {
        return ExitCode::fp16_memory;
    }
    const auto stats = preprocessor.stats();
    const Path expected_path = preference == PathPreference::metal3
                                   ? Path::metal3
                                   : (preference == PathPreference::metal4 ? Path::metal4 : stats.path);
    if (stats.path != expected_path || stats.submissions != 1 || stats.completed != 1 ||
        stats.output_bytes != view.byte_size) {
        return ExitCode::fp16_stats;
    }
    return ExitCode::success;
}

ExitCode run_int8(id<MTLDevice> device, const char* library) {
    TensorSpec spec{};
    spec.width = 2;
    spec.height = 2;
    spec.format = TensorFormat::planar_int8;
    spec.channel_scale = {255.0F, 255.0F, 255.0F};
    spec.channel_bias = {-128.0F, -128.0F, -128.0F};
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize((__bridge void*)device, library, PathPreference::automatic, spec) != SACCADE_OK) {
        return ExitCode::int8_initialize;
    }
    id<MTLTexture> texture = make_texture(device);
    PreprocessSubmission first{};
    if (preprocessor.submit((__bridge void*)texture, 2, 2, {}, 9, 10, &first) != SACCADE_OK ||
        preprocessor.wait(first, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return ExitCode::int8_submit;
    }
    TensorView view{};
    if (preprocessor.tensor(first, &view) != SACCADE_OK || view.byte_size != 12) {
        return ExitCode::int8_tensor;
    }
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)view.buffer;
    const auto* values = static_cast<const int8_t*>(buffer.contents);
    const std::array<int8_t, 12> expected{127, -128, -128, 127, -128, 127, -128, 127, -128, -128, 127, 127};
    for (size_t index = 0; index < expected.size(); ++index) {
        if (values[index] != expected[index]) {
            return ExitCode::int8_values;
        }
    }

    PreprocessSubmission second{};
    const SourceRegion right_column{1, 0, 1, 2};
    if (preprocessor.submit((__bridge void*)texture, 2, 2, right_column, 10, 10, &second) != SACCADE_OK ||
        preprocessor.wait(second, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        preprocessor.tensor(first, &view) != SACCADE_ERROR_STALE_HANDLE ||
        preprocessor.tensor(second, &view) != SACCADE_OK) {
        return ExitCode::int8_crop;
    }
    buffer = (__bridge id<MTLBuffer>)view.buffer;
    values = static_cast<const int8_t*>(buffer.contents);
    const std::array<int8_t, 12> cropped{-128, -128, 127, -128, 127, -128, 127, -128, -128, -128, 127, -128};
    for (size_t index = 0; index < cropped.size(); ++index) {
        if (values[index] != cropped[index]) {
            std::fprintf(stderr, "crop[%zu]=%d expected=%d\n", index, static_cast<int>(values[index]),
                         static_cast<int>(cropped[index]));
            return ExitCode::int8_cropped_values;
        }
    }
    return ExitCode::success;
}

ExitCode run_direct(id<MTLDevice> device) {
    TensorSpec spec{};
    spec.format = TensorFormat::direct_texture;
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize((__bridge void*)device, nullptr, PathPreference::automatic, spec) != SACCADE_OK) {
        return ExitCode::direct_initialize;
    }
    id<MTLTexture> texture = make_texture(device);
    DirectTextureView view{};
    if (preprocessor.direct_texture((__bridge void*)texture, 2, 2, &view) != SACCADE_OK ||
        view.texture != (__bridge void*)texture || view.width != 2 || view.height != 2 ||
        view.pixel_format != SACCADE_FORMAT_BGRA8 || preprocessor.stats().direct_texture_views != 1) {
        return ExitCode::direct_view;
    }
    return ExitCode::success;
}

ExitCode run_image(id<MTLDevice> device, const char* library, PathPreference preference) {
    TensorSpec spec{};
    spec.width = 4;
    spec.height = 2;
    spec.format = TensorFormat::image_bgra8;
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize((__bridge void*)device, library, preference, spec) != SACCADE_OK) {
        return ExitCode::image_initialize;
    }
    id<MTLTexture> texture = make_texture(device);
    PreprocessSubmission submission{};
    if (preprocessor.submit((__bridge void*)texture, 2, 2, {}, 20, 21, &submission) != SACCADE_OK ||
        preprocessor.wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return ExitCode::image_submit;
    }
    ImageView image{};
    if (preprocessor.image(submission, &image) != SACCADE_OK || image.pixel_buffer == nullptr ||
        image.texture == nullptr || image.iosurface_id == 0 || image.width != 4 || image.height != 2 ||
        image.pixel_format != SACCADE_FORMAT_BGRA8 || image.content.x != 1 || image.content.y != 0 ||
        image.content.width != 2 || image.content.height != 2) {
        return ExitCode::image_view;
    }
    CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(image.pixel_buffer);
    if (CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return ExitCode::image_lock;
    }
    const auto* row0 = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const auto* row1 = row0 + CVPixelBufferGetBytesPerRow(pixel_buffer);
    const std::array<uint8_t, 16> expected_row0{0, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255, 0, 0, 0, 255};
    const std::array<uint8_t, 16> expected_row1{0, 0, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    const bool pixels_match = std::memcmp(row0, expected_row0.data(), expected_row0.size()) == 0 &&
                              std::memcmp(row1, expected_row1.data(), expected_row1.size()) == 0;
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    if (!pixels_match) {
        return ExitCode::image_pixels;
    }
    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    if (preprocessor.memory_stats(&memory) != SACCADE_OK || memory.device_owned < 4U * 2U * 4U ||
        memory.copied_bytes != 0) {
        return ExitCode::image_memory;
    }
    return ExitCode::success;
}

ExitCode run_atlas(id<MTLDevice> device, const char* library, PathPreference preference) {
    TensorSpec spec{};
    spec.width = 4;
    spec.height = 2;
    spec.format = TensorFormat::image_bgra8;
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize((__bridge void*)device, library, preference, spec) != SACCADE_OK) {
        return ExitCode::atlas_initialize;
    }

    id<MTLTexture> texture = make_texture(device);
    const std::array<AtlasSource, 2> sources = {
        AtlasSource{(__bridge void*)texture, 2, 2, {0, 0, 1, 2}, {0, 0, 2, 2}},
        AtlasSource{(__bridge void*)texture, 2, 2, {1, 0, 1, 2}, {2, 0, 2, 2}},
    };
    PreprocessSubmission submission{};
    if (preprocessor.submit_atlas(sources.data(), sources.size(), {0, 0, 4, 2}, AtlasLoad::clear, 30, 31,
                                  &submission) != SACCADE_OK ||
        preprocessor.wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return ExitCode::atlas_submit;
    }

    ImageView image{};
    if (preprocessor.image(submission, &image) != SACCADE_OK || image.content.x != 0 || image.content.y != 0 ||
        image.content.width != 4 || image.content.height != 2) {
        return ExitCode::atlas_view;
    }
    CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(image.pixel_buffer);
    if (CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return ExitCode::atlas_lock;
    }
    const auto* row0 = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const auto* row1 = row0 + CVPixelBufferGetBytesPerRow(pixel_buffer);
    const std::array<uint8_t, 16> expected_row0{0, 0, 255, 255, 0, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    const std::array<uint8_t, 16> expected_row1{255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    const bool pixels_match = std::memcmp(row0, expected_row0.data(), expected_row0.size()) == 0 &&
                              std::memcmp(row1, expected_row1.data(), expected_row1.size()) == 0;
    if (!pixels_match) {
        CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
        return ExitCode::atlas_pixels;
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

    const AtlasSource update{(__bridge void*)texture, 2, 2, {0, 0, 1, 2}, {2, 0, 2, 2}};
    if (preprocessor.submit_atlas(&update, 1, {0, 0, 4, 2}, AtlasLoad::preserve, 32, 31, &submission) != SACCADE_OK ||
        preprocessor.wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        preprocessor.image(submission, &image) != SACCADE_OK ||
        CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return ExitCode::atlas_update;
    }
    row0 = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    row1 = row0 + CVPixelBufferGetBytesPerRow(pixel_buffer);
    const std::array<uint8_t, 16> updated_row0{0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    const std::array<uint8_t, 16> updated_row1{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const bool update_matches = std::memcmp(row0, updated_row0.data(), updated_row0.size()) == 0 &&
                                std::memcmp(row1, updated_row1.data(), updated_row1.size()) == 0;
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    const auto stats = preprocessor.stats();
    if (!update_matches || stats.atlas_submissions != 2 || stats.atlas_sources != 3) {
        return ExitCode::atlas_stats;
    }
    return ExitCode::success;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) {
            return to_process_exit_code(ExitCode::fp16_initialize);
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return to_process_exit_code(ExitCode::unsupported);
        }
        ExitCode result = run_direct(device);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_image(device, argv[1], PathPreference::automatic);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_image(device, argv[1], PathPreference::metal3);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_atlas(device, argv[1], PathPreference::automatic);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_atlas(device, argv[1], PathPreference::metal3);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_fp16(device, argv[1], PathPreference::automatic);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        result = run_fp16(device, argv[1], PathPreference::metal3);
        if (result != ExitCode::success) {
            return to_process_exit_code(result);
        }
        if (@available(macOS 26.0, *)) {
            if ([device supportsFamily:MTLGPUFamilyMetal4]) {
                result = run_fp16(device, argv[1], PathPreference::metal4);
                if (result != ExitCode::success) {
                    return to_process_exit_code(result);
                }
            }
        }
        return to_process_exit_code(run_int8(device, argv[1]));
    }
}

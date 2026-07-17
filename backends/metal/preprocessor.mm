#include "backends/metal/preprocessor.hpp"

#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace saccade::backend::metal {
namespace {

struct PreprocessParameters {
    std::array<float, 4> source_rect;
    std::array<float, 4> content_rect;
    std::array<float, 4> channel_scale;
    std::array<float, 4> channel_bias;
    std::array<float, 4> letterbox_rgb;
    std::array<uint32_t, 2> output_size;
    std::array<uint32_t, 2> reserved;
};

struct AtlasClearParameters {
    std::array<float, 4> color;
    std::array<uint32_t, 2> output_size;
    std::array<uint32_t, 2> reserved;
};

struct AtlasParameters {
    std::array<float, 4> source_rect;
    std::array<float, 4> channel_scale;
    std::array<float, 4> channel_bias;
    std::array<uint32_t, 4> destination_rect;
};

static_assert(sizeof(PreprocessParameters) == 96);
static_assert(sizeof(AtlasClearParameters) == 32);
static_assert(sizeof(AtlasParameters) == 64);

constexpr size_t parameter_stride = 256;
constexpr size_t preprocess_parameter_offset = 0;
constexpr size_t atlas_clear_parameter_offset = parameter_stride;
constexpr size_t atlas_parameter_offset = parameter_stride * 2;
constexpr size_t atlas_parameter_bytes = parameter_stride * (atlas_source_capacity + 2U);

bool finite_channels(const std::array<float, 3>& values) noexcept {
    return std::all_of(values.begin(), values.end(), [](float value) noexcept { return std::isfinite(value); });
}

bool tensor_spec_valid(const TensorSpec& spec) noexcept {
    if (spec.reserved != 0 || !finite_channels(spec.channel_scale) || !finite_channels(spec.channel_bias) ||
        !finite_channels(spec.letterbox_rgb)) {
        return false;
    }
    if (spec.format == TensorFormat::direct_texture) {
        return spec.width == 0 && spec.height == 0;
    }
    return (spec.format == TensorFormat::planar_fp16 || spec.format == TensorFormat::planar_int8 ||
            spec.format == TensorFormat::image_bgra8) &&
           spec.width != 0 && spec.height != 0;
}

bool required_output_bytes(const TensorSpec& spec, size_t* output) noexcept {
    if (output == nullptr || spec.format == TensorFormat::direct_texture) {
        return false;
    }
    const size_t element_bytes =
        spec.format == TensorFormat::planar_fp16 ? 2U : (spec.format == TensorFormat::image_bgra8 ? 4U : 1U);
    const size_t width = spec.width;
    const size_t height = spec.height;
    const size_t channels = spec.format == TensorFormat::image_bgra8 ? 1U : 3U;
    const size_t maximum = std::numeric_limits<size_t>::max();
    if (width > maximum / height || width * height > maximum / channels ||
        width * height * channels > maximum / element_bytes) {
        return false;
    }

    *output = width * height * channels * element_bytes;
    return true;
}

uint64_t allocated_bytes(id<MTLBuffer> buffer) noexcept {
    return buffer == nil ? 0 : static_cast<uint64_t>(buffer.allocatedSize);
}

bool region_valid(const SourceRegion& region, uint32_t width, uint32_t height) noexcept {
    return region.width != 0 && region.height != 0 && static_cast<uint64_t>(region.x) + region.width <= width &&
           static_cast<uint64_t>(region.y) + region.height <= height;
}

} // namespace

struct ImagePreprocessor::Impl {
    id<MTLDevice> device_ = nil;
    id<MTLLibrary> library_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    id<MTLComputePipelineState> atlas_clear_pipeline_ = nil;
    id<MTLComputePipelineState> atlas_pipeline_ = nil;
    id<MTLBuffer> parameters_ = nil;
    id<MTLBuffer> output_ = nil;
    CVMetalTextureCacheRef texture_cache_ = nullptr;
    CVPixelBufferRef output_pixel_buffer_ = nullptr;
    CVMetalTextureRef output_cv_texture_ = nullptr;
    id<MTLTexture> output_texture_ = nil;
    id<MTLCommandQueue> queue3_ = nil;
    id<MTLCommandBuffer> command_buffer3_ = nil;
    id queue4_ = nil;
    id argument_table4_ = nil;
    std::array<id, atlas_source_capacity + 1> atlas_argument_tables4_{};
    id allocator4_ = nil;
    id command_buffer4_ = nil;
    id residency_set4_ = nil;
    id<MTLSharedEvent> completion_event_ = nil;
    std::array<id<MTLTexture>, atlas_source_capacity> source_textures_{};
    TensorSpec spec_{};
    PreprocessorStats stats_{};
    uint64_t sequence_ = 0;
    uint64_t frame_id_ = 0;
    uint64_t transform_epoch_ = 0;
    uint64_t counted_sequence_ = 0;
    size_t output_bytes_ = 0;
    uint32_t source_texture_count_ = 0;
    SourceRegion content_region_{};
    bool initialized_ = false;

    ~Impl() {
        bool retired = true;
        if (completion_event_ != nil && sequence_ != 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (completion_event_.signaledValue < sequence_) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    retired = false;
                    break;
                }
                std::this_thread::yield();
            }
        }
        if (@available(macOS 26.0, *)) {
            if (retired && queue4_ != nil && residency_set4_ != nil) {
                [queue4_ removeResidencySet:residency_set4_];
                [residency_set4_ endResidency];
            }
        }
        output_texture_ = nil;
        if (output_cv_texture_ != nullptr) {
            CFRelease(output_cv_texture_);
        }
        if (output_pixel_buffer_ != nullptr) {
            CVPixelBufferRelease(output_pixel_buffer_);
        }
        if (texture_cache_ != nullptr) {
            CFRelease(texture_cache_);
        }
    }

    bool metal4_supported() const noexcept {
        if (@available(macOS 26.0, *)) {
            return [device_ supportsFamily:MTLGPUFamilyMetal4];
        }
        return false;
    }

    bool create_pipeline(const char* metallib_path) noexcept {
        NSString* path = [NSString stringWithUTF8String:metallib_path];
        if (path == nil) {
            return false;
        }
        NSError* error = nil;
        library_ = [device_ newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (library_ == nil) {
            return false;
        }
        NSString* name = spec_.format == TensorFormat::planar_fp16
                             ? @"saccade_preprocess_fp16"
                             : (spec_.format == TensorFormat::planar_int8 ? @"saccade_preprocess_int8"
                                                                          : @"saccade_preprocess_bgra8");
        id<MTLFunction> function = [library_ newFunctionWithName:name];
        if (function == nil) {
            return false;
        }
        pipeline_ = [device_ newComputePipelineStateWithFunction:function error:&error];
        if (pipeline_ == nil) return false;
        if (spec_.format != TensorFormat::image_bgra8) return true;

        id<MTLFunction> clear_function = [library_ newFunctionWithName:@"saccade_preprocess_clear_bgra8"];
        id<MTLFunction> atlas_function = [library_ newFunctionWithName:@"saccade_preprocess_atlas_bgra8"];
        if (clear_function == nil || atlas_function == nil) return false;
        atlas_clear_pipeline_ = [device_ newComputePipelineStateWithFunction:clear_function error:&error];
        atlas_pipeline_ = [device_ newComputePipelineStateWithFunction:atlas_function error:&error];
        return atlas_clear_pipeline_ != nil && atlas_pipeline_ != nil;
    }

    bool create_output_image() noexcept {
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device_, nullptr, &texture_cache_) !=
            kCVReturnSuccess) {
            return false;
        }
        NSDictionary* pixel_attributes = @{
            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
            (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey : @YES
        };
        if (CVPixelBufferCreate(kCFAllocatorDefault, spec_.width, spec_.height, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)pixel_attributes,
                                &output_pixel_buffer_) != kCVReturnSuccess) {
            return false;
        }
        NSDictionary* texture_attributes =
            @{(__bridge NSString*)kCVMetalTextureUsage : @(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)};
        if (CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, texture_cache_, output_pixel_buffer_, (__bridge CFDictionaryRef)texture_attributes,
                MTLPixelFormatBGRA8Unorm, spec_.width, spec_.height, 0, &output_cv_texture_) != kCVReturnSuccess ||
            output_cv_texture_ == nullptr) {
            return false;
        }
        output_texture_ = CVMetalTextureGetTexture(output_cv_texture_);
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(output_pixel_buffer_);
        if (output_texture_ == nil || surface == nullptr) return false;
        output_bytes_ = IOSurfaceGetAllocSize(surface);
        return output_bytes_ != 0;
    }

    bool create_resources() noexcept {
        constexpr MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
        const size_t parameter_bytes =
            spec_.format == TensorFormat::image_bgra8 ? atlas_parameter_bytes : sizeof(PreprocessParameters);
        parameters_ = [device_ newBufferWithLength:parameter_bytes options:options];
        if (parameters_ == nil) return false;
        if (spec_.format == TensorFormat::image_bgra8) {
            return create_output_image();
        }
        output_ = [device_ newBufferWithLength:output_bytes_ options:options];
        return output_ != nil;
    }

    bool create_metal3() noexcept {
        queue3_ = [device_ newCommandQueueWithMaxCommandBufferCount:1];
        return queue3_ != nil;
    }

    bool create_metal4() noexcept API_AVAILABLE(macos(26.0)) {
        queue4_ = [device_ newMTL4CommandQueue];
        MTL4ArgumentTableDescriptor* table_descriptor = [[MTL4ArgumentTableDescriptor alloc] init];
        table_descriptor.maxBufferBindCount = 2;
        table_descriptor.maxTextureBindCount = 2;
        table_descriptor.initializeBindings = YES;
        argument_table4_ = [device_ newArgumentTableWithDescriptor:table_descriptor error:nil];
        if (spec_.format == TensorFormat::image_bgra8) {
            for (uint32_t index = 0; index < atlas_argument_tables4_.size(); ++index) {
                atlas_argument_tables4_[index] = [device_ newArgumentTableWithDescriptor:table_descriptor error:nil];
                if (atlas_argument_tables4_[index] == nil) return false;
            }
        }
        allocator4_ = [device_ newCommandAllocator];
        command_buffer4_ = [device_ newCommandBuffer];
        MTLResidencySetDescriptor* residency_descriptor = [[MTLResidencySetDescriptor alloc] init];
        residency_descriptor.initialCapacity = atlas_source_capacity + 2U;
        residency_set4_ = [device_ newResidencySetWithDescriptor:residency_descriptor error:nil];
        if (queue4_ == nil || argument_table4_ == nil || allocator4_ == nil || command_buffer4_ == nil ||
            residency_set4_ == nil) {
            return false;
        }
        [residency_set4_ addAllocation:parameters_];
        if (output_ != nil) {
            [residency_set4_ addAllocation:output_];
        } else {
            [residency_set4_ addAllocation:output_texture_];
        }
        [residency_set4_ commit];
        [residency_set4_ requestResidency];
        [queue4_ addResidencySet:residency_set4_];
        id<MTL4ArgumentTable> argument_table = static_cast<id<MTL4ArgumentTable>>(argument_table4_);
        [argument_table setAddress:parameters_.gpuAddress + preprocess_parameter_offset atIndex:0];
        if (output_ != nil) {
            [argument_table setAddress:output_.gpuAddress atIndex:1];
        } else {
            [argument_table setTexture:output_texture_.gpuResourceID atIndex:1];
            id<MTL4ArgumentTable> clear_table = static_cast<id<MTL4ArgumentTable>>(atlas_argument_tables4_[0]);
            [clear_table setAddress:parameters_.gpuAddress + atlas_clear_parameter_offset atIndex:0];
            [clear_table setTexture:output_texture_.gpuResourceID atIndex:1];
            for (uint32_t index = 0; index < atlas_source_capacity; ++index) {
                id<MTL4ArgumentTable> atlas_table =
                    static_cast<id<MTL4ArgumentTable>>(atlas_argument_tables4_[index + 1U]);
                [atlas_table setAddress:parameters_.gpuAddress + atlas_parameter_offset + index * parameter_stride
                                atIndex:0];
                [atlas_table setTexture:output_texture_.gpuResourceID atIndex:1];
            }
        }
        return true;
    }

    void discard_metal4() noexcept API_AVAILABLE(macos(26.0)) {
        if (queue4_ != nil && residency_set4_ != nil) {
            [queue4_ removeResidencySet:residency_set4_];
            [residency_set4_ endResidency];
        }
        queue4_ = nil;
        argument_table4_ = nil;
        atlas_argument_tables4_.fill(nil);
        allocator4_ = nil;
        command_buffer4_ = nil;
        residency_set4_ = nil;
    }

    bool texture_valid(id<MTLTexture> texture, uint32_t width, uint32_t height) const noexcept {
        return texture != nil && texture.device == device_ && width != 0 && height != 0 && texture.width == width &&
               texture.height == height && texture.pixelFormat == MTLPixelFormatBGRA8Unorm;
    }

    bool idle() const noexcept { return sequence_ == 0 || completion_event_.signaledValue >= sequence_; }

    PreprocessParameters make_parameters(uint32_t source_width, uint32_t source_height,
                                         SourceRegion region) const noexcept {
        if (region.width == 0 || region.height == 0) {
            region = {0, 0, source_width, source_height};
        }
        const float output_width = static_cast<float>(spec_.width);
        const float output_height = static_cast<float>(spec_.height);
        const float region_width = static_cast<float>(region.width);
        const float region_height = static_cast<float>(region.height);
        const float scale = std::min(output_width / region_width, output_height / region_height);
        const float content_width = std::max(1.0F, std::floor(region_width * scale));
        const float content_height = std::max(1.0F, std::floor(region_height * scale));
        PreprocessParameters result{};
        result.source_rect = {static_cast<float>(region.x), static_cast<float>(region.y), region_width, region_height};
        result.content_rect = {std::floor((output_width - content_width) * 0.5F),
                               std::floor((output_height - content_height) * 0.5F), content_width, content_height};
        result.channel_scale = {spec_.channel_scale[0], spec_.channel_scale[1], spec_.channel_scale[2], 0.0F};
        result.channel_bias = {spec_.channel_bias[0], spec_.channel_bias[1], spec_.channel_bias[2], 0.0F};
        result.letterbox_rgb = {spec_.letterbox_rgb[0], spec_.letterbox_rgb[1], spec_.letterbox_rgb[2], 0.0F};
        result.output_size = {spec_.width, spec_.height};
        return result;
    }

    AtlasClearParameters make_atlas_clear_parameters() const noexcept {
        AtlasClearParameters result{};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            result.color[channel] = std::clamp(
                spec_.letterbox_rgb[channel] * spec_.channel_scale[channel] + spec_.channel_bias[channel], 0.0F, 1.0F);
        }
        result.color[3] = 1.0F;
        result.output_size = {spec_.width, spec_.height};
        return result;
    }

    AtlasParameters make_atlas_parameters(const AtlasSource& source) const noexcept {
        AtlasParameters result{};
        result.source_rect = {static_cast<float>(source.source.x), static_cast<float>(source.source.y),
                              static_cast<float>(source.source.width), static_cast<float>(source.source.height)};
        result.channel_scale = {spec_.channel_scale[0], spec_.channel_scale[1], spec_.channel_scale[2], 0.0F};
        result.channel_bias = {spec_.channel_bias[0], spec_.channel_bias[1], spec_.channel_bias[2], 0.0F};
        result.destination_rect = {source.destination.x, source.destination.y, source.destination.width,
                                   source.destination.height};
        return result;
    }

    bool replace_resident_sources4(id<MTLTexture> __strong const* textures, uint32_t count) noexcept
        API_AVAILABLE(macos(26.0)) {
        for (uint32_t index = 0; index < source_texture_count_; ++index) {
            id<MTLTexture> texture = source_textures_[index];
            if (texture != nil && [residency_set4_ containsAllocation:texture]) {
                [residency_set4_ removeAllocation:texture];
            }
            source_textures_[index] = nil;
        }
        for (uint32_t index = 0; index < count; ++index) {
            id<MTLTexture> texture = textures[index];
            if (![residency_set4_ containsAllocation:texture]) {
                [residency_set4_ addAllocation:texture];
            }
            source_textures_[index] = texture;
        }
        source_texture_count_ = count;
        [residency_set4_ commit];
        return true;
    }

    bool encode_metal3(id<MTLTexture> texture) noexcept {
        command_buffer3_ = [queue3_ commandBuffer];
        if (command_buffer3_ == nil) {
            return false;
        }
        id<MTLComputeCommandEncoder> encoder = [command_buffer3_ computeCommandEncoder];
        if (encoder == nil) {
            return false;
        }
        [encoder setComputePipelineState:pipeline_];
        [encoder setTexture:texture atIndex:0];
        if (output_texture_ != nil) {
            [encoder setTexture:output_texture_ atIndex:1];
        }
        [encoder setBuffer:parameters_ offset:0 atIndex:0];
        if (output_ != nil) {
            [encoder setBuffer:output_ offset:0 atIndex:1];
        }
        const MTLSize threads = MTLSizeMake(16, 16, 1);
        [encoder dispatchThreads:MTLSizeMake(spec_.width, spec_.height, 1) threadsPerThreadgroup:threads];
        [encoder endEncoding];
        [command_buffer3_ encodeSignalEvent:completion_event_ value:sequence_];
        [command_buffer3_ commit];
        return true;
    }

    bool encode_metal4(id<MTLTexture> texture) noexcept API_AVAILABLE(macos(26.0)) {
        id<MTLTexture> textures[] = {texture};
        replace_resident_sources4(textures, 1);
        id<MTL4ArgumentTable> argument_table = static_cast<id<MTL4ArgumentTable>>(argument_table4_);
        [argument_table setTexture:texture.gpuResourceID atIndex:0];
        if (sequence_ > 1) {
            [allocator4_ reset];
        }
        id<MTL4CommandBuffer> command_buffer = static_cast<id<MTL4CommandBuffer>>(command_buffer4_);
        [command_buffer beginCommandBufferWithAllocator:allocator4_];
        id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        [encoder setArgumentTable:argument_table4_];
        [encoder setComputePipelineState:pipeline_];
        [encoder dispatchThreads:MTLSizeMake(spec_.width, spec_.height, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [encoder endEncoding];
        [command_buffer endCommandBuffer];
        id<MTL4CommandBuffer> buffers[] = {command_buffer};
        [queue4_ commit:buffers count:1];
        [queue4_ signalEvent:completion_event_ value:sequence_];
        return true;
    }

    bool encode_atlas_metal3(const AtlasSource* sources, uint32_t source_count, AtlasLoad load) noexcept {
        command_buffer3_ = [queue3_ commandBuffer];
        if (command_buffer3_ == nil) return false;
        id<MTLComputeCommandEncoder> encoder = [command_buffer3_ computeCommandEncoder];
        if (encoder == nil) return false;

        const MTLSize threads = MTLSizeMake(16, 16, 1);
        [encoder setTexture:output_texture_ atIndex:1];
        if (load == AtlasLoad::clear) {
            [encoder setComputePipelineState:atlas_clear_pipeline_];
            [encoder setBuffer:parameters_ offset:atlas_clear_parameter_offset atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(spec_.width, spec_.height, 1) threadsPerThreadgroup:threads];
            [encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
        }

        [encoder setComputePipelineState:atlas_pipeline_];
        for (uint32_t index = 0; index < source_count; ++index) {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)sources[index].texture;
            [encoder setTexture:texture atIndex:0];
            [encoder setBuffer:parameters_ offset:atlas_parameter_offset + index * parameter_stride atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(sources[index].destination.width, sources[index].destination.height, 1)
                threadsPerThreadgroup:threads];
            if (index + 1U < source_count) {
                [encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
            }
        }
        [encoder endEncoding];
        [command_buffer3_ encodeSignalEvent:completion_event_ value:sequence_];
        [command_buffer3_ commit];
        return true;
    }

    bool encode_atlas_metal4(const AtlasSource* sources, uint32_t source_count, AtlasLoad load) noexcept
        API_AVAILABLE(macos(26.0)) {
        std::array<id<MTLTexture>, atlas_source_capacity> textures{};
        for (uint32_t index = 0; index < source_count; ++index) {
            textures[index] = (__bridge id<MTLTexture>)sources[index].texture;
            id<MTL4ArgumentTable> table = static_cast<id<MTL4ArgumentTable>>(atlas_argument_tables4_[index + 1U]);
            [table setTexture:textures[index].gpuResourceID atIndex:0];
        }
        replace_resident_sources4(textures.data(), source_count);
        if (sequence_ > 1) [allocator4_ reset];

        id<MTL4CommandBuffer> command_buffer = static_cast<id<MTL4CommandBuffer>>(command_buffer4_);
        [command_buffer beginCommandBufferWithAllocator:allocator4_];
        id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        const MTLSize threads = MTLSizeMake(16, 16, 1);
        if (load == AtlasLoad::clear) {
            [encoder setArgumentTable:atlas_argument_tables4_[0]];
            [encoder setComputePipelineState:atlas_clear_pipeline_];
            [encoder dispatchThreads:MTLSizeMake(spec_.width, spec_.height, 1) threadsPerThreadgroup:threads];
            [encoder barrierAfterEncoderStages:MTLStageDispatch
                           beforeEncoderStages:MTLStageDispatch
                             visibilityOptions:MTL4VisibilityOptionNone];
        }

        [encoder setComputePipelineState:atlas_pipeline_];
        for (uint32_t index = 0; index < source_count; ++index) {
            [encoder setArgumentTable:atlas_argument_tables4_[index + 1U]];
            [encoder dispatchThreads:MTLSizeMake(sources[index].destination.width, sources[index].destination.height, 1)
                threadsPerThreadgroup:threads];
            if (index + 1U < source_count) {
                [encoder barrierAfterEncoderStages:MTLStageDispatch
                               beforeEncoderStages:MTLStageDispatch
                                 visibilityOptions:MTL4VisibilityOptionNone];
            }
        }
        [encoder endEncoding];
        [command_buffer endCommandBuffer];
        id<MTL4CommandBuffer> buffers[] = {command_buffer};
        [queue4_ commit:buffers count:1];
        [queue4_ signalEvent:completion_event_ value:sequence_];
        return true;
    }

    void count_completion() noexcept {
        if (sequence_ != 0 && counted_sequence_ != sequence_ && idle()) {
            counted_sequence_ = sequence_;
            ++stats_.completed;
        }
    }
};

ImagePreprocessor::ImagePreprocessor() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    ::new (static_cast<void*>(storage_.data())) Impl{};
}

ImagePreprocessor::~ImagePreprocessor() {
    impl().~Impl();
}

ImagePreprocessor::Impl& ImagePreprocessor::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const ImagePreprocessor::Impl& ImagePreprocessor::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult ImagePreprocessor::initialize(void* metal_device, const char* metallib_path, PathPreference preference,
                                            const TensorSpec& spec) noexcept {
    Impl& state = impl();
    if (metal_device == nullptr || !tensor_spec_valid(spec) ||
        (preference != PathPreference::automatic && preference != PathPreference::metal3 &&
         preference != PathPreference::metal4)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    state.device_ = (__bridge id<MTLDevice>)metal_device;
    state.spec_ = spec;
    state.stats_.format = spec.format;
    state.stats_.output_width = spec.width;
    state.stats_.output_height = spec.height;
    if (spec.format == TensorFormat::direct_texture) {
        const bool supports_metal4 = state.metal4_supported();
        if (preference == PathPreference::metal4 && !supports_metal4) {
            return SACCADE_ERROR_UNSUPPORTED;
        }
        state.stats_.path =
            preference == PathPreference::metal3 ? Path::metal3 : (supports_metal4 ? Path::metal4 : Path::metal3);
        state.initialized_ = true;
        return SACCADE_OK;
    }
    if (metallib_path == nullptr || metallib_path[0] == '\0' || !required_output_bytes(spec, &state.output_bytes_)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const bool supports_metal4 = state.metal4_supported();
    if (preference == PathPreference::metal4 && !supports_metal4) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    if (!state.create_resources() || !state.create_pipeline(metallib_path)) {
        return SACCADE_ERROR_BACKEND;
    }
    const bool use_metal4 =
        preference == PathPreference::metal4 || (preference == PathPreference::automatic && supports_metal4);
    if (use_metal4) {
        if (@available(macOS 26.0, *)) {
            if (!state.create_metal4()) {
                if (preference == PathPreference::metal4) {
                    return SACCADE_ERROR_BACKEND;
                }
                state.discard_metal4();
                if (!state.create_metal3()) {
                    return SACCADE_ERROR_BACKEND;
                }
                state.stats_.path = Path::metal3;
            } else {
                state.stats_.path = Path::metal4;
            }
        } else {
            return SACCADE_ERROR_UNSUPPORTED;
        }
    } else {
        if (!state.create_metal3()) {
            return SACCADE_ERROR_BACKEND;
        }
        state.stats_.path = Path::metal3;
    }
    state.completion_event_ = [state.device_ newSharedEvent];
    if (state.completion_event_ == nil) {
        return SACCADE_ERROR_BACKEND;
    }
    state.stats_.output_bytes = state.output_bytes_;
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::direct_texture(void* texture_pointer, uint32_t width, uint32_t height,
                                                DirectTextureView* output) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || state.spec_.format != TensorFormat::direct_texture || output == nullptr ||
        texture_pointer == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_pointer;
    if (!state.texture_valid(texture, width, height)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {texture_pointer, width, height, SACCADE_FORMAT_BGRA8, 0};
    ++state.stats_.direct_texture_views;
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::submit(void* texture_pointer, uint32_t width, uint32_t height, SourceRegion region,
                                        uint64_t frame_id, uint64_t transform_epoch,
                                        PreprocessSubmission* output) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || state.spec_.format == TensorFormat::direct_texture || texture_pointer == nullptr ||
        output == nullptr || frame_id == 0 || transform_epoch == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_pointer;
    if (!state.texture_valid(texture, width, height)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (region.width == 0 || region.height == 0) {
        region = {0, 0, width, height};
    }
    const uint64_t right = static_cast<uint64_t>(region.x) + region.width;
    const uint64_t bottom = static_cast<uint64_t>(region.y) + region.height;
    if (region.width == 0 || region.height == 0 || right > width || bottom > height) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state.count_completion();
    if (!state.idle()) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    if (state.sequence_ == std::numeric_limits<uint64_t>::max()) {
        return SACCADE_ERROR_CAPACITY;
    }
    const PreprocessParameters parameters = state.make_parameters(width, height, region);
    state.content_region_ = {
        static_cast<uint32_t>(parameters.content_rect[0]), static_cast<uint32_t>(parameters.content_rect[1]),
        static_cast<uint32_t>(parameters.content_rect[2]), static_cast<uint32_t>(parameters.content_rect[3])};
    std::memcpy(static_cast<std::byte*>(state.parameters_.contents) + preprocess_parameter_offset, &parameters,
                sizeof(parameters));
    ++state.sequence_;
    state.frame_id_ = frame_id;
    state.transform_epoch_ = transform_epoch;
    bool encoded = false;
    if (state.stats_.path == Path::metal4) {
        if (@available(macOS 26.0, *)) {
            encoded = state.encode_metal4(texture);
        }
    } else {
        encoded = state.encode_metal3(texture);
    }
    if (!encoded) {
        ++state.stats_.failures;
        state.sequence_ = 0;
        return SACCADE_ERROR_BACKEND;
    }
    if (state.stats_.path != Path::metal4) {
        state.source_textures_.fill(nil);
        state.source_textures_[0] = texture;
        state.source_texture_count_ = 1;
    }
    ++state.stats_.submissions;
    *output = {state.sequence_, frame_id, transform_epoch};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::submit_atlas(const AtlasSource* sources, uint32_t source_count, SourceRegion content,
                                              AtlasLoad load, uint64_t frame_id, uint64_t transform_epoch,
                                              PreprocessSubmission* output) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || state.spec_.format != TensorFormat::image_bgra8 || sources == nullptr ||
        source_count == 0 || source_count > atlas_source_capacity || output == nullptr || frame_id == 0 ||
        transform_epoch == 0 || (load != AtlasLoad::clear && load != AtlasLoad::preserve) ||
        !region_valid(content, state.spec_.width, state.spec_.height)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t content_right = static_cast<uint64_t>(content.x) + content.width;
    const uint64_t content_bottom = static_cast<uint64_t>(content.y) + content.height;
    for (uint32_t index = 0; index < source_count; ++index) {
        const AtlasSource& source = sources[index];
        id<MTLTexture> texture = (__bridge id<MTLTexture>)source.texture;
        const uint64_t destination_right = static_cast<uint64_t>(source.destination.x) + source.destination.width;
        const uint64_t destination_bottom = static_cast<uint64_t>(source.destination.y) + source.destination.height;
        if (!state.texture_valid(texture, source.texture_width, source.texture_height) ||
            !region_valid(source.source, source.texture_width, source.texture_height) ||
            !region_valid(source.destination, state.spec_.width, state.spec_.height) ||
            source.destination.x < content.x || source.destination.y < content.y || destination_right > content_right ||
            destination_bottom > content_bottom) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }

    state.count_completion();
    if (!state.idle()) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    if (state.sequence_ == std::numeric_limits<uint64_t>::max()) return SACCADE_ERROR_CAPACITY;

    auto* parameter_bytes = static_cast<std::byte*>(state.parameters_.contents);
    const AtlasClearParameters clear_parameters = state.make_atlas_clear_parameters();
    std::memcpy(parameter_bytes + atlas_clear_parameter_offset, &clear_parameters, sizeof(clear_parameters));
    for (uint32_t index = 0; index < source_count; ++index) {
        const AtlasParameters parameters = state.make_atlas_parameters(sources[index]);
        std::memcpy(parameter_bytes + atlas_parameter_offset + index * parameter_stride, &parameters,
                    sizeof(parameters));
    }

    state.content_region_ = content;
    ++state.sequence_;
    state.frame_id_ = frame_id;
    state.transform_epoch_ = transform_epoch;
    bool encoded = false;
    if (state.stats_.path == Path::metal4) {
        if (@available(macOS 26.0, *)) {
            encoded = state.encode_atlas_metal4(sources, source_count, load);
        }
    } else {
        encoded = state.encode_atlas_metal3(sources, source_count, load);
    }
    if (!encoded) {
        ++state.stats_.failures;
        state.sequence_ = 0;
        return SACCADE_ERROR_BACKEND;
    }

    if (state.stats_.path != Path::metal4) {
        state.source_textures_.fill(nil);
        for (uint32_t index = 0; index < source_count; ++index) {
            state.source_textures_[index] = (__bridge id<MTLTexture>)sources[index].texture;
        }
        state.source_texture_count_ = source_count;
    }
    ++state.stats_.submissions;
    ++state.stats_.atlas_submissions;
    state.stats_.atlas_sources += source_count;
    *output = {state.sequence_, frame_id, transform_epoch};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::poll(const PreprocessSubmission& submission, bool* complete) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || complete == nullptr || submission.sequence == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (submission.sequence != state.sequence_ || submission.frame_id != state.frame_id_ ||
        submission.transform_epoch != state.transform_epoch_) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *complete = state.idle();
    if (*complete) {
        state.count_completion();
    }
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::wait(const PreprocessSubmission& submission, uint64_t timeout_ns) noexcept {
    const auto begin = std::chrono::steady_clock::now();
    for (;;) {
        bool complete = false;
        const SaccadeResult result = poll(submission, &complete);
        if (result != SACCADE_OK || complete) {
            return result;
        }
        if (timeout_ns == 0) {
            return SACCADE_ERROR_TIMEOUT;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin);
        if (static_cast<uint64_t>(elapsed.count()) >= timeout_ns) {
            return SACCADE_ERROR_TIMEOUT;
        }
        std::this_thread::yield();
    }
}

SaccadeResult ImagePreprocessor::tensor(const PreprocessSubmission& submission, TensorView* output) noexcept {
    Impl& state = impl();
    if (output == nullptr || state.spec_.format == TensorFormat::image_bgra8) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    bool complete = false;
    const SaccadeResult result = poll(submission, &complete);
    if (result != SACCADE_OK) {
        return result;
    }
    if (!complete) {
        return SACCADE_ERROR_BUSY;
    }
    const size_t element_bytes = state.spec_.format == TensorFormat::planar_fp16 ? 2U : 1U;
    const size_t plane_stride = static_cast<size_t>(state.spec_.width) * state.spec_.height * element_bytes;
    *output = {(__bridge void*)state.output_,
               state.output_bytes_,
               plane_stride,
               state.spec_.width,
               state.spec_.height,
               state.spec_.format,
               3};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::image(const PreprocessSubmission& submission, ImageView* output) noexcept {
    Impl& state = impl();
    if (output == nullptr || state.spec_.format != TensorFormat::image_bgra8) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    bool complete = false;
    const SaccadeResult result = poll(submission, &complete);
    if (result != SACCADE_OK) return result;
    if (!complete) return SACCADE_ERROR_BUSY;
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(state.output_pixel_buffer_);
    if (surface == nullptr) return SACCADE_ERROR_BACKEND;
    *output = {state.output_pixel_buffer_,
               (__bridge void*)state.output_texture_,
               IOSurfaceGetID(surface),
               state.spec_.width,
               state.spec_.height,
               SACCADE_FORMAT_BGRA8,
               0,
               state.content_region_};
    return SACCADE_OK;
}

SaccadeResult ImagePreprocessor::memory_stats(SaccadeMemoryStats* output) const noexcept {
    const Impl& state = impl();
    if (!state.initialized_ || output == nullptr || output->struct_size != sizeof(SaccadeMemoryStats) ||
        output->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeMemoryStats value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.host_committed = sizeof(Impl);
    value.device_owned = allocated_bytes(state.parameters_) + state.output_bytes_;
    if (@available(macOS 26.0, *)) {
        if (state.allocator4_ != nil) {
            value.framework_opaque = [state.allocator4_ allocatedSize];
        }
    }
    value.high_water_bytes = value.host_committed + value.device_owned + value.framework_opaque;
    *output = value;
    return SACCADE_OK;
}

PreprocessorStats ImagePreprocessor::stats() const noexcept {
    PreprocessorStats value = impl().stats_;
    if (@available(macOS 26.0, *)) {
        if (impl().allocator4_ != nil) {
            value.command_allocator_bytes = [impl().allocator4_ allocatedSize];
        }
    }
    return value;
}

} // namespace saccade::backend::metal

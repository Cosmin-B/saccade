#include "backends/metal/overlay_expander.hpp"

#include "overlay/packet.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>

namespace saccade::backend::metal {
namespace {

constexpr uint32_t slot_count = 3;
constexpr uint32_t instances_per_target = 5;
constexpr uint32_t instance_capacity = SACCADE_OVERLAY_MAX_TARGETS * instances_per_target + 1U;
constexpr size_t packet_capacity = sizeof(SaccadeOverlayPacketHeader) +
                                   SACCADE_OVERLAY_MAX_TARGETS * sizeof(SaccadeOverlayTarget) +
                                   SACCADE_OVERLAY_MAX_STYLES * sizeof(SaccadeOverlayStyle);

struct ExpandParameters {
    uint32_t target_count;
    uint32_t active_target_index;
    uint32_t static_instance_count;
    uint32_t has_active_target;
};

struct DrawArguments {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t vertex_start;
    uint32_t base_instance;
};

struct DisplayConstants {
    float inverse_drawable_width;
    float inverse_drawable_height;
    float animation_time_seconds;
    float scene_age_seconds;
};

static_assert(sizeof(ExpandParameters) == 16);
static_assert(sizeof(DrawArguments) == 16);
static_assert(sizeof(DisplayConstants) == 16);

bool all_zero(const uint64_t* values, size_t count) noexcept {
    for (size_t index = 0; index < count; ++index) {
        if (values[index] != 0) {
            return false;
        }
    }
    return true;
}

bool frame_header_valid(const SaccadeOverlayFrameDesc& frame) noexcept {
    return frame.struct_size == sizeof(SaccadeOverlayFrameDesc) && frame.api_version == SACCADE_API_VERSION &&
           (frame.flags & ~SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) == 0 &&
           all_zero(frame.reserved, std::size(frame.reserved));
}

template <class Record> Record load_record(const uint8_t* bytes) noexcept {
    Record record{};
    std::memcpy(&record, bytes, sizeof(record));
    return record;
}

uint64_t buffer_bytes(id<MTLBuffer> buffer) noexcept {
    return buffer == nil ? 0 : static_cast<uint64_t>(buffer.allocatedSize);
}

void fill_builtin_atlas(uint8_t* pixels) noexcept {
    constexpr uint64_t glyph_bits[overlay::glyph_atlas_capacity] = {
        0x4631FC62EULL, 0x3E317C62FULL, 0x78210843EULL, 0x3E318C62FULL, 0x7C217843FULL, 0x04217843FULL, 0x7A31E843EULL,
        0x4631FC631ULL, 0x7C842109FULL, 0x19294211CULL, 0x452519531ULL, 0x7C2108421ULL, 0x4631AD771ULL, 0x4631CD671ULL,
        0x3A318C62EULL, 0x04217C62FULL, 0x59358C62EULL, 0x45257C62FULL, 0x3E107043EULL, 0x10842109FULL, 0x3A318C631ULL,
        0x11518C631ULL, 0x2AB5AC631ULL, 0x462A22A31ULL, 0x108422A31ULL, 0x7C222221FULL, 0x3A33AE62EULL, 0x3884210C4ULL,
        0x7C444422EULL, 0x3E107420FULL, 0x211F4A988ULL, 0x3E107843FULL};
    std::memset(pixels, 0, overlay::glyph_atlas_bytes);
    for (uint32_t glyph = 0; glyph < overlay::glyph_atlas_capacity; ++glyph) {
        const uint32_t cell_x = (glyph % overlay::glyph_atlas_columns) * overlay::glyph_atlas_cell_width;
        const uint32_t cell_y = (glyph / overlay::glyph_atlas_columns) * overlay::glyph_atlas_cell_height;
        for (uint32_t y = 0; y < 56; ++y) {
            const uint32_t row = y * 7U / 56U;
            for (uint32_t x = 0; x < 40; ++x) {
                const uint32_t column = x * 5U / 40U;
                if ((glyph_bits[glyph] & (UINT64_C(1) << (row * 5U + column))) != 0) {
                    pixels[static_cast<size_t>(cell_y + y + 4U) * overlay::glyph_atlas_width + cell_x + x + 12U] =
                        UINT8_MAX;
                }
            }
        }
    }
}

} // namespace

struct OverlayExpander::Impl {
    struct Slot {
        id<MTLBuffer> packet_ = nil;
        id<MTLBuffer> rects_ = nil;
        id<MTLBuffer> metadata_ = nil;
        id<MTLBuffer> parameters_ = nil;
        id<MTLBuffer> arguments_ = nil;
        id<MTLBuffer> display_constants_ = nil;
        id argument_table_ = nil;
        id allocator_ = nil;
        id command_buffer4_ = nil;
        id<MTLFence> fence4_ = nil;
        id residency_set_ = nil;
        id render_pass4_ = nil;
        MTLRenderPassDescriptor* render_pass3_ = nil;
        id<MTLCommandBuffer> command_buffer3_ = nil;
        id<MTLTexture> render_texture_ = nil;
        id<MTLDrawable> drawable_ = nil;
        uint64_t sequence_ = 0;
        uint64_t scene_epoch_ = 0;
        uint64_t transform_epoch_ = 0;
        uint32_t instance_count_ = 0;
        bool scene_valid_ = false;
    };

    id<MTLDevice> device_ = nil;
    id<MTLLibrary> library_ = nil;
    id<MTLComputePipelineState> static_pipeline_ = nil;
    id<MTLComputePipelineState> active_pipeline_ = nil;
    id<MTLRenderPipelineState> render_pipeline_ = nil;
    id<MTLBuffer> glyph_atlas_buffer_ = nil;
    id<MTLTexture> glyph_atlas_texture_ = nil;
    id<MTLBuffer> validated_packet_ = nil;
    id<MTLCommandQueue> queue3_ = nil;
    id queue4_ = nil;
    id<MTLSharedEvent> completion_event_ = nil;
    std::array<Slot, slot_count> slots_{};
    Stats stats_{};
    SaccadeOverlayPacketHeader validated_header_{};
    size_t validated_packet_size_ = 0;
    uint64_t next_sequence_ = 1;
    uint64_t animation_scene_epoch_ = 0;
    double animation_scene_start_ = 0.0;
    uint32_t next_slot_ = 0;
    bool initialized_ = false;
    bool validated_scene_ = false;

    ~Impl() {
        bool retired = true;
        if (completion_event_ != nil && next_sequence_ > 1) {
            const uint64_t last_sequence = next_sequence_ - 1;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (completion_event_.signaledValue < last_sequence) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    retired = false;
                    break;
                }
                std::this_thread::yield();
            }
        }
        if (@available(macOS 26.0, *)) {
            if (retired && queue4_ != nil) {
                for (Slot& slot : slots_) {
                    if (slot.residency_set_ != nil) {
                        [queue4_ removeResidencySet:slot.residency_set_];
                        [slot.residency_set_ endResidency];
                    }
                }
            }
        }
    }

    bool metal4_supported() const noexcept {
        if (@available(macOS 26.0, *)) {
            return [device_ supportsFamily:MTLGPUFamilyMetal4];
        }
        return false;
    }

    bool create_buffers() noexcept {
        constexpr MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
        validated_packet_ = [device_ newBufferWithLength:packet_capacity options:options];
        glyph_atlas_buffer_ = [device_ newBufferWithLength:overlay::glyph_atlas_bytes options:options];
        MTLTextureDescriptor* atlas_descriptor = [[MTLTextureDescriptor alloc] init];
        atlas_descriptor.textureType = MTLTextureType2D;
        atlas_descriptor.pixelFormat = MTLPixelFormatR8Unorm;
        atlas_descriptor.width = overlay::glyph_atlas_width;
        atlas_descriptor.height = overlay::glyph_atlas_height;
        atlas_descriptor.depth = 1;
        atlas_descriptor.mipmapLevelCount = 1;
        atlas_descriptor.arrayLength = 1;
        atlas_descriptor.sampleCount = 1;
        atlas_descriptor.storageMode = MTLStorageModeShared;
        atlas_descriptor.usage = MTLTextureUsageShaderRead;
        glyph_atlas_texture_ = [glyph_atlas_buffer_ newTextureWithDescriptor:atlas_descriptor
                                                                      offset:0
                                                                 bytesPerRow:overlay::glyph_atlas_width];
        if (validated_packet_ == nil || glyph_atlas_buffer_ == nil || glyph_atlas_texture_ == nil) {
            return false;
        }
        fill_builtin_atlas(static_cast<uint8_t*>(glyph_atlas_buffer_.contents));
        for (Slot& slot : slots_) {
            slot.packet_ = [device_ newBufferWithLength:packet_capacity options:options];
            slot.rects_ =
                [device_ newBufferWithLength:static_cast<NSUInteger>(instance_capacity) * sizeof(SaccadeOverlayRect)
                                     options:options];
            slot.metadata_ = [device_
                newBufferWithLength:static_cast<NSUInteger>(instance_capacity) * sizeof(SaccadeOverlayInstanceMeta)
                            options:options];
            slot.parameters_ = [device_ newBufferWithLength:sizeof(ExpandParameters) options:options];
            slot.arguments_ = [device_ newBufferWithLength:sizeof(DrawArguments) options:options];
            slot.display_constants_ = [device_ newBufferWithLength:sizeof(DisplayConstants) options:options];
            if (slot.packet_ == nil || slot.rects_ == nil || slot.metadata_ == nil || slot.parameters_ == nil ||
                slot.arguments_ == nil || slot.display_constants_ == nil) {
                return false;
            }
            slot.render_pass3_ = [MTLRenderPassDescriptor renderPassDescriptor];
            if (slot.render_pass3_ == nil) {
                return false;
            }
        }
        return true;
    }

    bool create_pipelines(const char* metallib_path) noexcept {
        NSString* path = [NSString stringWithUTF8String:metallib_path];
        if (path == nil) {
            return false;
        }
        NSError* error = nil;
        library_ = [device_ newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (library_ == nil) {
            return false;
        }
        id<MTLFunction> static_function = [library_ newFunctionWithName:@"saccade_expand_static"];
        id<MTLFunction> active_function = [library_ newFunctionWithName:@"saccade_update_active"];
        id<MTLFunction> vertex_function = [library_ newFunctionWithName:@"saccade_overlay_vertex"];
        id<MTLFunction> fragment_function = [library_ newFunctionWithName:@"saccade_overlay_fragment"];
        if (static_function == nil || active_function == nil || vertex_function == nil || fragment_function == nil) {
            return false;
        }
        static_pipeline_ = [device_ newComputePipelineStateWithFunction:static_function error:&error];
        active_pipeline_ = [device_ newComputePipelineStateWithFunction:active_function error:&error];

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex_function;
        descriptor.fragmentFunction = fragment_function;
        MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
        color.pixelFormat = MTLPixelFormatBGRA8Unorm;
        color.blendingEnabled = YES;
        color.rgbBlendOperation = MTLBlendOperationAdd;
        color.alphaBlendOperation = MTLBlendOperationAdd;
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        render_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
        return static_pipeline_ != nil && active_pipeline_ != nil && render_pipeline_ != nil;
    }

    bool create_metal4_state() noexcept API_AVAILABLE(macos(26.0)) {
        queue4_ = [device_ newMTL4CommandQueue];
        if (queue4_ == nil) {
            return false;
        }

        MTL4ArgumentTableDescriptor* table_descriptor = [[MTL4ArgumentTableDescriptor alloc] init];
        table_descriptor.maxBufferBindCount = 7;
        table_descriptor.maxTextureBindCount = 1;
        table_descriptor.initializeBindings = YES;
        for (Slot& slot : slots_) {
            slot.argument_table_ = [device_ newArgumentTableWithDescriptor:table_descriptor error:nil];
            slot.allocator_ = [device_ newCommandAllocator];
            slot.command_buffer4_ = [device_ newCommandBuffer];
            slot.fence4_ = [device_ newFence];
            slot.render_pass4_ = [[MTL4RenderPassDescriptor alloc] init];
            if (slot.argument_table_ == nil || slot.allocator_ == nil || slot.command_buffer4_ == nil ||
                slot.fence4_ == nil || slot.render_pass4_ == nil) {
                return false;
            }
            MTLResidencySetDescriptor* residency_descriptor = [[MTLResidencySetDescriptor alloc] init];
            residency_descriptor.initialCapacity = 8;
            NSError* error = nil;
            slot.residency_set_ = [device_ newResidencySetWithDescriptor:residency_descriptor error:&error];
            if (slot.residency_set_ == nil) {
                return false;
            }
            const std::array<id<MTLAllocation>, 6> allocations{
                slot.packet_, slot.rects_, slot.metadata_, slot.parameters_, slot.arguments_, slot.display_constants_};
            [slot.residency_set_ addAllocations:allocations.data() count:allocations.size()];
            [slot.residency_set_ addAllocation:glyph_atlas_texture_];
            [slot.residency_set_ commit];
            [slot.residency_set_ requestResidency];
            [queue4_ addResidencySet:slot.residency_set_];
            [static_cast<id<MTL4ArgumentTable>>(slot.argument_table_) setTexture:glyph_atlas_texture_.gpuResourceID
                                                                         atIndex:0];
        }
        return true;
    }

    void discard_metal4_state() noexcept API_AVAILABLE(macos(26.0)) {
        for (Slot& slot : slots_) {
            if (queue4_ != nil && slot.residency_set_ != nil) {
                [queue4_ removeResidencySet:slot.residency_set_];
                [slot.residency_set_ endResidency];
            }
            slot.argument_table_ = nil;
            slot.allocator_ = nil;
            slot.command_buffer4_ = nil;
            slot.fence4_ = nil;
            slot.residency_set_ = nil;
            slot.render_pass4_ = nil;
        }
        queue4_ = nil;
    }

    bool create_metal3_state() noexcept {
        queue3_ = [device_ newCommandQueueWithMaxCommandBufferCount:slot_count];
        return queue3_ != nil;
    }

    SaccadeResult validate_frame(const SaccadeOverlayFrameDesc& frame,
                                 SaccadeOverlayPacketHeader* out_header) noexcept {
        if (!frame_header_valid(frame) || frame.packet.data == nullptr ||
            frame.packet.size < sizeof(SaccadeOverlayPacketHeader) || out_header == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        const SaccadeOverlayPacketHeader header = load_record<SaccadeOverlayPacketHeader>(frame.packet.data);
        if (frame.scene_epoch != header.scene_epoch || frame.transform_epoch != header.transform_epoch) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        const bool has_active = (frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0;
        if (has_active && frame.active_target_index >= header.target_count) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        if (!validated_scene_ || validated_header_.scene_epoch != header.scene_epoch ||
            validated_header_.transform_epoch != header.transform_epoch) {
            overlay::PacketView view{};
            const SaccadeResult result = overlay::validate_packet(frame.packet, &view);
            if (result != SACCADE_OK) {
                return result;
            }
            validated_header_ = view.header;
            validated_packet_size_ = frame.packet.size;
            std::memcpy(validated_packet_.contents, frame.packet.data, frame.packet.size);
            validated_scene_ = true;
        } else if (frame.packet.size != validated_packet_size_ ||
                   std::memcmp(&header, &validated_header_, sizeof(header)) != 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_header = header;
        return SACCADE_OK;
    }

    Slot* acquire_slot(uint32_t* out_index) noexcept {
        const uint64_t completed = completion_event_.signaledValue;
        for (uint32_t attempt = 0; attempt < slot_count; ++attempt) {
            const uint32_t index = (next_slot_ + attempt) % slot_count;
            Slot& slot = slots_[index];
            if (slot.sequence_ == 0 || slot.sequence_ <= completed) {
                next_slot_ = (index + 1U) % slot_count;
                *out_index = index;
                return &slot;
            }
        }
        ++stats_.busy_submissions;
        return nullptr;
    }

    void bind_metal4_slot(Slot& slot, const SaccadeOverlayPacketHeader& header) noexcept API_AVAILABLE(macos(26.0)) {
        [slot.argument_table_ setAddress:slot.packet_.gpuAddress + header.targets_offset atIndex:0];
        [slot.argument_table_ setAddress:slot.packet_.gpuAddress + header.styles_offset atIndex:1];
        [slot.argument_table_ setAddress:slot.rects_.gpuAddress atIndex:2];
        [slot.argument_table_ setAddress:slot.metadata_.gpuAddress atIndex:3];
        [slot.argument_table_ setAddress:slot.parameters_.gpuAddress atIndex:4];
        [slot.argument_table_ setAddress:slot.arguments_.gpuAddress atIndex:5];
        [slot.argument_table_ setAddress:slot.display_constants_.gpuAddress atIndex:6];
    }

    bool render_target_valid(const RenderTarget& target) const noexcept {
        if (target.texture == nullptr || target.width == 0 || target.height == 0 ||
            (target.flags & ~render_target_display_link) != 0 || target.reserved != 0 ||
            !std::isfinite(target.target_presentation_time) || target.target_presentation_time < 0.0) {
            return false;
        }
        id<MTLTexture> texture = (__bridge id<MTLTexture>)target.texture;
        return texture != nil && texture.device == device_ && texture.pixelFormat == MTLPixelFormatBGRA8Unorm &&
               (texture.usage & MTLTextureUsageRenderTarget) != 0 && texture.width == target.width &&
               texture.height == target.height;
    }

    void prepare_render_target(Slot& slot, const RenderTarget& target, uint64_t scene_epoch) noexcept {
        float animation_time = 0.0F;
        float scene_age = 1.0F;
        if (target.target_presentation_time > 0.0) {
            if (animation_scene_epoch_ != scene_epoch) {
                animation_scene_epoch_ = scene_epoch;
                animation_scene_start_ = target.target_presentation_time;
            }
            animation_time = static_cast<float>(std::fmod(target.target_presentation_time, 4.0));
            scene_age = static_cast<float>(std::max(0.0, target.target_presentation_time - animation_scene_start_));
        }
        const DisplayConstants constants{1.0F / static_cast<float>(target.width),
                                         1.0F / static_cast<float>(target.height), animation_time, scene_age};
        std::memcpy(slot.display_constants_.contents, &constants, sizeof(constants));
        slot.render_texture_ = (__bridge id<MTLTexture>)target.texture;
        slot.drawable_ = target.drawable == nullptr ? nil : (__bridge id<MTLDrawable>)target.drawable;
        MTLRenderPassColorAttachmentDescriptor* color = slot.render_pass3_.colorAttachments[0];
        color.texture = slot.render_texture_;
        color.loadAction = MTLLoadActionClear;
        color.storeAction = MTLStoreActionStore;
        color.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    }

    void prepare_metal4_render_target(Slot& slot) noexcept API_AVAILABLE(macos(26.0)) {
        MTLRenderPassColorAttachmentDescriptor* color =
            static_cast<MTL4RenderPassDescriptor*>(slot.render_pass4_).colorAttachments[0];
        color.texture = slot.render_texture_;
        color.loadAction = MTLLoadActionClear;
        color.storeAction = MTLStoreActionStore;
        color.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        if (![slot.residency_set_ containsAllocation:slot.render_texture_]) {
            [slot.residency_set_ addAllocation:slot.render_texture_];
            [slot.residency_set_ commit];
        }
    }

    bool encode_metal4(Slot& slot, bool expand_static, uint32_t target_count, const RenderTarget* target) noexcept
        API_AVAILABLE(macos(26.0)) {
        id<MTL4CommandBuffer> command_buffer = static_cast<id<MTL4CommandBuffer>>(slot.command_buffer4_);
        if (slot.sequence_ != 0) {
            [slot.allocator_ reset];
        }
        [command_buffer beginCommandBufferWithAllocator:slot.allocator_];
        id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        [encoder setArgumentTable:slot.argument_table_];
        if (expand_static && target_count != 0) {
            [encoder setComputePipelineState:static_pipeline_];
            const NSUInteger width = std::min<NSUInteger>(256, static_pipeline_.maxTotalThreadsPerThreadgroup);
            [encoder dispatchThreads:MTLSizeMake(target_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        }
        [encoder setComputePipelineState:active_pipeline_];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        if (target != nullptr) {
            [encoder updateFence:slot.fence4_ afterEncoderStages:MTLStageDispatch];
        }
        [encoder endEncoding];

        if (target != nullptr) {
            prepare_metal4_render_target(slot);
            id<MTL4RenderCommandEncoder> render = [command_buffer
                renderCommandEncoderWithDescriptor:static_cast<MTL4RenderPassDescriptor*>(slot.render_pass4_)];
            if (render == nil) {
                return false;
            }
            [render waitForFence:slot.fence4_ beforeEncoderStages:(MTLStageVertex | MTLStageFragment)];
            [render setRenderPipelineState:render_pipeline_];
            [render setArgumentTable:slot.argument_table_ atStages:(MTLRenderStageVertex | MTLRenderStageFragment)];
            [render drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:slot.arguments_.gpuAddress];
            [render endEncoding];
        }
        [command_buffer endCommandBuffer];
        if (slot.drawable_ != nil) {
            [queue4_ waitForDrawable:slot.drawable_];
        }
        id<MTL4CommandBuffer> buffers[] = {command_buffer};
        [queue4_ commit:buffers count:1];
        if (slot.drawable_ != nil) {
            [queue4_ signalDrawable:slot.drawable_];
            if ((target->flags & render_target_display_link) != 0) {
                [slot.drawable_ present];
            } else if (target->target_presentation_time > 0.0) {
                [slot.drawable_ presentAtTime:target->target_presentation_time];
            } else {
                [slot.drawable_ present];
            }
        }
        [queue4_ signalEvent:completion_event_ value:next_sequence_];
        return true;
    }

    bool encode_metal3(Slot& slot, bool expand_static, const SaccadeOverlayPacketHeader& header,
                       const RenderTarget* target) noexcept {
        slot.command_buffer3_ = [queue3_ commandBuffer];
        if (slot.command_buffer3_ == nil) {
            return false;
        }
        id<MTLComputeCommandEncoder> encoder = [slot.command_buffer3_ computeCommandEncoder];
        if (encoder == nil) {
            return false;
        }
        [encoder setBuffer:slot.packet_ offset:header.targets_offset atIndex:0];
        [encoder setBuffer:slot.packet_ offset:header.styles_offset atIndex:1];
        [encoder setBuffer:slot.rects_ offset:0 atIndex:2];
        [encoder setBuffer:slot.metadata_ offset:0 atIndex:3];
        [encoder setBuffer:slot.parameters_ offset:0 atIndex:4];
        [encoder setBuffer:slot.arguments_ offset:0 atIndex:5];
        if (expand_static && header.target_count != 0) {
            [encoder setComputePipelineState:static_pipeline_];
            const NSUInteger width = std::min<NSUInteger>(256, static_pipeline_.maxTotalThreadsPerThreadgroup);
            [encoder dispatchThreads:MTLSizeMake(header.target_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        }
        [encoder setComputePipelineState:active_pipeline_];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];

        if (target != nullptr) {
            id<MTLRenderCommandEncoder> render =
                [slot.command_buffer3_ renderCommandEncoderWithDescriptor:slot.render_pass3_];
            if (render == nil) {
                return false;
            }
            [render setRenderPipelineState:render_pipeline_];
            [render setVertexBuffer:slot.rects_ offset:0 atIndex:2];
            [render setVertexBuffer:slot.metadata_ offset:0 atIndex:3];
            [render setVertexBuffer:slot.display_constants_ offset:0 atIndex:6];
            [render setFragmentBuffer:slot.packet_ offset:header.targets_offset atIndex:0];
            [render setFragmentBuffer:slot.packet_ offset:header.styles_offset atIndex:1];
            [render setFragmentBuffer:slot.display_constants_ offset:0 atIndex:6];
            [render setFragmentTexture:glyph_atlas_texture_ atIndex:0];
            [render drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:slot.arguments_ indirectBufferOffset:0];
            [render endEncoding];
        }
        [slot.command_buffer3_ encodeSignalEvent:completion_event_ value:next_sequence_];
        if (slot.drawable_ != nil) {
            if ((target->flags & render_target_display_link) != 0) {
                [slot.command_buffer3_ presentDrawable:slot.drawable_];
            } else if (target->target_presentation_time > 0.0) {
                [slot.command_buffer3_ presentDrawable:slot.drawable_ atTime:target->target_presentation_time];
            } else {
                [slot.command_buffer3_ presentDrawable:slot.drawable_];
            }
        }
        [slot.command_buffer3_ commit];
        return true;
    }
};

OverlayExpander::OverlayExpander() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    new (storage_.data()) Impl{};
}

OverlayExpander::~OverlayExpander() {
    impl().~Impl();
}

OverlayExpander::Impl& OverlayExpander::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlayExpander::Impl& OverlayExpander::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlayExpander::initialize(const char* metallib_path, PathPreference preference) noexcept {
    Impl& state = impl();
    if (metallib_path == nullptr || metallib_path[0] == '\0' ||
        (preference != PathPreference::automatic && preference != PathPreference::metal3 &&
         preference != PathPreference::metal4)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state.initialized_) {
        return SACCADE_ERROR_STATE;
    }

    @autoreleasepool {
        state.device_ = MTLCreateSystemDefaultDevice();
        if (state.device_ == nil) {
            return SACCADE_ERROR_UNSUPPORTED;
        }
        const bool supports_metal4 = state.metal4_supported();
        if (preference == PathPreference::metal4 && !supports_metal4) {
            return SACCADE_ERROR_UNSUPPORTED;
        }
        const bool use_metal4 =
            preference == PathPreference::metal4 || (preference == PathPreference::automatic && supports_metal4);
        if (!state.create_buffers() || !state.create_pipelines(metallib_path)) {
            return SACCADE_ERROR_BACKEND;
        }
        if (use_metal4) {
            if (@available(macOS 26.0, *)) {
                if (!state.create_metal4_state()) {
                    if (preference == PathPreference::metal4) {
                        return SACCADE_ERROR_BACKEND;
                    }
                    state.discard_metal4_state();
                    if (!state.create_metal3_state()) {
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
            if (!state.create_metal3_state()) {
                return SACCADE_ERROR_BACKEND;
            }
            state.stats_.path = Path::metal3;
        }
        state.completion_event_ = [state.device_ newSharedEvent];
        if (state.completion_event_ == nil) {
            return SACCADE_ERROR_BACKEND;
        }
    }

    state.stats_.slot_count = slot_count;
    state.stats_.target_capacity = SACCADE_OVERLAY_MAX_TARGETS;
    state.stats_.instance_capacity = instance_capacity;
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlayExpander::submit(const SaccadeOverlayFrameDesc& frame, Submission* out_submission) noexcept {
    return submit_internal(frame, nullptr, out_submission);
}

SaccadeResult OverlayExpander::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || !overlay::glyph_atlas_valid(atlas)) return SACCADE_ERROR_INVALID_ARGUMENT;

    if (state.next_sequence_ > 1 && state.completion_event_.signaledValue < state.next_sequence_ - 1U) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (state.completion_event_.signaledValue < state.next_sequence_ - 1U) {
            if (std::chrono::steady_clock::now() >= deadline) return SACCADE_ERROR_TIMEOUT;
            std::this_thread::yield();
        }
    }
    std::memcpy(state.glyph_atlas_buffer_.contents, atlas.pixels, overlay::glyph_atlas_bytes);
    return SACCADE_OK;
}

SaccadeResult OverlayExpander::render(const SaccadeOverlayFrameDesc& frame, const RenderTarget& target,
                                      Submission* out_submission) noexcept {
    return submit_internal(frame, &target, out_submission);
}

SaccadeResult OverlayExpander::submit_internal(const SaccadeOverlayFrameDesc& frame, const RenderTarget* target,
                                               Submission* out_submission) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || out_submission == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (target != nullptr && !state.render_target_valid(*target)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeOverlayPacketHeader header{};
    const SaccadeResult validated = state.validate_frame(frame, &header);
    if (validated != SACCADE_OK) {
        return validated;
    }

    uint32_t slot_index = 0;
    Impl::Slot* slot = state.acquire_slot(&slot_index);
    if (slot == nullptr) {
        return SACCADE_ERROR_BUSY;
    }
    const bool expand_static = !slot->scene_valid_ || slot->scene_epoch_ != header.scene_epoch ||
                               slot->transform_epoch_ != header.transform_epoch;
    if (expand_static) {
        std::memcpy(slot->packet_.contents, state.validated_packet_.contents, state.validated_packet_size_);
        state.stats_.packet_upload_bytes += frame.packet.size;
    }

    const bool has_active = (frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0;
    const ExpandParameters parameters{header.target_count,
                                      has_active ? frame.active_target_index : SACCADE_OVERLAY_ACTIVE_TARGET_NONE,
                                      header.target_count * instances_per_target, has_active ? 1U : 0U};
    std::memcpy(slot->parameters_.contents, &parameters, sizeof(parameters));
    if (target != nullptr) {
        if (@available(macOS 26.0, *)) {
            if (state.stats_.path == Path::metal4 && slot->render_texture_ != nil &&
                slot->render_texture_ != (__bridge id<MTLTexture>)target->texture) {
                [slot->residency_set_ removeAllocation:slot->render_texture_];
            }
        }
        state.prepare_render_target(*slot, *target, header.scene_epoch);
    } else {
        slot->drawable_ = nil;
        if (state.stats_.path != Path::metal4) {
            slot->render_texture_ = nil;
        }
    }

    bool encoded = false;
    if (state.stats_.path == Path::metal4) {
        if (@available(macOS 26.0, *)) {
            if (expand_static) {
                state.bind_metal4_slot(*slot, header);
            }
            encoded = state.encode_metal4(*slot, expand_static, header.target_count, target);
        } else {
            return SACCADE_ERROR_UNSUPPORTED;
        }
    } else {
        encoded = state.encode_metal3(*slot, expand_static, header, target);
    }
    if (!encoded) {
        state.stats_.render_failures += target != nullptr ? 1U : 0U;
        return SACCADE_ERROR_BACKEND;
    }

    const uint64_t sequence = state.next_sequence_++;
    slot->sequence_ = sequence;
    slot->scene_epoch_ = header.scene_epoch;
    slot->transform_epoch_ = header.transform_epoch;
    slot->scene_valid_ = true;
    slot->instance_count_ = parameters.static_instance_count + parameters.has_active_target;
    ++state.stats_.submissions;
    state.stats_.static_dispatches += expand_static && header.target_count != 0 ? 1U : 0U;
    ++state.stats_.active_dispatches;
    if (target != nullptr) {
        ++state.stats_.rendered_frames;
        ++state.stats_.draw_calls;
        state.stats_.presented_frames += target->drawable != nullptr ? 1U : 0U;
    }
    *out_submission = {sequence, header.scene_epoch, slot_index, slot->instance_count_};
    return SACCADE_OK;
}

SaccadeResult OverlayExpander::poll(const Submission& submission, bool* out_complete) const noexcept {
    const Impl& state = impl();
    if (!state.initialized_ || out_complete == nullptr || submission.sequence == 0 ||
        submission.slot_index >= slot_count) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot& slot = state.slots_[submission.slot_index];
    if (slot.sequence_ != submission.sequence || slot.scene_epoch_ != submission.scene_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *out_complete = state.completion_event_.signaledValue >= submission.sequence;
    return SACCADE_OK;
}

SaccadeResult OverlayExpander::wait(const Submission& submission, uint64_t timeout_ns) const noexcept {
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

SaccadeResult OverlayExpander::copy_instances(const Submission& submission, InstanceSpan output,
                                              size_t* out_count) const noexcept {
    const Impl& state = impl();
    if (out_count == nullptr || submission.slot_index >= slot_count || submission.sequence == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot& slot = state.slots_[submission.slot_index];
    if (slot.sequence_ != submission.sequence || slot.scene_epoch_ != submission.scene_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state.completion_event_.signaledValue < submission.sequence) {
        return SACCADE_ERROR_BUSY;
    }
    *out_count = slot.instance_count_;
    if (output.capacity < slot.instance_count_) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (slot.instance_count_ != 0 && (output.rects == nullptr || output.metadata == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (slot.instance_count_ != 0) {
        std::memcpy(output.rects, slot.rects_.contents,
                    static_cast<size_t>(slot.instance_count_) * sizeof(SaccadeOverlayRect));
        std::memcpy(output.metadata, slot.metadata_.contents,
                    static_cast<size_t>(slot.instance_count_) * sizeof(SaccadeOverlayInstanceMeta));
    }
    return SACCADE_OK;
}

SaccadeResult OverlayExpander::memory_stats(SaccadeMemoryStats* output) const noexcept {
    const Impl& state = impl();
    if (!state.initialized_ || output == nullptr || output->struct_size != sizeof(SaccadeMemoryStats) ||
        output->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint64_t device_owned = buffer_bytes(state.validated_packet_) + buffer_bytes(state.glyph_atlas_buffer_);
    for (const Impl::Slot& slot : state.slots_) {
        device_owned += buffer_bytes(slot.packet_) + buffer_bytes(slot.rects_) + buffer_bytes(slot.metadata_) +
                        buffer_bytes(slot.parameters_) + buffer_bytes(slot.arguments_) +
                        buffer_bytes(slot.display_constants_);
    }
    uint64_t framework_opaque = 0;
    if (@available(macOS 26.0, *)) {
        for (const Impl::Slot& slot : state.slots_) {
            if (slot.allocator_ != nil) {
                framework_opaque += [slot.allocator_ allocatedSize];
            }
        }
    }
    SaccadeMemoryStats result{};
    result.struct_size = sizeof(result);
    result.api_version = SACCADE_API_VERSION;
    result.host_committed = sizeof(Impl);
    result.device_owned = device_owned;
    result.framework_opaque = framework_opaque;
    result.copied_bytes = state.stats_.packet_upload_bytes;
    result.high_water_bytes = result.host_committed + result.device_owned + result.framework_opaque;
    *output = result;
    return SACCADE_OK;
}

Stats OverlayExpander::stats() const noexcept {
    Stats result = impl().stats_;
    if (@available(macOS 26.0, *)) {
        for (const Impl::Slot& slot : impl().slots_) {
            if (slot.allocator_ != nil) {
                result.command_allocator_bytes += [slot.allocator_ allocatedSize];
            }
            if (slot.residency_set_ != nil && [slot.residency_set_ allocationCount] > 7) {
                result.resident_render_targets += static_cast<uint32_t>([slot.residency_set_ allocationCount] - 7U);
            }
        }
    }
    return result;
}

void* OverlayExpander::native_device() const noexcept {
    return impl().initialized_ ? (__bridge void*)impl().device_ : nullptr;
}

} // namespace saccade::backend::metal

#include "model/artifact.hpp"
#include "model/coreml_contract.hpp"
#include "platform/macos/coreml_provider.hpp"
#include "scene/packet.hpp"

#if defined(SACCADE_TEST_METAL_PREPROCESSOR)
#include "backends/metal/preprocessor.hpp"
#endif

#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVPixelBufferIOSurface.h>
#include <IOSurface/IOSurface.h>
#if defined(SACCADE_TEST_METAL_PREPROCESSOR)
#import <Metal/Metal.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    provider_admission_failed,
    runtime_failed,
    provider_registration_failed,
    session_failed,
    pixel_buffer_failed,
    frame_import_failed,
    submit_failed,
    inference_failed,
    packet_failed,
    memory_stats_failed,
    cancellation_failed,
    cleanup_failed
};

constexpr uint64_t stable_id = 701;
constexpr uint32_t input_width = 320;
constexpr uint32_t input_height = 320;
constexpr uint32_t maximum_targets = 8;
constexpr uint32_t candidate_capacity = 16;
constexpr uint16_t minimum_confidence_q16 = 4096;
constexpr uint16_t iou_threshold_q16 = 32768;
constexpr uint64_t frame_id = 702;
constexpr uint64_t model_epoch = 703;
constexpr uint64_t session_epoch = 704;
constexpr uint64_t transform_epoch = 705;
constexpr uint64_t topology_epoch = 706;
constexpr uint64_t source_id = 707;
constexpr uint64_t cancellation_frame_id = 708;
constexpr uint64_t inference_timeout_ns = UINT64_C(5'000'000'000);
constexpr uint32_t provider_base_capabilities = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                                SACCADE_PROVIDER_CAPABILITY_ASYNC |
                                                SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
constexpr uint8_t locator[] = "coreml-contract-fixture.mlmodelc";
constexpr uint8_t input_name[] = "image";
constexpr uint8_t rows_name[] = "targets";
constexpr uint8_t count_name[] = "count";
constexpr size_t locator_size = sizeof(locator) - 1U;
constexpr size_t input_name_size = sizeof(input_name) - 1U;
constexpr size_t rows_name_size = sizeof(rows_name) - 1U;
constexpr size_t count_name_size = sizeof(count_name) - 1U;
constexpr size_t payload_size =
    saccade::model::coreml::payload_header_bytes + locator_size + input_name_size + rows_name_size + count_name_size;
constexpr size_t payload_offset = saccade::model::artifact_header_bytes;
constexpr size_t signature_offset = payload_offset + payload_size;
constexpr size_t artifact_size = signature_offset + saccade::model::artifact_signature_bytes;
constexpr size_t output_size = sizeof(SaccadeTargetPacketHeader) + maximum_targets * sizeof(SaccadeTargetRecord);
constexpr std::array<uint8_t, 32> bundle_sha256{0x35, 0xac, 0x63, 0x1f, 0xd2, 0x0e, 0x68, 0x76, 0xea, 0x29, 0x07,
                                                0x4b, 0xd6, 0x4e, 0x1b, 0xab, 0x67, 0xbc, 0x68, 0x48, 0x97, 0x91,
                                                0x40, 0x88, 0x34, 0xc1, 0xd8, 0x7e, 0xc2, 0x90, 0x3a, 0x07};

constexpr size_t artifact_version_offset = 4;
constexpr size_t artifact_header_size_offset = 8;
constexpr size_t artifact_total_size_offset = 12;
constexpr size_t artifact_stable_id_offset = 16;
constexpr size_t artifact_graph_offset = 24;
constexpr size_t artifact_kind_offset = 28;
constexpr size_t artifact_precision_offset = 32;
constexpr size_t artifact_width_offset = 36;
constexpr size_t artifact_height_offset = 40;
constexpr size_t artifact_channels_offset = 44;
constexpr size_t artifact_maximum_targets_offset = 48;
constexpr size_t artifact_maximum_output_offset = 52;
constexpr size_t artifact_payload_offset_offset = 56;
constexpr size_t artifact_payload_size_offset = 64;
constexpr size_t artifact_signature_offset_offset = 72;
constexpr size_t artifact_signature_size_offset = 80;
constexpr size_t artifact_flags_offset = 84;
constexpr size_t artifact_compatibility_offset = 88;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void write_u32(uint8_t* bytes, size_t offset, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void write_u64(uint8_t* bytes, size_t offset, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

std::array<uint8_t, artifact_size> artifact_bytes() noexcept {
    std::array<uint8_t, artifact_size> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = 'D';
    write_u32(bytes.data(), artifact_version_offset, saccade::model::artifact_version);
    write_u32(bytes.data(), artifact_header_size_offset, saccade::model::artifact_header_bytes);
    write_u32(bytes.data(), artifact_total_size_offset, static_cast<uint32_t>(bytes.size()));
    write_u64(bytes.data(), artifact_stable_id_offset, stable_id);
    write_u32(bytes.data(), artifact_graph_offset, static_cast<uint32_t>(saccade::model::GraphKind::ui_detector));
    write_u32(bytes.data(), artifact_kind_offset,
              static_cast<uint32_t>(saccade::model::ArtifactKind::coreml_compiled_bundle));
    write_u32(bytes.data(), artifact_precision_offset, SACCADE_PRECISION_FP16);
    write_u32(bytes.data(), artifact_width_offset, input_width);
    write_u32(bytes.data(), artifact_height_offset, input_height);
    write_u32(bytes.data(), artifact_channels_offset, 3);
    write_u32(bytes.data(), artifact_maximum_targets_offset, maximum_targets);
    write_u32(bytes.data(), artifact_maximum_output_offset, static_cast<uint32_t>(output_size));
    write_u64(bytes.data(), artifact_payload_offset_offset, payload_offset);
    write_u64(bytes.data(), artifact_payload_size_offset, payload_size);
    write_u64(bytes.data(), artifact_signature_offset_offset, signature_offset);
    write_u32(bytes.data(), artifact_signature_size_offset, saccade::model::artifact_signature_bytes);
    write_u32(bytes.data(), artifact_flags_offset,
              saccade::model::artifact_has_signature | saccade::model::artifact_relative_locator);
    write_u64(bytes.data(), artifact_compatibility_offset, saccade::model::coreml::provider_compatibility_bit);

    uint8_t* payload = bytes.data() + payload_offset;
    payload[0] = 'S';
    payload[1] = 'C';
    payload[2] = 'M';
    payload[3] = 'C';
    write_u32(payload, 4, saccade::model::coreml::contract_version);
    write_u32(payload, 8, static_cast<uint32_t>(saccade::model::coreml::InputKind::image_bgra8));
    write_u32(payload, 12, static_cast<uint32_t>(saccade::model::coreml::OutputLayout::normalized_target_rows_v1));
    write_u32(payload, 16, candidate_capacity);
    write_u32(payload, 20, maximum_targets);
    write_u32(payload, 24, minimum_confidence_q16);
    write_u32(payload, 28, iou_threshold_q16);
    write_u32(payload, 32, static_cast<uint32_t>(locator_size));
    write_u32(payload, 36, static_cast<uint32_t>(input_name_size));
    write_u32(payload, 40, static_cast<uint32_t>(rows_name_size));
    write_u32(payload, 44, static_cast<uint32_t>(count_name_size));
    std::memcpy(payload + 48, bundle_sha256.data(), bundle_sha256.size());
    uint8_t* strings = payload + saccade::model::coreml::payload_header_bytes;
    std::memcpy(strings, locator, locator_size);
    strings += locator_size;
    std::memcpy(strings, input_name, input_name_size);
    strings += input_name_size;
    std::memcpy(strings, rows_name, rows_name_size);
    strings += rows_name_size;
    std::memcpy(strings, count_name, count_name_size);
    return bytes;
}

SaccadeResult verify_artifact(void*, const saccade::model::ArtifactView& artifact) noexcept {
    return artifact.stable_id == stable_id && artifact.signature.size == saccade::model::artifact_signature_bytes
               ? SACCADE_OK
               : SACCADE_ERROR_PERMISSION;
}

CVPixelBufferRef create_pixel_buffer() noexcept {
    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    if (empty == nullptr) return nullptr;
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {empty};
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(empty);
    if (attributes == nullptr) return nullptr;
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVReturn created = CVPixelBufferCreate(kCFAllocatorDefault, input_width, input_height,
                                                 kCVPixelFormatType_32BGRA, attributes, &pixel_buffer);
    CFRelease(attributes);
    return created == kCVReturnSuccess ? pixel_buffer : nullptr;
}

template <typename Structure> Structure output_structure() noexcept {
    Structure value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

#if defined(SACCADE_TEST_METAL_PREPROCESSOR)
id<MTLTexture> source_texture(id<MTLDevice> device) {
    MTLTextureDescriptor* description =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:2
                                                          height:2
                                                       mipmapped:NO];
    description.storageMode = MTLStorageModeShared;
    description.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:description];
    const std::array<uint8_t, 16> pixels{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:8];
    return texture;
}
#endif

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        saccade::platform::macos::CoreMlInferenceProvider provider;
        if (provider.initialize({}) != SACCADE_ERROR_INVALID_ARGUMENT) {
            return result(TestResult::provider_admission_failed);
        }
        if (argc == 1) {
            using saccade::platform::macos::CoreMlComputePolicy;
            constexpr std::array policies{CoreMlComputePolicy::cpu_only, CoreMlComputePolicy::cpu_and_gpu,
                                          CoreMlComputePolicy::cpu_and_neural_engine, CoreMlComputePolicy::all};
            std::array<uint32_t, policies.size()> capabilities{};
            for (size_t index = 0; index < policies.size(); ++index) {
                const saccade::platform::macos::CoreMlProviderConfig config{
                    "/", policies[index], false, {}, {nullptr, verify_artifact}};
                if (provider.initialize(config) != SACCADE_OK) return result(TestResult::provider_admission_failed);
                const SaccadeInferenceProviderDesc description = provider.descriptor();
                SaccadeDeviceInfo device = output_structure<SaccadeDeviceInfo>();
                if (description.ops.enumerate_devices(description.context, 0, &device) != SACCADE_OK ||
                    device.capability_bits != description.info.capability_bits || provider.shutdown() != SACCADE_OK) {
                    return result(TestResult::provider_admission_failed);
                }
                capabilities[index] = description.info.capability_bits;
            }
            const uint32_t cpu = capabilities[0];
            const uint32_t cpu_gpu = capabilities[1];
            const uint32_t cpu_accelerator = capabilities[2];
            const uint32_t all = capabilities[3];
            const bool truthful = cpu == (provider_base_capabilities | SACCADE_PROVIDER_CAPABILITY_CPU) &&
                                  (cpu_gpu & SACCADE_PROVIDER_CAPABILITY_CPU) != 0 &&
                                  (cpu_gpu & SACCADE_PROVIDER_CAPABILITY_ACCELERATOR) == 0 &&
                                  (cpu_accelerator & SACCADE_PROVIDER_CAPABILITY_CPU) != 0 &&
                                  (cpu_accelerator & SACCADE_PROVIDER_CAPABILITY_GPU) == 0 &&
                                  all == (cpu_gpu | cpu_accelerator);
            return truthful ? result(TestResult::success) : result(TestResult::provider_admission_failed);
        }

        const saccade::platform::macos::CoreMlProviderConfig provider_config{
            argv[1], saccade::platform::macos::CoreMlComputePolicy::all, false, {}, {nullptr, verify_artifact}};
        if (provider.initialize(provider_config) != SACCADE_OK) {
            return result(TestResult::provider_admission_failed);
        }

        SaccadeRuntimeDesc runtime_description = output_structure<SaccadeRuntimeDesc>();
        SaccadeRuntimeHandle runtime = 0;
        if (saccade_runtime_create(&runtime_description, &runtime) != SACCADE_OK) {
            return result(TestResult::runtime_failed);
        }
        const SaccadeInferenceProviderDesc provider_description = provider.descriptor();
        if (saccade_register_inference_provider(runtime, &provider_description) != SACCADE_OK ||
            saccade_runtime_freeze(runtime) != SACCADE_OK) {
            return result(TestResult::provider_registration_failed);
        }

        const auto artifact = artifact_bytes();
        SaccadeInferenceSessionDesc session_description = output_structure<SaccadeInferenceSessionDesc>();
        session_description.model_bytes = {artifact.data(), artifact.size()};
        session_description.model_stable_id = stable_id;
        session_description.provider_stable_id = provider_description.info.stable_id;
        session_description.required_capability_bits =
            SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_ACCELERATOR |
            SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC |
            SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
        session_description.required_format_bits = SACCADE_FORMAT_BGRA8;
        session_description.required_precision_bits = SACCADE_PRECISION_FP16;
        session_description.required_import_bits = SACCADE_IMPORT_IOSURFACE;
        session_description.queue_capacity = 1;
        session_description.max_in_flight = 1;
        SaccadeInferenceSessionInfo session_info = output_structure<SaccadeInferenceSessionInfo>();
        SaccadeExecutionContextHandle session = 0;
        if (saccade_inference_session_create(runtime, &session_description, &session, &session_info) != SACCADE_OK ||
            session == 0 || session_info.max_output_bytes != output_size) {
            return result(TestResult::session_failed);
        }

        CVPixelBufferRef pixel_buffer = nullptr;
#if defined(SACCADE_TEST_METAL_PREPROCESSOR)
        saccade::backend::metal::ImagePreprocessor preprocessor;
        if (argc >= 3) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            saccade::backend::metal::TensorSpec spec{};
            spec.width = input_width;
            spec.height = input_height;
            spec.format = saccade::backend::metal::TensorFormat::image_bgra8;
            saccade::backend::metal::PreprocessSubmission submission{};
            saccade::backend::metal::ImageView image{};
            id<MTLTexture> texture = source_texture(device);
            if (device == nil || texture == nil ||
                preprocessor.initialize((__bridge void*)device, argv[2],
                                        saccade::backend::metal::PathPreference::automatic, spec) != SACCADE_OK ||
                preprocessor.submit((__bridge void*)texture, 2, 2, {}, frame_id, transform_epoch, &submission) !=
                    SACCADE_OK ||
                preprocessor.wait(submission, inference_timeout_ns) != SACCADE_OK ||
                preprocessor.image(submission, &image) != SACCADE_OK) {
                return result(TestResult::pixel_buffer_failed);
            }
            pixel_buffer = static_cast<CVPixelBufferRef>(image.pixel_buffer);
            CVPixelBufferRetain(pixel_buffer);
        } else {
            pixel_buffer = create_pixel_buffer();
        }
#else
        pixel_buffer = create_pixel_buffer();
#endif
        if (pixel_buffer == nullptr) return result(TestResult::pixel_buffer_failed);
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel_buffer);
        if (surface == nullptr) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::pixel_buffer_failed);
        }
        SaccadeIOSurfaceFrameDesc frame_description = output_structure<SaccadeIOSurfaceFrameDesc>();
        frame_description.iosurface_id = IOSurfaceGetID(surface);
        frame_description.pixel_format = SACCADE_FORMAT_BGRA8;
        frame_description.width = input_width;
        frame_description.height = input_height;
        frame_description.frame_id = frame_id;
        frame_description.transform_epoch = transform_epoch;
        SaccadeFrameHandle frame = 0;
        if (saccade_frame_import(runtime, &frame_description, &frame) != SACCADE_OK || frame == 0) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::frame_import_failed);
        }

        SaccadeInferenceSubmitDesc submit = output_structure<SaccadeInferenceSubmitDesc>();
        submit.frame = frame;
        submit.scope = {0, 0, static_cast<int32_t>(input_width), static_cast<int32_t>(input_height)};
        submit.output_capacity = session_info.max_output_bytes;
        submit.model_epoch = model_epoch;
        submit.session_epoch = session_epoch;
        submit.transform_epoch = transform_epoch;
        submit.topology_epoch = topology_epoch;
        submit.source_id = source_id;
        SaccadeTicketHandle ticket = 0;
        if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK || ticket == 0) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::submit_failed);
        }

        SaccadeInferenceStatus status = output_structure<SaccadeInferenceStatus>();
        const SaccadeResult waited = saccade_inference_wait(runtime, session, ticket, inference_timeout_ns, &status);
        if (waited != SACCADE_OK || status.state != SACCADE_TICKET_COMPLETE || status.frame_id != frame_id ||
            status.model_epoch != model_epoch || status.session_epoch != session_epoch ||
            status.transform_epoch != transform_epoch || status.topology_epoch != topology_epoch ||
            status.source_id != source_id || status.produced_bytes == 0) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::inference_failed);
        }

        alignas(SaccadeTargetPacketHeader) std::array<uint8_t, output_size> output{};
        size_t required = 0;
        if (saccade_inference_collect(runtime, session, ticket, {output.data(), output.size()}, &required) !=
                SACCADE_OK ||
            required != status.produced_bytes) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::inference_failed);
        }
        saccade::scene::PacketView packet{};
        if (saccade::scene::validate_packet({output.data(), required}, &packet) != SACCADE_OK ||
            packet.header->target_count != 2 || packet.header->frame_id != frame_id ||
            packet.header->model_epoch != model_epoch || packet.header->session_epoch != session_epoch ||
            packet.header->transform_epoch != transform_epoch || packet.header->topology_epoch != topology_epoch ||
            packet.targets[0].x_q8 != 10240 || packet.targets[0].y_q8 != 20480 || packet.targets[0].width_q8 != 40960 ||
            packet.targets[0].height_q8 != 20480 ||
            packet.targets[1].capability_bits != (SACCADE_TARGET_CAPABILITY_POINTER_MOVE |
                                                  SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_TEXT)) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::packet_failed);
        }

        SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
        if (saccade_inference_memory_stats(runtime, session, &memory) != SACCADE_OK || memory.host_committed == 0 ||
            memory.host_reserved == 0 || memory.copied_bytes != required) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::memory_stats_failed);
        }
        if (saccade_frame_release(runtime, frame) != SACCADE_OK) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::cleanup_failed);
        }
        frame_description.frame_id = cancellation_frame_id;
        frame = 0;
        if (saccade_frame_import(runtime, &frame_description, &frame) != SACCADE_OK || frame == 0) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::cancellation_failed);
        }
        submit.frame = frame;
        ticket = 0;
        if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK || ticket == 0 ||
            saccade_inference_cancel(runtime, session, ticket) != SACCADE_OK ||
            saccade_inference_collect(runtime, session, ticket, {nullptr, 0}, &required) != SACCADE_ERROR_CANCELLED ||
            saccade_frame_release(runtime, frame) != SACCADE_OK) {
            CVPixelBufferRelease(pixel_buffer);
            return result(TestResult::cancellation_failed);
        }
        const bool cleaned = saccade_inference_session_destroy(runtime, session) == SACCADE_OK &&
                             saccade_runtime_destroy(runtime) == SACCADE_OK && provider.shutdown() == SACCADE_OK;
        CVPixelBufferRelease(pixel_buffer);
        return cleaned ? result(TestResult::success) : result(TestResult::cleanup_failed);
    }
}

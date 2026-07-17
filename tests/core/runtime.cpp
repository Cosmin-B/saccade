#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include "backends/reference_cpu/reference_cpu.hpp"
#include "../support/allocation_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVPixelBufferIOSurface.h>
#include <IOSurface/IOSurface.h>
#endif

namespace {

SaccadeInferenceOps inference_ops() {
    SaccadeInferenceOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = +[](void*, uint32_t, SaccadeDeviceInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.query_model = +[](void*, SaccadeSpanU8, SaccadeModelInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.create_model = +[](void*, const SaccadeModelDesc*, SaccadeModelHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.destroy_model = +[](void*, SaccadeModelHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.create_context = +[](void*, const SaccadeExecutionContextDesc*,
                             SaccadeExecutionContextHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.destroy_context = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.submit = +[](void*, SaccadeExecutionContextHandle, const SaccadeInferenceDispatchDesc*,
                     SaccadeTicketHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.poll = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.wait = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, uint64_t,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.collect = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, SaccadeMutableSpanU8,
                      size_t*) -> SaccadeResult { return SACCADE_OK; };
    ops.cancel = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.reset = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, SaccadeExecutionContextHandle, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats =
        +[](void*, SaccadeExecutionContextHandle, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeInferenceProviderDesc inference_provider(uint64_t id) {
    static const uint8_t name[] = "runtime-test";
    SaccadeInferenceProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info.struct_size = static_cast<uint32_t>(sizeof(desc.info));
    desc.info.api_version = SACCADE_API_VERSION;
    desc.info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    desc.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU;
    desc.info.stable_id = id;
    desc.info.name = {name, sizeof(name) - 1};
    desc.ops = inference_ops();
    return desc;
}

SaccadeRuntimeDesc runtime_desc() {
    SaccadeRuntimeDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    return desc;
}

} // namespace

int main() {
    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    if (saccade_runtime_create(nullptr, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 1;
    }

    SaccadeRuntimeDesc malformed = runtime_desc();
    SaccadeRuntimeHandle runtime = 0;
    malformed.struct_size = 0;
    if (saccade_runtime_create(&malformed, &runtime) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 2;
    }
    malformed = runtime_desc();
    malformed.api_version = UINT32_C(0x00020000);
    if (saccade_runtime_create(&malformed, &runtime) != SACCADE_ERROR_VERSION) {
        return 3;
    }
    malformed = runtime_desc();
    malformed.reserved[0] = 1;
    if (saccade_runtime_create(&malformed, &runtime) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 4;
    }

    SaccadeRuntimeDesc desc = runtime_desc();
    if (saccade_runtime_create(&desc, &runtime) != SACCADE_OK || runtime == 0) {
        return 5;
    }

    SaccadeInferenceProviderDesc provider = inference_provider(1);
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_OK) {
        return 6;
    }
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_ERROR_ALREADY_EXISTS ||
        saccade_last_error().size == 0) {
        return 7;
    }
    saccade::backend::reference_cpu::Backend reference_backend;
    SaccadeInferenceProviderDesc reference_provider = reference_backend.provider();
    if (saccade_register_inference_provider(runtime, &reference_provider) != SACCADE_OK) {
        return 21;
    }

    // Begin frame-domain validation after provider registration is complete.
    uint8_t pixel[4] = {0, 0, 0, 0};
    SaccadeHostFrameDesc frame_desc{};
    frame_desc.struct_size = static_cast<uint32_t>(sizeof(frame_desc));
    frame_desc.api_version = SACCADE_API_VERSION;
    frame_desc.data = {pixel, sizeof(pixel)};
    frame_desc.width = 1;
    frame_desc.height = 1;
    frame_desc.row_stride_bytes = 4;
    frame_desc.pixel_format = 1;
    frame_desc.frame_id = 1;
    frame_desc.transform_epoch = 1;

    SaccadeRuntimeHandle other_runtime = 0;
    if (saccade_runtime_create(&desc, &other_runtime) != SACCADE_OK) {
        return 8;
    }
    SaccadeFrameHandle domain_frame = 0;
    SaccadeFrameHandle other_domain_frame = 0;
    if (saccade_frame_import(runtime, &frame_desc, &domain_frame) != SACCADE_OK ||
        saccade_frame_import(other_runtime, &frame_desc, &other_domain_frame) != SACCADE_OK ||
        domain_frame == other_domain_frame ||
        saccade_frame_release(other_runtime, domain_frame) != SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, other_domain_frame) != SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, domain_frame) != SACCADE_OK ||
        saccade_frame_release(other_runtime, other_domain_frame) != SACCADE_OK ||
        saccade_runtime_destroy(other_runtime) != SACCADE_OK) {
        return 9;
    }

    SaccadeFrameHandle frame = 99;
    SaccadeFrameHandle newest_frame = 99;
    saccade::test::begin_allocation_tracking();
    const SaccadeResult import_result = saccade_frame_import(runtime, &frame_desc, &frame);
    frame_desc.frame_id = 2;
    const SaccadeResult replacement_result = saccade_frame_import(runtime, &frame_desc, &newest_frame);
    const SaccadeResult release_result = saccade_frame_release(runtime, frame);
    const SaccadeResult newest_release_result = saccade_frame_release(runtime, newest_frame);
    const size_t frame_allocations = saccade::test::end_allocation_tracking();
    if (import_result != SACCADE_OK || replacement_result != SACCADE_OK || frame == 0 || newest_frame == 0 ||
        frame == newest_frame || release_result != SACCADE_OK || newest_release_result != SACCADE_OK ||
        frame_allocations != 0 || saccade_frame_release(runtime, frame) != SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, newest_frame) != SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, 0) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 10;
    }

    uint8_t undersized_pixels[2] = {0, 0};
    SaccadeHostFrameDesc undersized = frame_desc;
    undersized.data = {undersized_pixels, sizeof(undersized_pixels)};
    undersized.width = 2;
    undersized.height = 1;
    undersized.row_stride_bytes = 2;
    frame = 99;
    if (saccade_frame_import(runtime, &undersized, &frame) != SACCADE_ERROR_INVALID_ARGUMENT || frame != 0) {
        return 11;
    }

    // Continue with native-frame validation only after host-frame checks pass.
    SaccadeIOSurfaceFrameDesc iosurface{};
    iosurface.struct_size = static_cast<uint32_t>(sizeof(iosurface));
    iosurface.api_version = SACCADE_API_VERSION;
    iosurface.pixel_format = 1;
    iosurface.width = 1;
    iosurface.height = 1;
    frame = 99;
#if defined(__APPLE__)
    CFDictionaryRef surface_properties = CFDictionaryCreate(
        kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {surface_properties};
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(surface_properties);
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVReturn create_result =
        CVPixelBufferCreate(kCFAllocatorDefault, 1, 1, kCVPixelFormatType_32BGRA, attributes, &pixel_buffer);
    CFRelease(attributes);
    if (create_result != kCVReturnSuccess || pixel_buffer == nullptr) {
        return 12;
    }
    IOSurfaceRef native_surface = CVPixelBufferGetIOSurface(pixel_buffer);
    if (native_surface == nullptr) {
        CVPixelBufferRelease(pixel_buffer);
        return 12;
    }
    iosurface.iosurface_id = IOSurfaceGetID(native_surface);
    if (saccade_frame_import(runtime, &iosurface, &frame) != SACCADE_OK || frame == 0) {
        CVPixelBufferRelease(pixel_buffer);
        return 12;
    }
    CVPixelBufferRelease(pixel_buffer);
    if (saccade_frame_release(runtime, frame) != SACCADE_OK) {
        return 12;
    }
#else
    iosurface.iosurface_id = 1;
    if (saccade_frame_import(runtime, &iosurface, &frame) != SACCADE_ERROR_UNSUPPORTED || frame != 0) {
        return 12;
    }
#endif

    if (saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return 13;
    }

    const auto model_bytes = saccade::backend::reference_cpu::encode_model({200, 1});
    SaccadeInferenceSessionDesc session_desc{};
    session_desc.struct_size = sizeof(session_desc);
    session_desc.api_version = SACCADE_API_VERSION;
    session_desc.model_bytes = {model_bytes.data(), model_bytes.size()};
    session_desc.model_stable_id = 77;
    session_desc.provider_stable_id = reference_provider.info.stable_id;
    session_desc.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT;
    session_desc.required_format_bits = SACCADE_FORMAT_BGRA8;
    session_desc.required_precision_bits = SACCADE_PRECISION_FP32;
    session_desc.required_import_bits = SACCADE_IMPORT_HOST;
    session_desc.queue_capacity = 1;
    session_desc.max_in_flight = 1;
    SaccadeInferenceSessionInfo session_info{};
    session_info.struct_size = sizeof(session_info);
    session_info.api_version = SACCADE_API_VERSION;
    SaccadeExecutionContextHandle session = 0;
    if (saccade_inference_session_create(runtime, &session_desc, &session, &session_info) != SACCADE_OK ||
        session == 0 || session_info.session != session ||
        session_info.provider_stable_id != reference_provider.info.stable_id ||
        session_info.max_output_bytes != saccade::backend::reference_cpu::maximum_output_size) {
        return 22;
    }

    frame_desc.frame_id = 30;
    frame_desc.transform_epoch = 30;
    if (saccade_frame_import(runtime, &frame_desc, &frame) != SACCADE_OK) {
        return 23;
    }
    SaccadeInferenceSubmitDesc submit{};
    submit.struct_size = sizeof(submit);
    submit.api_version = SACCADE_API_VERSION;
    submit.frame = frame;
    submit.scope = {0, 0, 1, 1};
    submit.output_capacity = session_info.max_output_bytes;
    submit.model_epoch = 40;
    submit.session_epoch = 41;
    submit.transform_epoch = 30;
    submit.topology_epoch = 42;
    submit.source_id = 43;
    SaccadeTicketHandle ticket = 0;
    if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK || ticket == 0 ||
        saccade_frame_release(runtime, frame) != SACCADE_ERROR_BUSY) {
        return 24;
    }
    SaccadeInferenceStatus inference_status{};
    inference_status.struct_size = sizeof(inference_status);
    inference_status.api_version = SACCADE_API_VERSION;
    if (saccade_inference_poll(runtime, session, ticket, &inference_status) != SACCADE_OK ||
        inference_status.ticket != ticket || inference_status.state != SACCADE_TICKET_COMPLETE ||
        inference_status.frame_id != 30 || inference_status.model_epoch != 40 || inference_status.session_epoch != 41 ||
        inference_status.transform_epoch != 30 || inference_status.topology_epoch != 42 ||
        inference_status.source_id != 43) {
        return 25;
    }
    std::array<uint8_t, saccade::backend::reference_cpu::maximum_output_size> inference_output{};
    size_t required = 0;
    if (saccade_inference_collect(runtime, session, ticket, {inference_output.data(), inference_output.size()},
                                  &required) != SACCADE_OK ||
        required == 0 || saccade_frame_release(runtime, frame) != SACCADE_OK) {
        return 26;
    }

    frame_desc.frame_id = 31;
    frame_desc.transform_epoch = 31;
    if (saccade_frame_import(runtime, &frame_desc, &frame) != SACCADE_OK) {
        return 27;
    }
    submit.frame = frame;
    submit.transform_epoch = 31;
    if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK ||
        saccade_inference_cancel(runtime, session, ticket) != SACCADE_OK ||
        saccade_inference_collect(runtime, session, ticket, {nullptr, 0}, &required) != SACCADE_ERROR_CANCELLED ||
        saccade_frame_release(runtime, frame) != SACCADE_OK) {
        return 28;
    }

    frame_desc.frame_id = 32;
    frame_desc.transform_epoch = 32;
    if (saccade_frame_import(runtime, &frame_desc, &frame) != SACCADE_OK) {
        return 29;
    }
    submit.frame = frame;
    submit.transform_epoch = 32;
    if (saccade_inference_submit(runtime, session, &submit, &ticket) != SACCADE_OK ||
        saccade_inference_reset(runtime, session) != SACCADE_OK ||
        saccade_frame_release(runtime, frame) != SACCADE_OK) {
        return 30;
    }
    SaccadeMemoryStats inference_memory{};
    inference_memory.struct_size = sizeof(inference_memory);
    inference_memory.api_version = SACCADE_API_VERSION;
    if (saccade_inference_memory_stats(runtime, session, &inference_memory) != SACCADE_OK ||
        saccade_inference_synchronize(runtime, session, 0) != SACCADE_OK ||
        saccade_inference_session_destroy(runtime, session) != SACCADE_OK) {
        return 31;
    }

    provider = inference_provider(2);
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_ERROR_STATE) {
        return 14;
    }
    if (saccade_runtime_destroy(runtime) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_ERROR_STALE_HANDLE) {
        return 15;
    }

    SaccadeRuntimeDesc smaller = runtime_desc();
    smaller.struct_size = static_cast<uint32_t>(offsetof(SaccadeRuntimeDesc, reserved));
    if (saccade_runtime_create(&smaller, &runtime) != SACCADE_OK || saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 16;
    }

    struct ExtendedRuntimeDesc {
        SaccadeRuntimeDesc current;
        uint64_t future[4];
    };

    ExtendedRuntimeDesc larger{};
    larger.current = runtime_desc();
    larger.current.struct_size = static_cast<uint32_t>(sizeof(larger));
    if (saccade_runtime_create(&larger.current, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 17;
    }

    alignas(SaccadeRuntimeDesc) std::array<std::byte, sizeof(SaccadeRuntimeDesc) + 1> runtime_storage{};
    desc = runtime_desc();
    std::memcpy(runtime_storage.data() + 1, &desc, sizeof(desc));
    const auto* misaligned_runtime = reinterpret_cast<const SaccadeRuntimeDesc*>(runtime_storage.data() + 1);
    if (saccade_runtime_create(misaligned_runtime, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 18;
    }

    if (saccade_runtime_create(&desc, &runtime) != SACCADE_OK) {
        return 19;
    }
    alignas(SaccadeHostFrameDesc) std::array<std::byte, sizeof(SaccadeHostFrameDesc) + 1> frame_storage{};
    std::memcpy(frame_storage.data() + 1, &frame_desc, sizeof(frame_desc));
    const auto* misaligned_frame = reinterpret_cast<const SaccadeHostFrameDesc*>(frame_storage.data() + 1);
    if (saccade_frame_import_host(runtime, misaligned_frame, &frame) != SACCADE_OK || frame == 0 ||
        saccade_frame_release(runtime, frame) != SACCADE_OK || saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 20;
    }

    return 0;
}

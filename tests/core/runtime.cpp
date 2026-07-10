#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include "../support/allocation_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

SaccadeInferenceOps inference_ops() {
    SaccadeInferenceOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = +[](void*, uint32_t, SaccadeDeviceInfo*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.query_model = +[](void*, SaccadeSpanU8, SaccadeModelInfo*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.create_model = +[](void*, const SaccadeModelDesc*, SaccadeModelHandle*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.destroy_model = +[](void*, SaccadeModelHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.create_context = +[](void*, const SaccadeExecutionContextDesc*,
                             SaccadeExecutionContextHandle*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.destroy_context = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.submit = +[](void*, SaccadeExecutionContextHandle,
                     const SaccadeInferenceSubmitDesc*, SaccadeTicketHandle*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.poll = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.wait = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, uint64_t,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.collect = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                      SaccadeMutableSpanU8, size_t*) -> SaccadeResult { return SACCADE_OK; };
    ops.cancel = +[](void*, SaccadeExecutionContextHandle,
                     SaccadeTicketHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.reset = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.synchronize = +[](void*, SaccadeExecutionContextHandle,
                          uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats = +[](void*, SaccadeExecutionContextHandle,
                           SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
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

}  // namespace

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
    if (saccade_register_inference_provider(runtime, &provider) !=
            SACCADE_ERROR_ALREADY_EXISTS ||
        saccade_last_error().size == 0) {
        return 7;
    }

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
        saccade_frame_import(other_runtime, &frame_desc, &other_domain_frame) !=
            SACCADE_OK ||
        domain_frame == other_domain_frame ||
        saccade_frame_release(other_runtime, domain_frame) !=
            SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, other_domain_frame) !=
            SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, domain_frame) != SACCADE_OK ||
        saccade_frame_release(other_runtime, other_domain_frame) != SACCADE_OK ||
        saccade_runtime_destroy(other_runtime) != SACCADE_OK) {
        return 9;
    }

    SaccadeFrameHandle frame = 99;
    SaccadeFrameHandle newest_frame = 99;
    saccade::test::begin_allocation_tracking();
    const SaccadeResult import_result =
        saccade_frame_import(runtime, &frame_desc, &frame);
    frame_desc.frame_id = 2;
    const SaccadeResult replacement_result =
        saccade_frame_import(runtime, &frame_desc, &newest_frame);
    const SaccadeResult release_result = saccade_frame_release(runtime, frame);
    const SaccadeResult newest_release_result =
        saccade_frame_release(runtime, newest_frame);
    const size_t frame_allocations = saccade::test::end_allocation_tracking();
    if (import_result != SACCADE_OK || replacement_result != SACCADE_OK ||
        frame == 0 || newest_frame == 0 || frame == newest_frame ||
        release_result != SACCADE_OK || newest_release_result != SACCADE_OK ||
        frame_allocations != 0 ||
        saccade_frame_release(runtime, frame) != SACCADE_ERROR_STALE_HANDLE ||
        saccade_frame_release(runtime, newest_frame) !=
            SACCADE_ERROR_STALE_HANDLE ||
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
    if (saccade_frame_import(runtime, &undersized, &frame) !=
            SACCADE_ERROR_INVALID_ARGUMENT ||
        frame != 0) {
        return 11;
    }

    SaccadeIOSurfaceFrameDesc iosurface{};
    iosurface.struct_size = static_cast<uint32_t>(sizeof(iosurface));
    iosurface.api_version = SACCADE_API_VERSION;
    iosurface.iosurface_id = 1;
    iosurface.pixel_format = 1;
    iosurface.width = 1;
    iosurface.height = 1;
    frame = 99;
    if (saccade_frame_import(runtime, &iosurface, &frame) !=
            SACCADE_ERROR_UNSUPPORTED ||
        frame != 0) {
        return 12;
    }

    if (saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return 13;
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
    if (saccade_runtime_create(&smaller, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
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

    alignas(SaccadeRuntimeDesc)
        std::array<std::byte, sizeof(SaccadeRuntimeDesc) + 1> runtime_storage{};
    desc = runtime_desc();
    std::memcpy(runtime_storage.data() + 1, &desc, sizeof(desc));
    const auto* misaligned_runtime = reinterpret_cast<const SaccadeRuntimeDesc*>(
        runtime_storage.data() + 1);
    if (saccade_runtime_create(misaligned_runtime, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 18;
    }

    if (saccade_runtime_create(&desc, &runtime) != SACCADE_OK) {
        return 19;
    }
    alignas(SaccadeHostFrameDesc)
        std::array<std::byte, sizeof(SaccadeHostFrameDesc) + 1> frame_storage{};
    std::memcpy(frame_storage.data() + 1, &frame_desc, sizeof(frame_desc));
    const auto* misaligned_frame = reinterpret_cast<const SaccadeHostFrameDesc*>(
        frame_storage.data() + 1);
    if (saccade_frame_import_host(runtime, misaligned_frame, &frame) !=
            SACCADE_OK ||
        frame == 0 || saccade_frame_release(runtime, frame) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 20;
    }

    return 0;
}

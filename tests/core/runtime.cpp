#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

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
    SaccadeFrameHandle frame = 99;
    if (saccade_frame_import(runtime, &frame_desc, &frame) !=
            SACCADE_ERROR_UNSUPPORTED ||
        frame != 0) {
        return 8;
    }

    if (saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return 9;
    }
    provider = inference_provider(2);
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_ERROR_STATE) {
        return 10;
    }
    if (saccade_runtime_destroy(runtime) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_ERROR_STALE_HANDLE) {
        return 11;
    }

    SaccadeRuntimeDesc smaller = runtime_desc();
    smaller.struct_size = static_cast<uint32_t>(offsetof(SaccadeRuntimeDesc, reserved));
    if (saccade_runtime_create(&smaller, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 12;
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
        return 13;
    }

    alignas(SaccadeRuntimeDesc)
        std::array<std::byte, sizeof(SaccadeRuntimeDesc) + 1> runtime_storage{};
    desc = runtime_desc();
    std::memcpy(runtime_storage.data() + 1, &desc, sizeof(desc));
    const auto* misaligned_runtime = reinterpret_cast<const SaccadeRuntimeDesc*>(
        runtime_storage.data() + 1);
    if (saccade_runtime_create(misaligned_runtime, &runtime) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 14;
    }

    if (saccade_runtime_create(&desc, &runtime) != SACCADE_OK) {
        return 15;
    }
    alignas(SaccadeHostFrameDesc)
        std::array<std::byte, sizeof(SaccadeHostFrameDesc) + 1> frame_storage{};
    std::memcpy(frame_storage.data() + 1, &frame_desc, sizeof(frame_desc));
    const auto* misaligned_frame = reinterpret_cast<const SaccadeHostFrameDesc*>(
        frame_storage.data() + 1);
    if (saccade_frame_import_host(runtime, misaligned_frame, &frame) !=
            SACCADE_ERROR_UNSUPPORTED ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 16;
    }

    return 0;
}

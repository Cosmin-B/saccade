#include "backend/registry.hpp"
#include "core/abi_guard.hpp"
#include "core/handle_table.hpp"

#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

namespace saccade::core {
namespace {

struct RuntimeState {
    backend::ProviderRegistry providers;
};

struct RuntimeStore {
    std::mutex lock;
    HandleTable<RuntimeState, 16> runtimes;
};

RuntimeStore& runtime_store() noexcept {
    alignas(RuntimeStore) static std::byte storage[sizeof(RuntimeStore)];
    static RuntimeStore* const store =
        ::new (static_cast<void*>(storage)) RuntimeStore;
    return *store;
}

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

bool reserved_is_zero(
    const void* object,
    uint32_t struct_size,
    size_t reserved_offset,
    size_t current_size) noexcept {
    const size_t available = std::min(static_cast<size_t>(struct_size), current_size);
    if (available <= reserved_offset) {
        return true;
    }
    const auto* bytes = static_cast<const uint8_t*>(object);
    for (size_t index = reserved_offset; index < available; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

template <typename Descriptor>
SaccadeResult copy_and_validate_descriptor(
    const Descriptor* desc,
    Descriptor* out_desc) noexcept {
    if (desc == nullptr || out_desc == nullptr) {
        set_last_error("descriptor is null or smaller than its required prefix");
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t struct_size = 0;
    std::memcpy(&struct_size, static_cast<const void*>(desc), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Descriptor, reserved)) {
        set_last_error("descriptor is null or smaller than its required prefix");
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *out_desc = {};
    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(*out_desc));
    std::memcpy(out_desc, static_cast<const void*>(desc), copy_size);
    if (api_major(out_desc->api_version) != api_major(SACCADE_API_VERSION)) {
        set_last_error("descriptor API major version is incompatible");
        return SACCADE_ERROR_VERSION;
    }
    if (!reserved_is_zero(
            out_desc, struct_size, offsetof(Descriptor, reserved), sizeof(*out_desc))) {
        set_last_error("descriptor reserved fields must be zero");
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

void set_registry_error(SaccadeResult result) noexcept {
    switch (result) {
        case SACCADE_ERROR_ALREADY_EXISTS:
            set_last_error("provider stable ID is already registered");
            break;
        case SACCADE_ERROR_CAPACITY:
            set_last_error("provider registry capacity is exhausted");
            break;
        case SACCADE_ERROR_STATE:
            set_last_error("provider registry is frozen");
            break;
        case SACCADE_ERROR_VERSION:
            set_last_error("provider API major version is incompatible");
            break;
        case SACCADE_ERROR_INVALID_ARGUMENT:
            set_last_error("provider descriptor is malformed");
            break;
        default:
            set_last_error("provider registration failed");
            break;
    }
}

template <typename Descriptor, typename Method>
SaccadeResult register_provider(
    SaccadeRuntimeHandle runtime,
    const Descriptor* desc,
    Method method) noexcept {
    return abi_guard([&]() -> SaccadeResult {
        RuntimeStore& store = runtime_store();
        std::lock_guard<std::mutex> guard(store.lock);
        RuntimeState* state = store.runtimes.get(runtime);
        if (state == nullptr) {
            set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = (state->providers.*method)(desc, nullptr);
        if (result != SACCADE_OK) {
            set_registry_error(result);
        }
        return result;
    });
}

template <typename Descriptor, typename Validate>
SaccadeResult import_frame(
    SaccadeRuntimeHandle runtime,
    const Descriptor* desc,
    SaccadeFrameHandle* out_frame,
    Validate&& validate) noexcept {
    return abi_guard([&]() -> SaccadeResult {
        if (out_frame == nullptr) {
            set_last_error("frame output pointer is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_frame = 0;
        Descriptor normalized_desc{};
        const SaccadeResult descriptor_result =
            copy_and_validate_descriptor(desc, &normalized_desc);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        if (!validate(normalized_desc)) {
            set_last_error("frame descriptor fields are invalid");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        RuntimeStore& store = runtime_store();
        std::lock_guard<std::mutex> guard(store.lock);
        if (store.runtimes.get(runtime) == nullptr) {
            set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        set_last_error("no registered frame importer accepts this descriptor");
        return SACCADE_ERROR_UNSUPPORTED;
    });
}

}  // namespace
}  // namespace saccade::core

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_create(
    const SaccadeRuntimeDesc* desc,
    SaccadeRuntimeHandle* out_runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (out_runtime == nullptr) {
            saccade::core::set_last_error("runtime output pointer is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_runtime = 0;
        SaccadeRuntimeDesc normalized_desc{};
        const SaccadeResult descriptor_result =
            saccade::core::copy_and_validate_descriptor(desc, &normalized_desc);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        if (normalized_desc.flags != 0) {
            saccade::core::set_last_error("runtime flags are unsupported");
            return SACCADE_ERROR_UNSUPPORTED;
        }

        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        std::lock_guard<std::mutex> guard(store.lock);
        const SaccadeResult result = store.runtimes.emplace(out_runtime);
        if (result != SACCADE_OK) {
            saccade::core::set_last_error("runtime capacity is exhausted");
        }
        return result;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_freeze(
    SaccadeRuntimeHandle runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        std::lock_guard<std::mutex> guard(store.lock);
        saccade::core::RuntimeState* state = store.runtimes.get(runtime);
        if (state == nullptr) {
            saccade::core::set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        state->providers.freeze();
        return SACCADE_OK;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_destroy(
    SaccadeRuntimeHandle runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        std::lock_guard<std::mutex> guard(store.lock);
        const SaccadeResult result = store.runtimes.erase(runtime);
        if (result != SACCADE_OK) {
            saccade::core::set_last_error("runtime handle is stale");
        }
        return result;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_inference_provider(
    SaccadeRuntimeHandle runtime,
    const SaccadeInferenceProviderDesc* desc) {
    return saccade::core::register_provider(
        runtime, desc, &saccade::backend::ProviderRegistry::register_inference);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_capture_provider(
    SaccadeRuntimeHandle runtime,
    const SaccadeCaptureProviderDesc* desc) {
    return saccade::core::register_provider(
        runtime, desc, &saccade::backend::ProviderRegistry::register_capture);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_overlay_provider(
    SaccadeRuntimeHandle runtime,
    const SaccadeOverlayProviderDesc* desc) {
    return saccade::core::register_provider(
        runtime, desc, &saccade::backend::ProviderRegistry::register_overlay);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_accessibility_provider(
    SaccadeRuntimeHandle runtime,
    const SaccadeAccessibilityProviderDesc* desc) {
    return saccade::core::register_provider(
        runtime, desc, &saccade::backend::ProviderRegistry::register_accessibility);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_input_provider(
    SaccadeRuntimeHandle runtime,
    const SaccadeInputProviderDesc* desc) {
    return saccade::core::register_provider(
        runtime, desc, &saccade::backend::ProviderRegistry::register_input);
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_host(
    SaccadeRuntimeHandle runtime,
    const SaccadeHostFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame, [](const SaccadeHostFrameDesc& value) noexcept {
            return value.data.data != nullptr && value.data.size != 0 &&
                   value.width != 0 && value.height != 0 &&
                   value.row_stride_bytes != 0 && value.pixel_format != 0;
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(
    SaccadeRuntimeHandle runtime,
    const SaccadeIOSurfaceFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame, [](const SaccadeIOSurfaceFrameDesc& value) noexcept {
            return value.iosurface_id != 0 && value.width != 0 &&
                   value.height != 0 && value.pixel_format != 0;
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_d3d11(
    SaccadeRuntimeHandle runtime,
    const SaccadeD3D11FrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame, [](const SaccadeD3D11FrameDesc& value) noexcept {
            return value.shared_handle != 0 && value.width != 0 &&
                   value.height != 0 && value.pixel_format != 0;
        });
}

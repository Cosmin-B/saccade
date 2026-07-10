#include "backend/registry.hpp"
#include "core/abi_guard.hpp"
#include "core/frame_lease.hpp"
#include "core/frame_validation.hpp"
#include "core/handle_table.hpp"
#include "core/newest_frame_mailbox.hpp"

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

class RuntimeState final {
public:
    static constexpr size_t frame_capacity = 64;

    explicit RuntimeState(uint32_t frame_domain) noexcept : frames_(frame_domain) {}
    ~RuntimeState() noexcept {
        const SaccadeFrameHandle pending = newest_frame_.clear_quiescent();
        if (pending != 0) {
            (void)frames_.release_owner(pending, FrameLeaseOwner::mailbox);
        }
        frames_.clear();
    }

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;
    RuntimeState(RuntimeState&&) = delete;
    RuntimeState& operator=(RuntimeState&&) = delete;

    [[nodiscard]] backend::ProviderRegistry& providers() noexcept {
        return providers_;
    }

    [[nodiscard]] static constexpr uint32_t maximum_frame_domain() noexcept {
        return FrameLeasePool<frame_capacity>::maximum_domain();
    }

    SaccadeResult import_host(
        const SaccadeHostFrameDesc& desc,
        SaccadeFrameHandle* out_frame) noexcept {
        const SaccadeResult import_result = frames_.import_host(desc, out_frame);
        if (import_result != SACCADE_OK) {
            return import_result;
        }

        const SaccadeResult owner_result =
            frames_.add_owner(*out_frame, FrameLeaseOwner::mailbox);
        if (owner_result != SACCADE_OK) {
            (void)frames_.release_owner(*out_frame, FrameLeaseOwner::caller);
            *out_frame = 0;
            return owner_result;
        }

        const SaccadeFrameHandle replaced = newest_frame_.replace(*out_frame);
        if (replaced != 0) {
            (void)frames_.release_owner(replaced, FrameLeaseOwner::mailbox);
        }
        return SACCADE_OK;
    }

    SaccadeResult release_frame(SaccadeFrameHandle frame) noexcept {
        const FrameLease* lease = frames_.get(frame);
        if (lease == nullptr || !lease->has_owner(FrameLeaseOwner::caller)) {
            return SACCADE_ERROR_STALE_HANDLE;
        }

        if (newest_frame_.remove_quiescent(frame)) {
            const SaccadeResult mailbox_result =
                frames_.release_owner(frame, FrameLeaseOwner::mailbox);
            if (mailbox_result != SACCADE_OK) {
                return SACCADE_ERROR_STATE;
            }
        }
        return frames_.release_owner(frame, FrameLeaseOwner::caller);
    }

private:
    backend::ProviderRegistry providers_{};
    FrameLeasePool<frame_capacity> frames_;
    NewestFrameMailbox newest_frame_{};
};

class RuntimeStore final {
public:
    [[nodiscard]] std::mutex& lock() noexcept {
        return lock_;
    }

    [[nodiscard]] HandleTable<RuntimeState, 16>& runtimes() noexcept {
        return runtimes_;
    }

    SaccadeResult create_runtime(SaccadeRuntimeHandle* out_runtime) noexcept {
        if (next_frame_domain_ > RuntimeState::maximum_frame_domain()) {
            return SACCADE_ERROR_CAPACITY;
        }
        const SaccadeResult result =
            runtimes_.emplace(out_runtime, next_frame_domain_);
        if (result == SACCADE_OK) {
            ++next_frame_domain_;
        }
        return result;
    }

private:
    std::mutex lock_{};
    HandleTable<RuntimeState, 16> runtimes_{};
    uint32_t next_frame_domain_ = 1;
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
        std::lock_guard<std::mutex> guard(store.lock());
        RuntimeState* state = store.runtimes().get(runtime);
        if (state == nullptr) {
            set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = (state->providers().*method)(desc, nullptr);
        if (result != SACCADE_OK) {
            set_registry_error(result);
        }
        return result;
    });
}

template <typename Descriptor, typename Validate, typename Import>
SaccadeResult import_frame(
    SaccadeRuntimeHandle runtime,
    const Descriptor* desc,
    SaccadeFrameHandle* out_frame,
    Validate&& validate,
    Import&& import) noexcept {
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
        std::lock_guard<std::mutex> guard(store.lock());
        RuntimeState* state = store.runtimes().get(runtime);
        if (state == nullptr) {
            set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = import(*state, normalized_desc, out_frame);
        if (result == SACCADE_ERROR_CAPACITY) {
            set_last_error("frame lease capacity is exhausted");
        } else if (result == SACCADE_ERROR_UNSUPPORTED) {
            set_last_error("no registered frame importer accepts this descriptor");
        } else if (result != SACCADE_OK) {
            set_last_error("frame import failed");
        }
        return result;
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
        std::lock_guard<std::mutex> guard(store.lock());
        const SaccadeResult result = store.create_runtime(out_runtime);
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
        std::lock_guard<std::mutex> guard(store.lock());
        saccade::core::RuntimeState* state = store.runtimes().get(runtime);
        if (state == nullptr) {
            saccade::core::set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        state->providers().freeze();
        return SACCADE_OK;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_destroy(
    SaccadeRuntimeHandle runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        std::lock_guard<std::mutex> guard(store.lock());
        const SaccadeResult result = store.runtimes().erase(runtime);
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
            return saccade::core::valid_host_frame(value);
        },
        [](saccade::core::RuntimeState& state,
           const SaccadeHostFrameDesc& value,
           SaccadeFrameHandle* out) noexcept {
            return state.import_host(value, out);
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
        },
        [](saccade::core::RuntimeState&,
           const SaccadeIOSurfaceFrameDesc&,
           SaccadeFrameHandle*) noexcept {
            return SACCADE_ERROR_UNSUPPORTED;
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
        },
        [](saccade::core::RuntimeState&,
           const SaccadeD3D11FrameDesc&,
           SaccadeFrameHandle*) noexcept {
            return SACCADE_ERROR_UNSUPPORTED;
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_release(
    SaccadeRuntimeHandle runtime,
    SaccadeFrameHandle frame) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (frame == 0) {
            saccade::core::set_last_error("frame handle is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        std::lock_guard<std::mutex> guard(store.lock());
        saccade::core::RuntimeState* state = store.runtimes().get(runtime);
        if (state == nullptr) {
            saccade::core::set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = state->release_frame(frame);
        if (result == SACCADE_ERROR_STALE_HANDLE) {
            saccade::core::set_last_error("frame handle is stale");
        } else if (result != SACCADE_OK) {
            saccade::core::set_last_error("frame release failed");
        }
        return result;
    });
}

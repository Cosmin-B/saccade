#include "backend/registry.hpp"
#include "core/abi_guard.hpp"
#include "core/frame_lease.hpp"
#include "core/frame_validation.hpp"
#include "core/handle_table.hpp"
#include "core/newest_frame_mailbox.hpp"
#include "core/rare_global_gate.hpp"

#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <unknwn.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

namespace saccade::core {
namespace {

class RuntimeState final {
  public:
    static constexpr size_t frame_capacity = 64;
    static constexpr size_t inference_session_capacity = 8;
    static constexpr size_t inference_ticket_capacity = 16;

    struct InferenceSession {
        void* provider_context = nullptr;
        SaccadeInferenceOps ops{};
        SaccadeModelHandle provider_model = 0;
        SaccadeExecutionContextHandle provider_context_handle = 0;
        uint64_t provider_stable_id = 0;
        SaccadeDeviceInfo device{};
        SaccadeModelInfo model{};
        uint32_t queue_capacity = 0;
        uint32_t max_in_flight = 0;
        uint32_t active_tickets = 0;
    };

    struct InferenceTicket {
        SaccadeExecutionContextHandle session = 0;
        SaccadeTicketHandle provider_ticket = 0;
        SaccadeFrameHandle frame = 0;
    };

    explicit RuntimeState(uint32_t frame_domain) noexcept : frames_(frame_domain) {}

    ~RuntimeState() noexcept {
        inference_tickets_.clear_reverse([this](uint64_t, InferenceTicket& ticket) noexcept {
            InferenceSession* session = inference_sessions_.get(ticket.session);
            if (session != nullptr && ticket.provider_ticket != 0) {
                (void)session->ops.cancel(session->provider_context, session->provider_context_handle,
                                          ticket.provider_ticket);
            }
            (void)frames_.release_owner(ticket.frame, FrameLeaseOwner::worker);
        });
        inference_sessions_.clear_reverse([](uint64_t, InferenceSession& session) noexcept {
            (void)session.ops.reset(session.provider_context, session.provider_context_handle);
            (void)session.ops.destroy_context(session.provider_context, session.provider_context_handle);
            (void)session.ops.destroy_model(session.provider_context, session.provider_model);
        });
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

    [[nodiscard]] backend::ProviderRegistry& providers() noexcept { return providers_; }

    [[nodiscard]] static constexpr uint32_t maximum_frame_domain() noexcept {
        return FrameLeasePool<frame_capacity>::maximum_domain();
    }

    SaccadeResult import_host(const SaccadeHostFrameDesc& desc, SaccadeFrameHandle* out_frame) noexcept {
        const SaccadeResult import_result = frames_.import_host(desc, out_frame);
        if (import_result != SACCADE_OK) {
            return import_result;
        }

        const SaccadeResult owner_result = frames_.add_owner(*out_frame, FrameLeaseOwner::mailbox);
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

    SaccadeResult import_native(const NativeFrameResource& desc, SaccadeFrameHandle* out_frame) noexcept {
        const SaccadeResult import_result = frames_.import_native(desc, out_frame);
        if (import_result != SACCADE_OK) {
            return import_result;
        }

        const SaccadeResult owner_result = frames_.add_owner(*out_frame, FrameLeaseOwner::mailbox);
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
        if (lease->storage() == FrameStorage::host && lease->has_owner(FrameLeaseOwner::worker)) {
            return SACCADE_ERROR_BUSY;
        }

        if (newest_frame_.remove_quiescent(frame)) {
            const SaccadeResult mailbox_result = frames_.release_owner(frame, FrameLeaseOwner::mailbox);
            if (mailbox_result != SACCADE_OK) {
                return SACCADE_ERROR_STATE;
            }
        }
        return frames_.release_owner(frame, FrameLeaseOwner::caller);
    }

    SaccadeResult create_inference_session(const SaccadeInferenceSessionDesc& desc,
                                           SaccadeExecutionContextHandle* out_session,
                                           SaccadeInferenceSessionInfo* out_info) noexcept {
        if (out_session == nullptr || out_info == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        *out_session = 0;
        if (!providers_.frozen()) {
            return SACCADE_ERROR_STATE;
        }

        if (desc.model_bytes.data == nullptr || desc.model_bytes.size == 0 || desc.queue_capacity == 0 ||
            desc.max_in_flight == 0 || desc.max_in_flight > desc.queue_capacity || desc.reserved32 != 0 ||
            desc.flags != 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        backend::ProviderSelection selected{};
        const SaccadeResult selection =
            desc.provider_stable_id != 0
                ? providers_.select_inference_by_id(desc.provider_stable_id, &selected)
                : providers_.select_inference(desc.required_capability_bits, desc.preferred_capability_bits, &selected);
        if (selection != SACCADE_OK) {
            return selection;
        }

        const backend::ProviderRegistry::InferenceRecord* provider = providers_.inference(selected.handle);
        if (provider == nullptr) {
            return SACCADE_ERROR_STATE;
        }

        // Select a compatible device before querying the model metadata.
        SaccadeDeviceInfo device{};
        bool found_device = false;
        for (uint32_t index = 0; index < 64; ++index) {
            SaccadeDeviceInfo candidate{};
            candidate.struct_size = sizeof(candidate);
            candidate.api_version = SACCADE_API_VERSION;
            const SaccadeResult enumerated = provider->ops.enumerate_devices(provider->context, index, &candidate);
            if (enumerated == SACCADE_ERROR_NOT_FOUND) {
                break;
            }
            if (enumerated != SACCADE_OK) {
                return enumerated;
            }
            if ((desc.device_stable_id != 0 && candidate.stable_id != desc.device_stable_id) ||
                (candidate.capability_bits & desc.required_capability_bits) != desc.required_capability_bits ||
                (candidate.format_bits & desc.required_format_bits) != desc.required_format_bits ||
                (candidate.precision_bits & desc.required_precision_bits) != desc.required_precision_bits ||
                (candidate.import_bits & desc.required_import_bits) != desc.required_import_bits ||
                candidate.queue_capacity < desc.queue_capacity || candidate.max_in_flight < desc.max_in_flight) {
                continue;
            }
            device = candidate;
            found_device = true;
            break;
        }
        if (!found_device) {
            return SACCADE_ERROR_NOT_FOUND;
        }

        SaccadeModelInfo model_info{};
        model_info.struct_size = sizeof(model_info);
        model_info.api_version = SACCADE_API_VERSION;
        SaccadeResult result = provider->ops.query_model(provider->context, desc.model_bytes, &model_info);
        if (result != SACCADE_OK) {
            return result;
        }
        if (model_info.max_output_bytes == 0 ||
            (model_info.capability_bits & desc.required_capability_bits) != desc.required_capability_bits) {
            return SACCADE_ERROR_UNSUPPORTED;
        }

        SaccadeModelDesc model_desc{};
        model_desc.struct_size = sizeof(model_desc);
        model_desc.api_version = SACCADE_API_VERSION;
        model_desc.bytes = desc.model_bytes;
        model_desc.stable_id = desc.model_stable_id != 0 ? desc.model_stable_id : model_info.stable_id;
        model_desc.device_id = device.stable_id;
        SaccadeModelHandle model = 0;
        result = provider->ops.create_model(provider->context, &model_desc, &model);
        if (result != SACCADE_OK) {
            return result;
        }

        SaccadeExecutionContextDesc context_desc{};
        context_desc.struct_size = sizeof(context_desc);
        context_desc.api_version = SACCADE_API_VERSION;
        context_desc.model = model;
        context_desc.device_id = device.stable_id;
        context_desc.queue_capacity = desc.queue_capacity;
        context_desc.max_in_flight = desc.max_in_flight;
        SaccadeExecutionContextHandle provider_context_handle = 0;
        result = provider->ops.create_context(provider->context, &context_desc, &provider_context_handle);
        if (result != SACCADE_OK) {
            (void)provider->ops.destroy_model(provider->context, model);
            return result;
        }

        InferenceSession session{};
        session.provider_context = provider->context;
        session.ops = provider->ops;
        session.provider_model = model;
        session.provider_context_handle = provider_context_handle;
        session.provider_stable_id = provider->info.stable_id;
        session.device = device;
        session.model = model_info;
        session.queue_capacity = desc.queue_capacity;
        session.max_in_flight = desc.max_in_flight;
        result = inference_sessions_.emplace(out_session, session);
        if (result != SACCADE_OK) {
            (void)provider->ops.destroy_context(provider->context, provider_context_handle);
            (void)provider->ops.destroy_model(provider->context, model);
            return result;
        }

        SaccadeInferenceSessionInfo info{};
        info.struct_size = sizeof(info);
        info.api_version = SACCADE_API_VERSION;
        info.session = *out_session;
        info.provider_stable_id = provider->info.stable_id;
        info.device_stable_id = device.stable_id;
        info.model_stable_id = model_desc.stable_id;
        info.capability_bits = device.capability_bits;
        info.format_bits = device.format_bits;
        info.precision_bits = device.precision_bits;
        info.import_bits = device.import_bits;
        info.max_output_bytes = model_info.max_output_bytes;
        info.queue_capacity = desc.queue_capacity;
        info.max_in_flight = desc.max_in_flight;
        *out_info = info;
        return SACCADE_OK;
    }

    SaccadeResult destroy_inference_session(SaccadeExecutionContextHandle handle) noexcept {
        InferenceSession* session = inference_sessions_.get(handle);
        if (session == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        if (session->active_tickets != 0) {
            return SACCADE_ERROR_BUSY;
        }
        SaccadeResult result =
            session->ops.destroy_context(session->provider_context, session->provider_context_handle);
        if (result != SACCADE_OK) {
            return result;
        }
        result = session->ops.destroy_model(session->provider_context, session->provider_model);
        if (result != SACCADE_OK) {
            return result;
        }
        return inference_sessions_.erase(handle);
    }

    SaccadeResult submit_inference(SaccadeExecutionContextHandle session_handle, const SaccadeInferenceSubmitDesc& desc,
                                   SaccadeTicketHandle* out_ticket) noexcept {
        if (out_ticket == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_ticket = 0;
        InferenceSession* session = inference_sessions_.get(session_handle);
        FrameLease* frame = frames_.get(desc.frame);
        if (session == nullptr || frame == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        if (desc.scope.x < 0 || desc.scope.y < 0 || desc.scope.width <= 0 || desc.scope.height <= 0 ||
            desc.model_epoch == 0 || desc.session_epoch == 0 || desc.transform_epoch == 0 || desc.topology_epoch == 0 ||
            desc.source_id == 0 || desc.flags != 0 || desc.output_capacity < session->model.max_output_bytes ||
            (desc.priority_region_count != 0 && desc.priority_regions == nullptr) ||
            frame->transform_epoch() != desc.transform_epoch) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        const int64_t scope_right = static_cast<int64_t>(desc.scope.x) + desc.scope.width;
        const int64_t scope_bottom = static_cast<int64_t>(desc.scope.y) + desc.scope.height;
        if (scope_right > frame->width() || scope_bottom > frame->height()) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (session->active_tickets >= session->queue_capacity) {
            return SACCADE_ERROR_BUSY;
        }
        const SaccadeResult owner = frames_.add_owner(desc.frame, FrameLeaseOwner::worker);
        if (owner != SACCADE_OK) {
            return owner == SACCADE_ERROR_ALREADY_EXISTS ? SACCADE_ERROR_BUSY : owner;
        }

        InferenceTicket binding{};
        binding.session = session_handle;
        binding.frame = desc.frame;
        SaccadeTicketHandle runtime_ticket = 0;
        SaccadeResult result = inference_tickets_.emplace(&runtime_ticket, binding);
        if (result != SACCADE_OK) {
            (void)frames_.release_owner(desc.frame, FrameLeaseOwner::worker);
            return result;
        }
        ++session->active_tickets;

        SaccadeInferenceDispatchDesc dispatch{};
        dispatch.struct_size = sizeof(dispatch);
        dispatch.api_version = SACCADE_API_VERSION;
        dispatch.frame = frame->resource_view();
        dispatch.scope = desc.scope;
        dispatch.priority_regions = desc.priority_regions;
        dispatch.priority_region_count = desc.priority_region_count;
        dispatch.output_capacity = desc.output_capacity;
        dispatch.model_epoch = desc.model_epoch;
        dispatch.session_epoch = desc.session_epoch;
        dispatch.transform_epoch = desc.transform_epoch;
        dispatch.topology_epoch = desc.topology_epoch;
        dispatch.source_id = desc.source_id;
        dispatch.flags = desc.flags;
        SaccadeTicketHandle provider_ticket = 0;
        result = session->ops.submit(session->provider_context, session->provider_context_handle, &dispatch,
                                     &provider_ticket);
        if (result != SACCADE_OK) {
            --session->active_tickets;
            (void)inference_tickets_.erase(runtime_ticket);
            (void)frames_.release_owner(desc.frame, FrameLeaseOwner::worker);
            return result;
        }
        inference_tickets_.get(runtime_ticket)->provider_ticket = provider_ticket;
        *out_ticket = runtime_ticket;
        return SACCADE_OK;
    }

    SaccadeResult inference_status(SaccadeExecutionContextHandle session_handle, SaccadeTicketHandle ticket_handle,
                                   uint64_t timeout_ns, bool wait, SaccadeInferenceStatus* output) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        InferenceTicket* ticket = inference_tickets_.get(ticket_handle);
        if (session == nullptr || ticket == nullptr || ticket->session != session_handle) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        SaccadeInferenceStatus status{};
        status.struct_size = sizeof(status);
        status.api_version = SACCADE_API_VERSION;
        const SaccadeResult result =
            wait ? session->ops.wait(session->provider_context, session->provider_context_handle,
                                     ticket->provider_ticket, timeout_ns, &status)
                 : session->ops.poll(session->provider_context, session->provider_context_handle,
                                     ticket->provider_ticket, &status);
        if (result == SACCADE_OK || result == SACCADE_ERROR_TIMEOUT || result == SACCADE_ERROR_BUSY) {
            status.ticket = ticket_handle;
            *output = status;
        }
        return result;
    }

    SaccadeResult collect_inference(SaccadeExecutionContextHandle session_handle, SaccadeTicketHandle ticket_handle,
                                    SaccadeMutableSpanU8 output, size_t* required) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        InferenceTicket* ticket = inference_tickets_.get(ticket_handle);
        if (session == nullptr || ticket == nullptr || ticket->session != session_handle) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = session->ops.collect(session->provider_context, session->provider_context_handle,
                                                          ticket->provider_ticket, output, required);
        if (result == SACCADE_ERROR_BUSY || result == SACCADE_ERROR_CAPACITY || result == SACCADE_ERROR_TIMEOUT) {
            return result;
        }
        const SaccadeFrameHandle frame = ticket->frame;
        --session->active_tickets;
        (void)inference_tickets_.erase(ticket_handle);
        const SaccadeResult released = frames_.release_owner(frame, FrameLeaseOwner::worker);
        return released == SACCADE_OK ? result : released;
    }

    SaccadeResult cancel_inference(SaccadeExecutionContextHandle session_handle,
                                   SaccadeTicketHandle ticket_handle) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        InferenceTicket* ticket = inference_tickets_.get(ticket_handle);
        if (session == nullptr || ticket == nullptr || ticket->session != session_handle) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        return session->ops.cancel(session->provider_context, session->provider_context_handle,
                                   ticket->provider_ticket);
    }

    SaccadeResult reset_inference(SaccadeExecutionContextHandle session_handle) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        if (session == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        const SaccadeResult result = session->ops.reset(session->provider_context, session->provider_context_handle);
        if (result != SACCADE_OK) {
            return result;
        }
        std::array<SaccadeTicketHandle, inference_ticket_capacity> handles{};
        size_t count = 0;
        inference_tickets_.for_each([&](SaccadeTicketHandle handle, const InferenceTicket& ticket) noexcept {
            if (ticket.session == session_handle) {
                handles[count++] = handle;
            }
        });
        for (size_t index = 0; index < count; ++index) {
            InferenceTicket* ticket = inference_tickets_.get(handles[index]);
            const SaccadeFrameHandle frame = ticket->frame;
            (void)inference_tickets_.erase(handles[index]);
            (void)frames_.release_owner(frame, FrameLeaseOwner::worker);
        }
        session->active_tickets = 0;
        return SACCADE_OK;
    }

    SaccadeResult synchronize_inference(SaccadeExecutionContextHandle session_handle, uint64_t timeout_ns) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        return session == nullptr
                   ? SACCADE_ERROR_STALE_HANDLE
                   : session->ops.synchronize(session->provider_context, session->provider_context_handle, timeout_ns);
    }

    SaccadeResult inference_memory_stats(SaccadeExecutionContextHandle session_handle,
                                         SaccadeMemoryStats* output) noexcept {
        InferenceSession* session = inference_sessions_.get(session_handle);
        return session == nullptr
                   ? SACCADE_ERROR_STALE_HANDLE
                   : session->ops.memory_stats(session->provider_context, session->provider_context_handle, output);
    }

  private:
    backend::ProviderRegistry providers_{};
    FrameLeasePool<frame_capacity> frames_;
    NewestFrameMailbox newest_frame_{};
    HandleTable<InferenceSession, inference_session_capacity> inference_sessions_{};
    HandleTable<InferenceTicket, inference_ticket_capacity> inference_tickets_{};
};

class RuntimeStore final {
  public:
    static constexpr size_t capacity = 16;

    [[nodiscard]] RareGlobalGate& gate() noexcept { return gate_; }

    [[nodiscard]] HandleTable<RuntimeState, capacity>& runtimes() noexcept { return runtimes_; }

    SaccadeResult create_runtime(SaccadeRuntimeHandle* out_runtime) noexcept {
        if (next_frame_domain_ > RuntimeState::maximum_frame_domain()) {
            return SACCADE_ERROR_CAPACITY;
        }
        const SaccadeResult result = runtimes_.emplace(out_runtime, next_frame_domain_);
        if (result == SACCADE_OK) {
            const size_t index = runtime_index(*out_runtime);
            hot_[index].state.store(runtimes_.get(*out_runtime), std::memory_order_relaxed);
            hot_[index].handle.store(*out_runtime, std::memory_order_release);
            ++next_frame_domain_;
        }
        return result;
    }

    RuntimeState* resolve_hot(SaccadeRuntimeHandle runtime) noexcept {
        const size_t index = runtime_index(runtime);
        if (index >= capacity || hot_[index].handle.load(std::memory_order_acquire) != runtime) {
            return nullptr;
        }
        return hot_[index].state.load(std::memory_order_relaxed);
    }

    SaccadeResult destroy_runtime(SaccadeRuntimeHandle runtime) noexcept {
        const size_t index = runtime_index(runtime);
        if (index >= capacity || runtimes_.get(runtime) == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        hot_[index].handle.store(0, std::memory_order_release);
        hot_[index].state.store(nullptr, std::memory_order_relaxed);
        return runtimes_.erase(runtime);
    }

  private:
    struct HotSlot {
        std::atomic<uint64_t> handle{0};
        std::atomic<RuntimeState*> state{nullptr};
    };

    static size_t runtime_index(SaccadeRuntimeHandle runtime) noexcept {
        const uint32_t slot = static_cast<uint32_t>(runtime);
        return slot == 0 ? capacity : static_cast<size_t>(slot - 1U);
    }

    RareGlobalGate gate_{};
    HandleTable<RuntimeState, capacity> runtimes_{};
    std::array<HotSlot, capacity> hot_{};
    uint32_t next_frame_domain_ = 1;
};

RuntimeStore& runtime_store() noexcept {
    alignas(RuntimeStore) static std::byte storage[sizeof(RuntimeStore)];
    static RuntimeStore* const store = ::new (static_cast<void*>(storage)) RuntimeStore;
    return *store;
}

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

bool reserved_is_zero(const void* object, uint32_t struct_size, size_t reserved_offset, size_t current_size) noexcept {
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
SaccadeResult copy_and_validate_descriptor(const Descriptor* desc, Descriptor* out_desc) noexcept {
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
    if (!reserved_is_zero(out_desc, struct_size, offsetof(Descriptor, reserved), sizeof(*out_desc))) {
        set_last_error("descriptor reserved fields must be zero");
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Structure> bool valid_output_structure(const Structure* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    uint32_t size = 0;
    uint32_t version = 0;
    std::memcpy(&size, output, sizeof(size));
    std::memcpy(&version, reinterpret_cast<const uint8_t*>(output) + offsetof(Structure, api_version), sizeof(version));
    return static_cast<size_t>(size) >= offsetof(Structure, reserved) &&
           api_major(version) == api_major(SACCADE_API_VERSION);
}

template <typename Structure> SaccadeResult write_output_structure(Structure* output, Structure value) noexcept {
    if (!valid_output_structure(output)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t size = 0;
    std::memcpy(&size, output, sizeof(size));
    const size_t copy_size = std::min(static_cast<size_t>(size), sizeof(Structure));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(output, &value, copy_size);
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
SaccadeResult register_provider(SaccadeRuntimeHandle runtime, const Descriptor* desc, Method method) noexcept {
    return abi_guard([&]() -> SaccadeResult {
        RuntimeStore& store = runtime_store();
        RareGlobalGuard guard(store.gate());
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
SaccadeResult import_frame(SaccadeRuntimeHandle runtime, const Descriptor* desc, SaccadeFrameHandle* out_frame,
                           Validate&& validate, Import&& import) noexcept {
    return abi_guard([&]() -> SaccadeResult {
        if (out_frame == nullptr) {
            set_last_error("frame output pointer is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_frame = 0;
        Descriptor normalized_desc{};
        const SaccadeResult descriptor_result = copy_and_validate_descriptor(desc, &normalized_desc);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        if (!validate(normalized_desc)) {
            set_last_error("frame descriptor fields are invalid");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        RuntimeStore& store = runtime_store();
        RuntimeState* state = store.resolve_hot(runtime);
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

} // namespace
} // namespace saccade::core

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_create(const SaccadeRuntimeDesc* desc,
                                                             SaccadeRuntimeHandle* out_runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (out_runtime == nullptr) {
            saccade::core::set_last_error("runtime output pointer is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_runtime = 0;
        SaccadeRuntimeDesc normalized_desc{};
        const SaccadeResult descriptor_result = saccade::core::copy_and_validate_descriptor(desc, &normalized_desc);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        if (normalized_desc.flags != 0) {
            saccade::core::set_last_error("runtime flags are unsupported");
            return SACCADE_ERROR_UNSUPPORTED;
        }

        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        saccade::core::RareGlobalGuard guard(store.gate());
        const SaccadeResult result = store.create_runtime(out_runtime);
        if (result != SACCADE_OK) {
            saccade::core::set_last_error("runtime capacity is exhausted");
        }
        return result;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_freeze(SaccadeRuntimeHandle runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        saccade::core::RareGlobalGuard guard(store.gate());
        saccade::core::RuntimeState* state = store.runtimes().get(runtime);
        if (state == nullptr) {
            saccade::core::set_last_error("runtime handle is stale");
            return SACCADE_ERROR_STALE_HANDLE;
        }
        state->providers().freeze();
        return SACCADE_OK;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_runtime_destroy(SaccadeRuntimeHandle runtime) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        saccade::core::RareGlobalGuard guard(store.gate());
        const SaccadeResult result = store.destroy_runtime(runtime);
        if (result != SACCADE_OK) {
            saccade::core::set_last_error("runtime handle is stale");
        }
        return result;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_inference_provider(SaccadeRuntimeHandle runtime,
                                                                          const SaccadeInferenceProviderDesc* desc) {
    return saccade::core::register_provider(runtime, desc, &saccade::backend::ProviderRegistry::register_inference);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_capture_provider(SaccadeRuntimeHandle runtime,
                                                                        const SaccadeCaptureProviderDesc* desc) {
    return saccade::core::register_provider(runtime, desc, &saccade::backend::ProviderRegistry::register_capture);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_overlay_provider(SaccadeRuntimeHandle runtime,
                                                                        const SaccadeOverlayProviderDesc* desc) {
    return saccade::core::register_provider(runtime, desc, &saccade::backend::ProviderRegistry::register_overlay);
}

extern "C" SaccadeResult SACCADE_CALL
saccade_register_accessibility_provider(SaccadeRuntimeHandle runtime, const SaccadeAccessibilityProviderDesc* desc) {
    return saccade::core::register_provider(runtime, desc, &saccade::backend::ProviderRegistry::register_accessibility);
}

extern "C" SaccadeResult SACCADE_CALL saccade_register_input_provider(SaccadeRuntimeHandle runtime,
                                                                      const SaccadeInputProviderDesc* desc) {
    return saccade::core::register_provider(runtime, desc, &saccade::backend::ProviderRegistry::register_input);
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_host(SaccadeRuntimeHandle runtime,
                                                                const SaccadeHostFrameDesc* desc,
                                                                SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame,
        [](const SaccadeHostFrameDesc& value) noexcept { return saccade::core::valid_host_frame(value); },
        [](saccade::core::RuntimeState& state, const SaccadeHostFrameDesc& value, SaccadeFrameHandle* out) noexcept {
            return state.import_host(value, out);
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(SaccadeRuntimeHandle runtime,
                                                                     const SaccadeIOSurfaceFrameDesc* desc,
                                                                     SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame,
        [](const SaccadeIOSurfaceFrameDesc& value) noexcept {
            return value.iosurface_id != 0 && value.width != 0 && value.height != 0 && value.pixel_format != 0;
        },
        [](saccade::core::RuntimeState& state, const SaccadeIOSurfaceFrameDesc& value,
           SaccadeFrameHandle* out) noexcept -> SaccadeResult {
#if defined(__APPLE__)
            IOSurfaceRef surface = IOSurfaceLookup(static_cast<IOSurfaceID>(value.iosurface_id));
            if (surface == nullptr) {
                return SACCADE_ERROR_NOT_FOUND;
            }
            const size_t plane_count = IOSurfaceGetPlaneCount(surface);
            const bool planar = plane_count != 0;
            const bool valid_plane =
                planar ? static_cast<size_t>(value.plane_index) < plane_count : value.plane_index == 0;
            const size_t width =
                planar ? IOSurfaceGetWidthOfPlane(surface, value.plane_index) : IOSurfaceGetWidth(surface);
            const size_t height =
                planar ? IOSurfaceGetHeightOfPlane(surface, value.plane_index) : IOSurfaceGetHeight(surface);
            if (!valid_plane || width != value.width || height != value.height) {
                CFRelease(surface);
                return SACCADE_ERROR_INVALID_ARGUMENT;
            }
            saccade::core::NativeFrameResource resource{};
            resource.resource = surface;
            resource.release = +[](void* object) noexcept { CFRelease(static_cast<CFTypeRef>(object)); };
            resource.native_id = value.iosurface_id;
            resource.plane_index = value.plane_index;
            resource.pixel_format = value.pixel_format;
            resource.width = value.width;
            resource.height = value.height;
            resource.frame_id = value.frame_id;
            resource.transform_epoch = value.transform_epoch;
            resource.storage = saccade::core::FrameStorage::iosurface;
            const SaccadeResult result = state.import_native(resource, out);
            if (result != SACCADE_OK) {
                CFRelease(surface);
            }
            return result;
#else
            (void)state;
            (void)value;
            (void)out;
            return SACCADE_ERROR_UNSUPPORTED;
#endif
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_import_win32_capture(SaccadeRuntimeHandle runtime,
                                                                         const SaccadeWin32CaptureFrameDesc* desc,
                                                                         SaccadeFrameHandle* out_frame) {
    return saccade::core::import_frame(
        runtime, desc, out_frame,
        [](const SaccadeWin32CaptureFrameDesc& value) noexcept {
            return value.texture != nullptr && value.width != 0 && value.height != 0 && value.pixel_format != 0 &&
                   (value.ready_fence == nullptr) == (value.ready_value == 0);
        },
        [](saccade::core::RuntimeState& state, const SaccadeWin32CaptureFrameDesc& value,
           SaccadeFrameHandle* out) noexcept -> SaccadeResult {
#if defined(_WIN32)
            IUnknown* texture = static_cast<IUnknown*>(value.texture);
            texture->AddRef();
            IUnknown* ready_fence = static_cast<IUnknown*>(value.ready_fence);
            if (ready_fence != nullptr) ready_fence->AddRef();
            saccade::core::NativeFrameResource resource{};
            resource.resource = texture;
            resource.release = +[](void* object) noexcept { static_cast<IUnknown*>(object)->Release(); };
            resource.ready_fence = ready_fence;
            resource.release_ready_fence = resource.release;
            resource.ready_value = value.ready_value;
            resource.native_id = reinterpret_cast<uintptr_t>(texture);
            resource.plane_index = value.subresource;
            resource.pixel_format = value.pixel_format;
            resource.width = value.width;
            resource.height = value.height;
            resource.frame_id = value.frame_id;
            resource.transform_epoch = value.transform_epoch;
            resource.storage = saccade::core::FrameStorage::win32_capture;
            const SaccadeResult result = state.import_native(resource, out);
            if (result != SACCADE_OK) {
                if (ready_fence != nullptr) ready_fence->Release();
                texture->Release();
            }
            return result;
#else
            (void)state;
            (void)value;
            (void)out;
            return SACCADE_ERROR_UNSUPPORTED;
#endif
        });
}

extern "C" SaccadeResult SACCADE_CALL saccade_frame_release(SaccadeRuntimeHandle runtime, SaccadeFrameHandle frame) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (frame == 0) {
            saccade::core::set_last_error("frame handle is null");
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }

        saccade::core::RuntimeStore& store = saccade::core::runtime_store();
        saccade::core::RuntimeState* state = store.resolve_hot(runtime);
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

extern "C" SaccadeResult SACCADE_CALL saccade_inference_session_create(SaccadeRuntimeHandle runtime,
                                                                       const SaccadeInferenceSessionDesc* desc,
                                                                       SaccadeExecutionContextHandle* out_session,
                                                                       SaccadeInferenceSessionInfo* out_info) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (out_session == nullptr || !saccade::core::valid_output_structure(out_info)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_session = 0;
        SaccadeInferenceSessionDesc normalized{};
        const SaccadeResult descriptor_result = saccade::core::copy_and_validate_descriptor(desc, &normalized);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        if (state == nullptr) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        SaccadeInferenceSessionInfo info{};
        info.struct_size = sizeof(info);
        info.api_version = SACCADE_API_VERSION;
        const SaccadeResult result = state->create_inference_session(normalized, out_session, &info);
        if (result != SACCADE_OK) {
            return result;
        }
        const SaccadeResult written = saccade::core::write_output_structure(out_info, info);
        if (written != SACCADE_OK) {
            (void)state->destroy_inference_session(*out_session);
            *out_session = 0;
        }
        return written;
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_session_destroy(SaccadeRuntimeHandle runtime,
                                                                        SaccadeExecutionContextHandle session) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->destroy_inference_session(session);
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_submit(SaccadeRuntimeHandle runtime,
                                                               SaccadeExecutionContextHandle session,
                                                               const SaccadeInferenceSubmitDesc* desc,
                                                               SaccadeTicketHandle* out_ticket) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (out_ticket == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_ticket = 0;
        SaccadeInferenceSubmitDesc normalized{};
        const SaccadeResult descriptor_result = saccade::core::copy_and_validate_descriptor(desc, &normalized);
        if (descriptor_result != SACCADE_OK) {
            return descriptor_result;
        }
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->submit_inference(session, normalized, out_ticket);
    });
}

namespace {

SaccadeResult inference_status_call(SaccadeRuntimeHandle runtime, SaccadeExecutionContextHandle session,
                                    SaccadeTicketHandle ticket, uint64_t timeout_ns, bool wait,
                                    SaccadeInferenceStatus* output) noexcept {
    if (!saccade::core::valid_output_structure(output)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
    if (state == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeInferenceStatus status{};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    const SaccadeResult result = state->inference_status(session, ticket, timeout_ns, wait, &status);
    if (result != SACCADE_OK && result != SACCADE_ERROR_TIMEOUT && result != SACCADE_ERROR_BUSY) {
        return result;
    }
    const SaccadeResult written = saccade::core::write_output_structure(output, status);
    return written == SACCADE_OK ? result : written;
}

} // namespace

extern "C" SaccadeResult SACCADE_CALL saccade_inference_poll(SaccadeRuntimeHandle runtime,
                                                             SaccadeExecutionContextHandle session,
                                                             SaccadeTicketHandle ticket,
                                                             SaccadeInferenceStatus* output) {
    return saccade::core::abi_guard(
        [&]() -> SaccadeResult { return inference_status_call(runtime, session, ticket, 0, false, output); });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_wait(SaccadeRuntimeHandle runtime,
                                                             SaccadeExecutionContextHandle session,
                                                             SaccadeTicketHandle ticket, uint64_t timeout_ns,
                                                             SaccadeInferenceStatus* output) {
    return saccade::core::abi_guard(
        [&]() -> SaccadeResult { return inference_status_call(runtime, session, ticket, timeout_ns, true, output); });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_collect(SaccadeRuntimeHandle runtime,
                                                                SaccadeExecutionContextHandle session,
                                                                SaccadeTicketHandle ticket, SaccadeMutableSpanU8 output,
                                                                size_t* required) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        if (required == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE
                                : state->collect_inference(session, ticket, output, required);
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_cancel(SaccadeRuntimeHandle runtime,
                                                               SaccadeExecutionContextHandle session,
                                                               SaccadeTicketHandle ticket) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->cancel_inference(session, ticket);
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_reset(SaccadeRuntimeHandle runtime,
                                                              SaccadeExecutionContextHandle session) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->reset_inference(session);
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_synchronize(SaccadeRuntimeHandle runtime,
                                                                    SaccadeExecutionContextHandle session,
                                                                    uint64_t timeout_ns) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->synchronize_inference(session, timeout_ns);
    });
}

extern "C" SaccadeResult SACCADE_CALL saccade_inference_memory_stats(SaccadeRuntimeHandle runtime,
                                                                     SaccadeExecutionContextHandle session,
                                                                     SaccadeMemoryStats* output) {
    return saccade::core::abi_guard([&]() -> SaccadeResult {
        saccade::core::RuntimeState* state = saccade::core::runtime_store().resolve_hot(runtime);
        return state == nullptr ? SACCADE_ERROR_STALE_HANDLE : state->inference_memory_stats(session, output);
    });
}

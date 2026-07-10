#include "backends/mock/mock_backend.hpp"

#include "backends/callback_guard.hpp"
#include "core/handle_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace saccade::backend::mock {
namespace {

constexpr uint64_t inference_provider_id = UINT64_C(0x4D4F434B0001);
constexpr uint64_t capture_provider_id = UINT64_C(0x4D4F434B0002);
constexpr uint64_t overlay_provider_id = UINT64_C(0x4D4F434B0003);
constexpr uint64_t accessibility_provider_id = UINT64_C(0x4D4F434B0004);
constexpr uint64_t input_provider_id = UINT64_C(0x4D4F434B0005);
constexpr uint64_t device_id = UINT64_C(0x4D4F434B1001);
constexpr uint64_t capture_source_id = UINT64_C(0x4D4F434B2001);
constexpr uint64_t window_id = UINT64_C(0x4D4F434B3001);
constexpr size_t inference_output_size = 32;
constexpr size_t accessibility_output_size = 24;

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

bool reserved_is_zero(const void* object, uint32_t struct_size, size_t reserved_offset,
                      size_t current_size) noexcept {
    const size_t available = std::min(static_cast<size_t>(struct_size), current_size);
    const auto* bytes = static_cast<const uint8_t*>(object);
    for (size_t index = reserved_offset; index < available; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

template <typename Structure>
SaccadeResult read_structure(const Structure* source, Structure* out_value) noexcept {
    if (source == nullptr || out_value == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t struct_size = 0;
    std::memcpy(&struct_size, static_cast<const void*>(source), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *out_value = {};
    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(*out_value));
    std::memcpy(out_value, static_cast<const void*>(source), copy_size);
    if (api_major(out_value->api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    if (!reserved_is_zero(out_value, struct_size, offsetof(Structure, reserved),
                          sizeof(*out_value))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Structure>
SaccadeResult write_structure(Structure* destination, Structure value) noexcept {
    if (destination == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t struct_size = 0;
    uint32_t api_version = 0;
    std::memcpy(&struct_size, static_cast<const void*>(destination), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::memcpy(&api_version,
                static_cast<const uint8_t*>(static_cast<const void*>(destination)) +
                    offsetof(Structure, api_version),
                sizeof(api_version));
    if (api_major(api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }

    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(value));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(static_cast<void*>(destination), &value, copy_size);
    return SACCADE_OK;
}

void write_u64_le(uint8_t* destination, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index) {
        destination[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

uint64_t hash_bytes(SaccadeSpanU8 bytes) noexcept {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < bytes.size; ++index) {
        hash ^= bytes.data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

SaccadeSpanU8 literal_span(const char* text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text), std::strlen(text)};
}

SaccadeMemoryStats memory_stats(const MemoryConfig& config) noexcept {
    SaccadeMemoryStats result{};
    result.struct_size = static_cast<uint32_t>(sizeof(result));
    result.api_version = SACCADE_API_VERSION;
    result.host_committed = config.host_committed;
    result.host_reserved = config.host_reserved;
    result.device_imported = config.device_imported;
    result.device_owned = config.device_owned;
    result.framework_opaque = config.framework_opaque;
    result.copied_bytes = config.copied_bytes;
    result.high_water_bytes = config.high_water_bytes;
    return result;
}

SaccadeProviderInfo provider_info(uint32_t family, uint64_t stable_id, uint32_t capabilities,
                                  const char* name) noexcept {
    SaccadeProviderInfo result{};
    result.struct_size = static_cast<uint32_t>(sizeof(result));
    result.api_version = SACCADE_API_VERSION;
    result.family = family;
    result.capability_bits = capabilities;
    result.stable_id = stable_id;
    result.name = literal_span(name);
    return result;
}

} // namespace

struct Backend::Impl {
    struct Model {
        uint64_t stable_id = 0;
        uint64_t device = 0;
        uint32_t context_count = 0;
    };

    struct ExecutionContext {
        SaccadeModelHandle model = 0;
        uint32_t queue_capacity = 0;
        uint32_t max_in_flight = 0;
        uint32_t active_tickets = 0;
        uint32_t running_tickets = 0;
    };

    struct InferenceTicket {
        SaccadeExecutionContextHandle context = 0;
        SaccadeFrameHandle frame = 0;
        uint64_t model_epoch = 0;
        uint64_t session_epoch = 0;
        uint64_t transform_epoch = 0;
        uint32_t remaining_polls = 0;
        uint32_t state = SACCADE_TICKET_QUEUED;
        SaccadeResult result = SACCADE_OK;
        bool counted = true;
        bool running_counted = false;
        std::array<uint8_t, inference_output_size> output{};
    };

    struct CaptureStream {
        uint64_t source = 0;
        uint32_t queue_capacity = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixel_format = 0;
        uint32_t outstanding_frames = 0;
        bool started = false;
    };

    struct CapturedFrame {
        SaccadeCaptureStreamHandle stream = 0;
        uint64_t frame_id = 0;
        uint64_t transform_epoch = 0;
        uint64_t timestamp_ns = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixel_format = 0;
    };

    struct Overlay {
        SaccadeRectI32 bounds{};
        uint32_t visible = 0;
        uint64_t submissions = 0;
    };

    struct AccessibilityTicket {
        uint64_t window = 0;
        uint64_t session_epoch = 0;
        uint64_t transform_epoch = 0;
        uint32_t remaining_polls = 0;
        uint32_t state = SACCADE_TICKET_QUEUED;
        SaccadeResult result = SACCADE_OK;
        SaccadeSnapshotHandle snapshot = 0;
        bool counted = true;
    };

    struct Snapshot {
        SaccadeTicketHandle ticket = 0;
        uint64_t window = 0;
        uint64_t session_epoch = 0;
        uint64_t transform_epoch = 0;
    };

    struct InputTicket {
        uint64_t session_epoch = 0;
        uint32_t action_count = 0;
        uint32_t remaining_polls = 0;
        uint32_t state = SACCADE_TICKET_QUEUED;
        SaccadeResult result = SACCADE_OK;
        bool counted = true;
    };

    struct Fault {
        FaultPoint point = FaultPoint::none;
        SaccadeResult result = SACCADE_OK;
        uint32_t remaining = 0;
    };

    explicit Impl(const Config& value) noexcept : config(value) {
        config.queue_capacity = std::clamp(config.queue_capacity, 1U, 8U);
        config.capture_width = std::clamp(
            config.capture_width, 1U, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
        config.capture_height = std::clamp(
            config.capture_height, 1U, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    }

    SaccadeResult take_fault(FaultPoint point) noexcept {
        if (fault.point != point || fault.remaining == 0) {
            return SACCADE_OK;
        }
        const SaccadeResult result = fault.result;
        --fault.remaining;
        if (fault.remaining == 0) {
            fault = {};
        }
        return result;
    }

    bool start_inference(InferenceTicket& ticket) noexcept {
        if (ticket.state != SACCADE_TICKET_QUEUED) {
            return ticket.state == SACCADE_TICKET_RUNNING;
        }
        ExecutionContext* context = contexts.get(ticket.context);
        if (context == nullptr || context->running_tickets >= context->max_in_flight) {
            return false;
        }
        ticket.state = SACCADE_TICKET_RUNNING;
        ticket.running_counted = true;
        ++context->running_tickets;
        return true;
    }

    void release_running(InferenceTicket& ticket) noexcept {
        if (!ticket.running_counted) {
            return;
        }
        if (ExecutionContext* context = contexts.get(ticket.context)) {
            --context->running_tickets;
        }
        ticket.running_counted = false;
    }

    void finish_inference(InferenceTicket& ticket) noexcept {
        if (ticket.state != SACCADE_TICKET_QUEUED && ticket.state != SACCADE_TICKET_RUNNING) {
            return;
        }
        if (!start_inference(ticket)) {
            return;
        }
        if (ticket.remaining_polls > 0) {
            --ticket.remaining_polls;
        }
        if (ticket.remaining_polls == 0) {
            ticket.state = SACCADE_TICKET_COMPLETE;
            release_running(ticket);
        } else {
            ticket.state = SACCADE_TICKET_RUNNING;
        }
    }

    bool complete_inference(InferenceTicket& ticket) noexcept {
        if (!start_inference(ticket)) {
            return false;
        }
        ticket.remaining_polls = 0;
        ticket.state = SACCADE_TICKET_COMPLETE;
        release_running(ticket);
        return true;
    }

    void release_inference_count(InferenceTicket& ticket) noexcept {
        if (!ticket.counted) {
            return;
        }
        if (ExecutionContext* context = contexts.get(ticket.context)) {
            --context->active_tickets;
        }
        release_running(ticket);
        ticket.counted = false;
    }

    void finish_accessibility(SaccadeTicketHandle handle, AccessibilityTicket& ticket) noexcept {
        if (ticket.state != SACCADE_TICKET_QUEUED && ticket.state != SACCADE_TICKET_RUNNING) {
            return;
        }
        if (ticket.remaining_polls > 0) {
            --ticket.remaining_polls;
        }
        if (ticket.remaining_polls != 0) {
            ticket.state = SACCADE_TICKET_RUNNING;
            return;
        }
        if (ticket.snapshot == 0) {
            const SaccadeResult result = snapshots.emplace(
                &ticket.snapshot,
                Snapshot{handle, ticket.window, ticket.session_epoch, ticket.transform_epoch});
            if (result != SACCADE_OK) {
                ticket.state = SACCADE_TICKET_FAILED;
                ticket.result = result;
            }
        }
        if (ticket.result == SACCADE_OK) {
            ticket.state = SACCADE_TICKET_COMPLETE;
        }
        if (ticket.counted) {
            --active_accessibility;
            ticket.counted = false;
        }
    }

    void complete_accessibility(SaccadeTicketHandle handle, AccessibilityTicket& ticket) noexcept {
        ticket.remaining_polls = 0;
        finish_accessibility(handle, ticket);
    }

    void finish_input(InputTicket& ticket) noexcept {
        if (ticket.state != SACCADE_TICKET_QUEUED && ticket.state != SACCADE_TICKET_RUNNING) {
            return;
        }
        if (ticket.remaining_polls > 0) {
            --ticket.remaining_polls;
        }
        if (ticket.remaining_polls == 0) {
            ticket.state = SACCADE_TICKET_COMPLETE;
            if (ticket.counted) {
                --active_input;
                ticket.counted = false;
            }
        } else {
            ticket.state = SACCADE_TICKET_RUNNING;
        }
    }

    void complete_input(InputTicket& ticket) noexcept {
        ticket.remaining_polls = 0;
        finish_input(ticket);
    }

    static Impl* from(void* context) noexcept {
        if (context == nullptr) {
            return nullptr;
        }
        return &static_cast<Backend*>(context)->impl();
    }

    static SaccadeInferenceStatus inference_status(SaccadeTicketHandle handle,
                                                   const InferenceTicket& ticket) noexcept {
        SaccadeInferenceStatus result{};
        result.struct_size = static_cast<uint32_t>(sizeof(result));
        result.api_version = SACCADE_API_VERSION;
        result.state = ticket.state;
        result.result = ticket.result;
        result.ticket = handle;
        result.frame_id = ticket.frame;
        result.model_epoch = ticket.model_epoch;
        result.session_epoch = ticket.session_epoch;
        result.transform_epoch = ticket.transform_epoch;
        result.produced_bytes = ticket.state == SACCADE_TICKET_COMPLETE
                                    ? static_cast<uint32_t>(inference_output_size)
                                    : 0;
        result.required_bytes = static_cast<uint32_t>(inference_output_size);
        return result;
    }

    static SaccadeAccessibilityStatus
    accessibility_status(SaccadeTicketHandle handle, const AccessibilityTicket& ticket) noexcept {
        SaccadeAccessibilityStatus result{};
        result.struct_size = static_cast<uint32_t>(sizeof(result));
        result.api_version = SACCADE_API_VERSION;
        result.state = ticket.state;
        result.result = ticket.result;
        result.ticket = handle;
        result.snapshot = ticket.snapshot;
        result.session_epoch = ticket.session_epoch;
        result.transform_epoch = ticket.transform_epoch;
        result.target_count = ticket.state == SACCADE_TICKET_COMPLETE ? 1U : 0U;
        result.required_bytes = static_cast<uint32_t>(accessibility_output_size);
        return result;
    }

    static SaccadeInputStatus input_status(SaccadeTicketHandle handle,
                                           const InputTicket& ticket) noexcept {
        SaccadeInputStatus result{};
        result.struct_size = static_cast<uint32_t>(sizeof(result));
        result.api_version = SACCADE_API_VERSION;
        result.state = ticket.state;
        result.result = ticket.result;
        result.ticket = handle;
        result.session_epoch = ticket.session_epoch;
        result.completed_actions =
            ticket.state == SACCADE_TICKET_COMPLETE ? ticket.action_count : 0;
        result.total_actions = ticket.action_count;
        return result;
    }

    static SaccadeResult SACCADE_CALL enumerate_devices(void*, uint32_t, SaccadeDeviceInfo*);
    static SaccadeResult SACCADE_CALL query_model(void*, SaccadeSpanU8, SaccadeModelInfo*);
    static SaccadeResult SACCADE_CALL create_model(void*, const SaccadeModelDesc*,
                                                   SaccadeModelHandle*);
    static SaccadeResult SACCADE_CALL destroy_model(void*, SaccadeModelHandle);
    static SaccadeResult SACCADE_CALL create_context(void*, const SaccadeExecutionContextDesc*,
                                                     SaccadeExecutionContextHandle*);
    static SaccadeResult SACCADE_CALL destroy_context(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL submit_inference(void*, SaccadeExecutionContextHandle,
                                                       const SaccadeInferenceSubmitDesc*,
                                                       SaccadeTicketHandle*);
    static SaccadeResult SACCADE_CALL poll_inference(void*, SaccadeExecutionContextHandle,
                                                     SaccadeTicketHandle, SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL wait_inference(void*, SaccadeExecutionContextHandle,
                                                     SaccadeTicketHandle, uint64_t,
                                                     SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL collect_inference(void*, SaccadeExecutionContextHandle,
                                                        SaccadeTicketHandle, SaccadeMutableSpanU8,
                                                        size_t*);
    static SaccadeResult SACCADE_CALL cancel_inference(void*, SaccadeExecutionContextHandle,
                                                       SaccadeTicketHandle);
    static SaccadeResult SACCADE_CALL reset_inference(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL synchronize_inference(void*, SaccadeExecutionContextHandle,
                                                            uint64_t);
    static SaccadeResult SACCADE_CALL inference_memory(void*, SaccadeExecutionContextHandle,
                                                       SaccadeMemoryStats*);

    static SaccadeResult SACCADE_CALL enumerate_sources(void*, uint32_t, SaccadeCaptureSourceInfo*);
    static SaccadeResult SACCADE_CALL create_stream(void*, const SaccadeCaptureStreamDesc*,
                                                    SaccadeCaptureStreamHandle*);
    static SaccadeResult SACCADE_CALL destroy_stream(void*, SaccadeCaptureStreamHandle);
    static SaccadeResult SACCADE_CALL start_stream(void*, SaccadeCaptureStreamHandle);
    static SaccadeResult SACCADE_CALL stop_stream(void*, SaccadeCaptureStreamHandle);
    static SaccadeResult SACCADE_CALL acquire_frame(void*, SaccadeCaptureStreamHandle, uint64_t,
                                                    SaccadeCapturedFrame*);
    static SaccadeResult SACCADE_CALL copy_damage(void*, SaccadeCaptureStreamHandle,
                                                  SaccadeFrameHandle, SaccadeRectI32*, uint32_t,
                                                  uint32_t*);
    static SaccadeResult SACCADE_CALL release_frame(void*, SaccadeCaptureStreamHandle,
                                                    SaccadeFrameHandle);
    static SaccadeResult SACCADE_CALL synchronize_capture(void*, SaccadeCaptureStreamHandle,
                                                          uint64_t);
    static SaccadeResult SACCADE_CALL capture_memory(void*, SaccadeCaptureStreamHandle,
                                                     SaccadeMemoryStats*);

    static SaccadeResult SACCADE_CALL create_overlay(void*, const SaccadeOverlayDesc*,
                                                     SaccadeOverlayHandle*);
    static SaccadeResult SACCADE_CALL destroy_overlay(void*, SaccadeOverlayHandle);
    static SaccadeResult SACCADE_CALL submit_overlay(void*, SaccadeOverlayHandle,
                                                     const SaccadeOverlayFrameDesc*);
    static SaccadeResult SACCADE_CALL set_overlay_visible(void*, SaccadeOverlayHandle, uint32_t);
    static SaccadeResult SACCADE_CALL synchronize_overlay(void*, SaccadeOverlayHandle, uint64_t);
    static SaccadeResult SACCADE_CALL overlay_memory(void*, SaccadeOverlayHandle,
                                                     SaccadeMemoryStats*);
    static SaccadeResult SACCADE_CALL reset_overlay(void*, SaccadeOverlayHandle);

    static SaccadeResult SACCADE_CALL enumerate_windows(void*, uint32_t, SaccadeWindowInfo*);
    static SaccadeResult SACCADE_CALL request_accessibility(void*,
                                                            const SaccadeAccessibilityQueryDesc*,
                                                            SaccadeTicketHandle*);
    static SaccadeResult SACCADE_CALL poll_accessibility(void*, SaccadeTicketHandle,
                                                         SaccadeAccessibilityStatus*);
    static SaccadeResult SACCADE_CALL wait_accessibility(void*, SaccadeTicketHandle, uint64_t,
                                                         SaccadeAccessibilityStatus*);
    static SaccadeResult SACCADE_CALL collect_accessibility(void*, SaccadeSnapshotHandle,
                                                            SaccadeMutableSpanU8, size_t*);
    static SaccadeResult SACCADE_CALL cancel_accessibility(void*, SaccadeTicketHandle);
    static SaccadeResult SACCADE_CALL release_snapshot(void*, SaccadeSnapshotHandle);
    static SaccadeResult SACCADE_CALL synchronize_accessibility(void*, uint64_t);
    static SaccadeResult SACCADE_CALL accessibility_memory(void*, SaccadeMemoryStats*);

    static SaccadeResult SACCADE_CALL execute_input(void*, const SaccadeInputPlanDesc*,
                                                    SaccadeTicketHandle*);
    static SaccadeResult SACCADE_CALL poll_input(void*, SaccadeTicketHandle, SaccadeInputStatus*);
    static SaccadeResult SACCADE_CALL wait_input(void*, SaccadeTicketHandle, uint64_t,
                                                 SaccadeInputStatus*);
    static SaccadeResult SACCADE_CALL cancel_input(void*, SaccadeTicketHandle);
    static SaccadeResult SACCADE_CALL release_all_input(void*);
    static SaccadeResult SACCADE_CALL synchronize_input(void*, uint64_t);
    static SaccadeResult SACCADE_CALL reset_input(void*);
    static SaccadeResult SACCADE_CALL input_memory(void*, SaccadeMemoryStats*);

    Config config{};
    mutable std::mutex lock;
    Observations observed{};
    Fault fault{};
    core::HandleTable<Model, 4> models;
    core::HandleTable<ExecutionContext, 4> contexts;
    core::HandleTable<InferenceTicket, 8> inference_tickets;
    core::HandleTable<CaptureStream, 4> streams;
    core::HandleTable<CapturedFrame, 8> frames;
    core::HandleTable<Overlay, 4> overlays;
    core::HandleTable<AccessibilityTicket, 8> accessibility_tickets;
    core::HandleTable<Snapshot, 8> snapshots;
    core::HandleTable<InputTicket, 8> input_tickets;
    uint32_t active_accessibility = 0;
    uint32_t active_input = 0;
    uint64_t next_frame_id = 1;
};

Backend::Impl& Backend::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const Backend::Impl& Backend::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

Backend::Backend(const Config& config) noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= alignof(Backend));
    ::new (static_cast<void*>(storage_.data())) Impl(config);
}

Backend::~Backend() {
    impl().~Impl();
}

void Backend::set_fault(FaultPoint point, SaccadeResult result, uint32_t count) noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    state.fault = {point, result, count};
}

void Backend::clear_fault() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    state.fault = {};
}

Observations Backend::observations() const noexcept {
    const Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    return state.observed;
}

SaccadeDeviceInfo Backend::device_info() const noexcept {
    const Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeDeviceInfo result{};
    result.struct_size = static_cast<uint32_t>(sizeof(result));
    result.api_version = SACCADE_API_VERSION;
    result.stable_id = device_id;
    result.capability_bits = state.config.capability_bits;
    result.format_bits = state.config.format_bits;
    result.precision_bits = state.config.precision_bits;
    result.import_bits = state.config.import_bits;
    result.queue_capacity = state.config.queue_capacity;
    result.max_in_flight = state.config.queue_capacity;
    result.host_alignment = 64;
    result.device_alignment = 0;
    result.name = literal_span("deterministic mock device");
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::enumerate_devices(void* context, uint32_t index,
                                                            SaccadeDeviceInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    return write_structure(out_info, static_cast<Backend*>(context)->device_info());
}

SaccadeResult SACCADE_CALL Backend::Impl::query_model(void* context, SaccadeSpanU8 identifier,
                                                      SaccadeModelInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr || out_info == nullptr || identifier.data == nullptr ||
        identifier.size == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    SaccadeModelInfo info{};
    info.stable_id = UINT64_C(0x4D4F434B4001);
    info.required_host_bytes = state->config.memory.host_committed;
    info.required_device_bytes = state->config.memory.device_owned;
    info.capability_bits = state->config.capability_bits;
    info.max_output_bytes = static_cast<uint32_t>(inference_output_size);
    info.name = literal_span("deterministic mock model");
    return write_structure(out_info, info);
}

SaccadeResult SACCADE_CALL Backend::Impl::create_model(void* context, const SaccadeModelDesc* desc,
                                                       SaccadeModelHandle* out_model) {
    Impl* state = from(context);
    if (state == nullptr || out_model == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_model = 0;
    SaccadeModelDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.bytes.data == nullptr || value.bytes.size == 0 || value.stable_id == 0 ||
        value.device_id != device_id || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(state->lock);
    return state->models.emplace(out_model, Model{value.stable_id, value.device_id, 0});
}

SaccadeResult SACCADE_CALL Backend::Impl::destroy_model(void* context, SaccadeModelHandle model) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    Model* value = state->models.get(model);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->context_count != 0) {
        return SACCADE_ERROR_BUSY;
    }
    return state->models.erase(model);
}

SaccadeResult SACCADE_CALL
Backend::Impl::create_context(void* context, const SaccadeExecutionContextDesc* desc,
                              SaccadeExecutionContextHandle* out_execution_context) {
    Impl* state = from(context);
    if (state == nullptr || out_execution_context == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_execution_context = 0;
    SaccadeExecutionContextDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.model == 0 || value.device_id != device_id || value.queue_capacity == 0 ||
        value.max_in_flight == 0 || value.queue_capacity > state->config.queue_capacity ||
        value.max_in_flight > value.queue_capacity || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(state->lock);
    Model* model = state->models.get(value.model);
    if (model == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult result = state->contexts.emplace(
        out_execution_context,
        ExecutionContext{value.model, value.queue_capacity, value.max_in_flight, 0, 0});
    if (result == SACCADE_OK) {
        ++model->context_count;
    }
    return result;
}

SaccadeResult SACCADE_CALL
Backend::Impl::destroy_context(void* context, SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    ExecutionContext* value = state->contexts.get(execution_context);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->active_tickets != 0) {
        return SACCADE_ERROR_BUSY;
    }
    Model* model = state->models.get(value->model);
    if (model != nullptr) {
        --model->context_count;
    }
    return state->contexts.erase(execution_context);
}

SaccadeResult SACCADE_CALL Backend::Impl::submit_inference(
    void* context, SaccadeExecutionContextHandle execution_context,
    const SaccadeInferenceSubmitDesc* desc, SaccadeTicketHandle* out_ticket) {
    Impl* state = from(context);
    if (state == nullptr || out_ticket == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_ticket = 0;
    SaccadeInferenceSubmitDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.frame == 0 || value.scope.width <= 0 || value.scope.height <= 0 ||
        (value.priority_region_count != 0 && value.priority_regions == nullptr) ||
        value.output_capacity < inference_output_size || value.model_epoch == 0 ||
        value.session_epoch == 0 || value.transform_epoch == 0 || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(state->lock);
    ExecutionContext* execution = state->contexts.get(execution_context);
    if (execution == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (execution->active_tickets >= execution->queue_capacity) {
        return SACCADE_ERROR_BUSY;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::inference_submit);
    if (fault != SACCADE_OK) {
        return fault;
    }

    InferenceTicket ticket{};
    ticket.context = execution_context;
    ticket.frame = value.frame;
    ticket.model_epoch = value.model_epoch;
    ticket.session_epoch = value.session_epoch;
    ticket.transform_epoch = value.transform_epoch;
    ticket.remaining_polls = state->config.completion_polls;
    write_u64_le(ticket.output.data(), ticket.frame);
    write_u64_le(ticket.output.data() + 8, ticket.model_epoch);
    write_u64_le(ticket.output.data() + 16, ticket.session_epoch);
    write_u64_le(ticket.output.data() + 24, ticket.transform_epoch);
    if (ticket.remaining_polls == 0) {
        ticket.state = SACCADE_TICKET_COMPLETE;
    }
    const SaccadeResult result = state->inference_tickets.emplace(out_ticket, ticket);
    if (result == SACCADE_OK) {
        ++execution->active_tickets;
        ++state->observed.inference_submissions;
    }
    return result;
}

SaccadeResult SACCADE_CALL
Backend::Impl::poll_inference(void* context, SaccadeExecutionContextHandle execution_context,
                              SaccadeTicketHandle handle, SaccadeInferenceStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InferenceTicket* ticket = state->inference_tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->finish_inference(*ticket);
    return write_structure(out_status, inference_status(handle, *ticket));
}

SaccadeResult SACCADE_CALL Backend::Impl::wait_inference(
    void* context, SaccadeExecutionContextHandle execution_context, SaccadeTicketHandle handle,
    uint64_t timeout_ns, SaccadeInferenceStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InferenceTicket* ticket = state->inference_tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if ((ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) &&
        timeout_ns == 0) {
        const SaccadeResult output = write_structure(out_status, inference_status(handle, *ticket));
        return output == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : output;
    }
    if (ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) {
        if (!state->complete_inference(*ticket)) {
            const SaccadeResult output =
                write_structure(out_status, inference_status(handle, *ticket));
            return output == SACCADE_OK ? SACCADE_ERROR_BUSY : output;
        }
    }
    return write_structure(out_status, inference_status(handle, *ticket));
}

SaccadeResult SACCADE_CALL Backend::Impl::collect_inference(
    void* context, SaccadeExecutionContextHandle execution_context, SaccadeTicketHandle handle,
    SaccadeMutableSpanU8 output, size_t* out_required) {
    Impl* state = from(context);
    if (state == nullptr || out_required == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_required = inference_output_size;
    std::lock_guard<std::mutex> guard(state->lock);
    InferenceTicket* ticket = state->inference_tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state == SACCADE_TICKET_CANCELLED || ticket->state == SACCADE_TICKET_FAILED) {
        const SaccadeResult result = ticket->result;
        state->release_inference_count(*ticket);
        (void)state->inference_tickets.erase(handle);
        return result;
    }
    if (ticket->state != SACCADE_TICKET_COMPLETE) {
        return SACCADE_ERROR_BUSY;
    }
    if (output.data == nullptr || output.size < inference_output_size) {
        return SACCADE_ERROR_CAPACITY;
    }
    std::memcpy(output.data, ticket->output.data(), inference_output_size);
    state->release_inference_count(*ticket);
    return state->inference_tickets.erase(handle);
}

SaccadeResult SACCADE_CALL Backend::Impl::cancel_inference(
    void* context, SaccadeExecutionContextHandle execution_context, SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InferenceTicket* ticket = state->inference_tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state != SACCADE_TICKET_QUEUED && ticket->state != SACCADE_TICKET_RUNNING) {
        return SACCADE_ERROR_STATE;
    }
    ticket->state = SACCADE_TICKET_CANCELLED;
    ticket->result = SACCADE_ERROR_CANCELLED;
    state->release_inference_count(*ticket);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL
Backend::Impl::reset_inference(void* context, SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    ExecutionContext* execution = state->contexts.get(execution_context);
    if (execution == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    std::array<SaccadeTicketHandle, 8> handles{};
    size_t count = 0;
    state->inference_tickets.for_each(
        [&](SaccadeTicketHandle handle, const InferenceTicket& ticket) noexcept {
            if (ticket.context == execution_context) {
                handles[count++] = handle;
            }
        });
    for (size_t index = 0; index < count; ++index) {
        if (InferenceTicket* ticket = state->inference_tickets.get(handles[index])) {
            state->release_inference_count(*ticket);
        }
        (void)state->inference_tickets.erase(handles[index]);
    }
    execution->active_tickets = 0;
    execution->running_tickets = 0;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize_inference(
    void* context, SaccadeExecutionContextHandle execution_context, uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->contexts.get(execution_context) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::inference_synchronize);
    if (fault != SACCADE_OK) {
        return fault;
    }
    if (timeout_ns == 0) {
        bool pending = false;
        state->inference_tickets.for_each([&](SaccadeTicketHandle,
                                              const InferenceTicket& ticket) noexcept {
            if (ticket.context == execution_context &&
                (ticket.state == SACCADE_TICKET_QUEUED || ticket.state == SACCADE_TICKET_RUNNING)) {
                pending = true;
            }
        });
        return pending ? SACCADE_ERROR_TIMEOUT : SACCADE_OK;
    }

    for (;;) {
        bool pending = false;
        bool progressed = false;
        state->inference_tickets.for_each([&](SaccadeTicketHandle,
                                              InferenceTicket& ticket) noexcept {
            if (ticket.context == execution_context &&
                (ticket.state == SACCADE_TICKET_QUEUED || ticket.state == SACCADE_TICKET_RUNNING)) {
                pending = true;
                progressed = state->complete_inference(ticket) || progressed;
            }
        });
        if (!pending) {
            return SACCADE_OK;
        }
        if (!progressed) {
            return SACCADE_ERROR_BUSY;
        }
    }
}

SaccadeResult SACCADE_CALL Backend::Impl::inference_memory(
    void* context, SaccadeExecutionContextHandle execution_context, SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->contexts.get(execution_context) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return write_structure(out_stats, memory_stats(state->config.memory));
}

SaccadeResult SACCADE_CALL Backend::Impl::enumerate_sources(void* context, uint32_t index,
                                                            SaccadeCaptureSourceInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr || out_info == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    SaccadeCaptureSourceInfo info{};
    info.stable_id = capture_source_id;
    info.kind = SACCADE_CAPTURE_SOURCE_DISPLAY;
    info.capability_bits = state->config.capability_bits;
    info.desktop_bounds = {0, 0, static_cast<int32_t>(state->config.capture_width),
                           static_cast<int32_t>(state->config.capture_height)};
    info.name = literal_span("deterministic display");
    return write_structure(out_info, info);
}

SaccadeResult SACCADE_CALL Backend::Impl::create_stream(void* context,
                                                        const SaccadeCaptureStreamDesc* desc,
                                                        SaccadeCaptureStreamHandle* out_stream) {
    Impl* state = from(context);
    if (state == nullptr || out_stream == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = 0;
    SaccadeCaptureStreamDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.source_id != capture_source_id || value.pixel_format == 0 ||
        (value.pixel_format & state->config.format_bits) == 0 || value.queue_capacity == 0 ||
        value.queue_capacity > state->config.queue_capacity || value.max_width == 0 ||
        value.max_height == 0 || value.max_width > state->config.capture_width ||
        value.max_height > state->config.capture_height || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(state->lock);
    return state->streams.emplace(out_stream, CaptureStream{value.source_id, value.queue_capacity,
                                                            value.max_width, value.max_height,
                                                            value.pixel_format, 0, false});
}

SaccadeResult SACCADE_CALL Backend::Impl::destroy_stream(void* context,
                                                         SaccadeCaptureStreamHandle stream) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    CaptureStream* value = state->streams.get(stream);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->started || value->outstanding_frames != 0) {
        return SACCADE_ERROR_BUSY;
    }
    return state->streams.erase(stream);
}

SaccadeResult SACCADE_CALL Backend::Impl::start_stream(void* context,
                                                       SaccadeCaptureStreamHandle stream) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    CaptureStream* value = state->streams.get(stream);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->started) {
        return SACCADE_ERROR_STATE;
    }
    value->started = true;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::stop_stream(void* context,
                                                      SaccadeCaptureStreamHandle stream) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    CaptureStream* value = state->streams.get(stream);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!value->started) {
        return SACCADE_ERROR_STATE;
    }
    value->started = false;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::acquire_frame(void* context,
                                                        SaccadeCaptureStreamHandle stream, uint64_t,
                                                        SaccadeCapturedFrame* out_frame) {
    Impl* state = from(context);
    if (state == nullptr || out_frame == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    CaptureStream* value = state->streams.get(stream);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!value->started) {
        return SACCADE_ERROR_STATE;
    }
    if (value->outstanding_frames >= value->queue_capacity) {
        return SACCADE_ERROR_BUSY;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::capture_acquire);
    if (fault != SACCADE_OK) {
        return fault;
    }

    const uint64_t frame_id = state->next_frame_id++;
    SaccadeFrameHandle frame = 0;
    const SaccadeResult result = state->frames.emplace(
        &frame, CapturedFrame{stream, frame_id, frame_id, frame_id * UINT64_C(1000000),
                              value->width, value->height, value->pixel_format});
    if (result != SACCADE_OK) {
        return result;
    }

    SaccadeCapturedFrame output{};
    output.frame = frame;
    output.source_id = value->source;
    output.frame_id = frame_id;
    output.transform_epoch = frame_id;
    output.timestamp_ns = frame_id * UINT64_C(1000000);
    output.width = value->width;
    output.height = value->height;
    output.pixel_format = value->pixel_format;
    output.damage_count = 1;
    const SaccadeResult output_result = write_structure(out_frame, output);
    if (output_result != SACCADE_OK) {
        (void)state->frames.erase(frame);
        return output_result;
    }
    ++value->outstanding_frames;
    ++state->observed.captured_frames;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::copy_damage(void* context,
                                                      SaccadeCaptureStreamHandle stream,
                                                      SaccadeFrameHandle frame,
                                                      SaccadeRectI32* rectangles, uint32_t capacity,
                                                      uint32_t* out_count) {
    Impl* state = from(context);
    if (state == nullptr || out_count == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_count = 1;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->streams.get(stream) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const CapturedFrame* value = state->frames.get(frame);
    if (value == nullptr || value->stream != stream) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (capacity < 1) {
        return SACCADE_ERROR_CAPACITY;
    }
    if (rectangles == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    rectangles[0] = {0, 0, static_cast<int32_t>(value->width), static_cast<int32_t>(value->height)};
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::release_frame(void* context,
                                                        SaccadeCaptureStreamHandle stream,
                                                        SaccadeFrameHandle frame) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    CaptureStream* stream_value = state->streams.get(stream);
    CapturedFrame* frame_value = state->frames.get(frame);
    if (stream_value == nullptr || frame_value == nullptr || frame_value->stream != stream) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    --stream_value->outstanding_frames;
    return state->frames.erase(frame);
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize_capture(void* context,
                                                              SaccadeCaptureStreamHandle stream,
                                                              uint64_t) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->streams.get(stream) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return state->take_fault(FaultPoint::capture_synchronize);
}

SaccadeResult SACCADE_CALL Backend::Impl::capture_memory(void* context,
                                                         SaccadeCaptureStreamHandle stream,
                                                         SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->streams.get(stream) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return write_structure(out_stats, memory_stats(state->config.memory));
}

SaccadeResult SACCADE_CALL Backend::Impl::create_overlay(void* context,
                                                         const SaccadeOverlayDesc* desc,
                                                         SaccadeOverlayHandle* out_overlay) {
    Impl* state = from(context);
    if (state == nullptr || out_overlay == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_overlay = 0;
    SaccadeOverlayDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.source_id == 0 || value.desktop_bounds.width <= 0 ||
        value.desktop_bounds.height <= 0 || value.queue_capacity == 0 ||
        value.queue_capacity > state->config.queue_capacity || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    return state->overlays.emplace(out_overlay, Overlay{value.desktop_bounds, 0, 0});
}

SaccadeResult SACCADE_CALL Backend::Impl::destroy_overlay(void* context,
                                                          SaccadeOverlayHandle overlay) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->overlays.get(overlay) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return state->overlays.erase(overlay);
}

SaccadeResult SACCADE_CALL Backend::Impl::submit_overlay(void* context,
                                                         SaccadeOverlayHandle overlay,
                                                         const SaccadeOverlayFrameDesc* desc) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeOverlayFrameDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.scene_epoch == 0 || value.transform_epoch == 0 ||
        (value.commands.size != 0 && value.commands.data == nullptr) || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    Overlay* target = state->overlays.get(overlay);
    if (target == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::overlay_submit);
    if (fault != SACCADE_OK) {
        return fault;
    }
    ++target->submissions;
    ++state->observed.overlay_submissions;
    state->observed.last_scene_epoch = value.scene_epoch;
    state->observed.last_transform_epoch = value.transform_epoch;
    state->observed.last_command_hash = hash_bytes(value.commands);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::set_overlay_visible(void* context,
                                                              SaccadeOverlayHandle overlay,
                                                              uint32_t visible) {
    Impl* state = from(context);
    if (state == nullptr || visible > 1) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    Overlay* target = state->overlays.get(overlay);
    if (target == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    target->visible = visible;
    state->observed.overlay_visible = visible;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize_overlay(void* context,
                                                              SaccadeOverlayHandle overlay,
                                                              uint64_t) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->overlays.get(overlay) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::overlay_memory(void* context,
                                                         SaccadeOverlayHandle overlay,
                                                         SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->overlays.get(overlay) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return write_structure(out_stats, memory_stats(state->config.memory));
}

SaccadeResult SACCADE_CALL Backend::Impl::reset_overlay(void* context,
                                                        SaccadeOverlayHandle overlay) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    Overlay* target = state->overlays.get(overlay);
    if (target == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    target->submissions = 0;
    target->visible = 0;
    state->observed.overlay_visible = 0;
    state->observed.last_scene_epoch = 0;
    state->observed.last_transform_epoch = 0;
    state->observed.last_command_hash = 0;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::enumerate_windows(void* context, uint32_t index,
                                                            SaccadeWindowInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr || out_info == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    SaccadeWindowInfo info{};
    info.stable_id = window_id;
    info.process_id = 100;
    info.desktop_bounds = {0, 0, static_cast<int32_t>(state->config.capture_width),
                           static_cast<int32_t>(state->config.capture_height)};
    info.title = literal_span("deterministic window");
    return write_structure(out_info, info);
}

SaccadeResult SACCADE_CALL Backend::Impl::request_accessibility(
    void* context, const SaccadeAccessibilityQueryDesc* desc, SaccadeTicketHandle* out_ticket) {
    Impl* state = from(context);
    if (state == nullptr || out_ticket == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_ticket = 0;
    SaccadeAccessibilityQueryDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.window_id != window_id || value.scope.width <= 0 || value.scope.height <= 0 ||
        value.target_capacity == 0 || value.flags != 0 || value.session_epoch == 0 ||
        value.transform_epoch == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->active_accessibility >= state->config.queue_capacity) {
        return SACCADE_ERROR_BUSY;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::accessibility_request);
    if (fault != SACCADE_OK) {
        return fault;
    }
    AccessibilityTicket ticket{};
    ticket.window = value.window_id;
    ticket.session_epoch = value.session_epoch;
    ticket.transform_epoch = value.transform_epoch;
    ticket.remaining_polls = state->config.completion_polls;
    const SaccadeResult result = state->accessibility_tickets.emplace(out_ticket, ticket);
    if (result == SACCADE_OK) {
        ++state->active_accessibility;
        ++state->observed.accessibility_requests;
        if (ticket.remaining_polls == 0) {
            AccessibilityTicket* stored = state->accessibility_tickets.get(*out_ticket);
            if (stored != nullptr) {
                state->complete_accessibility(*out_ticket, *stored);
            }
        }
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::poll_accessibility(
    void* context, SaccadeTicketHandle handle, SaccadeAccessibilityStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    AccessibilityTicket* ticket = state->accessibility_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->finish_accessibility(handle, *ticket);
    const bool terminal_without_snapshot =
        ticket->state == SACCADE_TICKET_CANCELLED ||
        (ticket->state == SACCADE_TICKET_FAILED && ticket->snapshot == 0);
    const SaccadeResult result = write_structure(out_status, accessibility_status(handle, *ticket));
    if (result == SACCADE_OK && terminal_without_snapshot) {
        (void)state->accessibility_tickets.erase(handle);
    }
    return result;
}

SaccadeResult SACCADE_CALL
Backend::Impl::wait_accessibility(void* context, SaccadeTicketHandle handle, uint64_t timeout_ns,
                                  SaccadeAccessibilityStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    AccessibilityTicket* ticket = state->accessibility_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if ((ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) &&
        timeout_ns == 0) {
        const SaccadeResult output =
            write_structure(out_status, accessibility_status(handle, *ticket));
        return output == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : output;
    }
    if (ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) {
        state->complete_accessibility(handle, *ticket);
    }
    const bool terminal_without_snapshot =
        ticket->state == SACCADE_TICKET_CANCELLED ||
        (ticket->state == SACCADE_TICKET_FAILED && ticket->snapshot == 0);
    const SaccadeResult result = write_structure(out_status, accessibility_status(handle, *ticket));
    if (result == SACCADE_OK && terminal_without_snapshot) {
        (void)state->accessibility_tickets.erase(handle);
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::collect_accessibility(void* context,
                                                                SaccadeSnapshotHandle snapshot,
                                                                SaccadeMutableSpanU8 output,
                                                                size_t* out_required) {
    Impl* state = from(context);
    if (state == nullptr || out_required == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_required = accessibility_output_size;
    std::lock_guard<std::mutex> guard(state->lock);
    const Snapshot* value = state->snapshots.get(snapshot);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (output.data == nullptr || output.size < accessibility_output_size) {
        return SACCADE_ERROR_CAPACITY;
    }
    write_u64_le(output.data, value->window);
    write_u64_le(output.data + 8, value->session_epoch);
    write_u64_le(output.data + 16, value->transform_epoch);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::cancel_accessibility(void* context,
                                                               SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    AccessibilityTicket* ticket = state->accessibility_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state != SACCADE_TICKET_QUEUED && ticket->state != SACCADE_TICKET_RUNNING) {
        return SACCADE_ERROR_STATE;
    }
    ticket->state = SACCADE_TICKET_CANCELLED;
    ticket->result = SACCADE_ERROR_CANCELLED;
    if (ticket->counted) {
        --state->active_accessibility;
        ticket->counted = false;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::release_snapshot(void* context,
                                                           SaccadeSnapshotHandle snapshot) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    const Snapshot* value = state->snapshots.get(snapshot);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeTicketHandle ticket = value->ticket;
    const SaccadeResult result = state->snapshots.erase(snapshot);
    if (result == SACCADE_OK) {
        (void)state->accessibility_tickets.erase(ticket);
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize_accessibility(void* context,
                                                                    uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    const SaccadeResult fault = state->take_fault(FaultPoint::accessibility_synchronize);
    if (fault != SACCADE_OK) {
        return fault;
    }
    if (state->active_accessibility != 0 && timeout_ns == 0) {
        return SACCADE_ERROR_TIMEOUT;
    }
    state->accessibility_tickets.for_each(
        [&](SaccadeTicketHandle handle, AccessibilityTicket& ticket) noexcept {
            if (ticket.state == SACCADE_TICKET_QUEUED || ticket.state == SACCADE_TICKET_RUNNING) {
                state->complete_accessibility(handle, ticket);
            }
        });
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::accessibility_memory(void* context,
                                                               SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    return write_structure(out_stats, memory_stats(state->config.memory));
}

SaccadeResult SACCADE_CALL Backend::Impl::execute_input(void* context,
                                                        const SaccadeInputPlanDesc* desc,
                                                        SaccadeTicketHandle* out_ticket) {
    Impl* state = from(context);
    if (state == nullptr || out_ticket == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_ticket = 0;
    SaccadeInputPlanDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.session_epoch == 0 || value.transform_epoch == 0 || value.actions.data == nullptr ||
        value.actions.size == 0 || value.actions.size > UINT32_MAX || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->active_input >= state->config.queue_capacity) {
        return SACCADE_ERROR_BUSY;
    }
    const SaccadeResult fault = state->take_fault(FaultPoint::input_execute);
    if (fault != SACCADE_OK) {
        return fault;
    }
    InputTicket ticket{};
    ticket.session_epoch = value.session_epoch;
    ticket.action_count = static_cast<uint32_t>(value.actions.size);
    ticket.remaining_polls = state->config.completion_polls;
    const SaccadeResult result = state->input_tickets.emplace(out_ticket, ticket);
    if (result == SACCADE_OK) {
        ++state->active_input;
        ++state->observed.input_executions;
        if (ticket.remaining_polls == 0) {
            InputTicket* stored = state->input_tickets.get(*out_ticket);
            if (stored != nullptr) {
                state->complete_input(*stored);
            }
        }
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::poll_input(void* context, SaccadeTicketHandle handle,
                                                     SaccadeInputStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InputTicket* ticket = state->input_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->finish_input(*ticket);
    const bool terminal = ticket->state == SACCADE_TICKET_COMPLETE ||
                          ticket->state == SACCADE_TICKET_CANCELLED ||
                          ticket->state == SACCADE_TICKET_FAILED;
    const SaccadeResult result = write_structure(out_status, input_status(handle, *ticket));
    if (result == SACCADE_OK && terminal) {
        (void)state->input_tickets.erase(handle);
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::wait_input(void* context, SaccadeTicketHandle handle,
                                                     uint64_t timeout_ns,
                                                     SaccadeInputStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InputTicket* ticket = state->input_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if ((ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) &&
        timeout_ns == 0) {
        const SaccadeResult output = write_structure(out_status, input_status(handle, *ticket));
        return output == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : output;
    }
    if (ticket->state == SACCADE_TICKET_QUEUED || ticket->state == SACCADE_TICKET_RUNNING) {
        state->complete_input(*ticket);
    }
    const bool terminal = ticket->state == SACCADE_TICKET_COMPLETE ||
                          ticket->state == SACCADE_TICKET_CANCELLED ||
                          ticket->state == SACCADE_TICKET_FAILED;
    const SaccadeResult result = write_structure(out_status, input_status(handle, *ticket));
    if (result == SACCADE_OK && terminal) {
        (void)state->input_tickets.erase(handle);
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::cancel_input(void* context, SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    InputTicket* ticket = state->input_tickets.get(handle);
    if (ticket == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state != SACCADE_TICKET_QUEUED && ticket->state != SACCADE_TICKET_RUNNING) {
        return SACCADE_ERROR_STATE;
    }
    ticket->state = SACCADE_TICKET_CANCELLED;
    ticket->result = SACCADE_ERROR_CANCELLED;
    if (ticket->counted) {
        --state->active_input;
        ticket->counted = false;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::release_all_input(void* context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    const SaccadeResult fault = state->take_fault(FaultPoint::input_release_all);
    if (fault != SACCADE_OK) {
        return fault;
    }
    state->input_tickets.for_each([](SaccadeTicketHandle, InputTicket& ticket) noexcept {
        if (ticket.state == SACCADE_TICKET_QUEUED || ticket.state == SACCADE_TICKET_RUNNING) {
            ticket.state = SACCADE_TICKET_CANCELLED;
            ticket.result = SACCADE_ERROR_CANCELLED;
            ticket.counted = false;
        }
    });
    state->active_input = 0;
    ++state->observed.release_all_calls;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize_input(void* context, uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    const SaccadeResult fault = state->take_fault(FaultPoint::input_synchronize);
    if (fault != SACCADE_OK) {
        return fault;
    }
    if (state->active_input != 0 && timeout_ns == 0) {
        return SACCADE_ERROR_TIMEOUT;
    }
    state->input_tickets.for_each([&](SaccadeTicketHandle, InputTicket& ticket) noexcept {
        if (ticket.state == SACCADE_TICKET_QUEUED || ticket.state == SACCADE_TICKET_RUNNING) {
            state->complete_input(ticket);
        }
    });
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::reset_input(void* context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    state->input_tickets.clear_reverse([](SaccadeTicketHandle, InputTicket&) noexcept {});
    state->active_input = 0;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::input_memory(void* context,
                                                       SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(state->lock);
    return write_structure(out_stats, memory_stats(state->config.memory));
}

SaccadeInferenceProviderDesc Backend::inference_provider() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeInferenceOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = detail::guarded_callback<&Impl::enumerate_devices>;
    ops.query_model = detail::guarded_callback<&Impl::query_model>;
    ops.create_model = detail::guarded_callback<&Impl::create_model>;
    ops.destroy_model = detail::guarded_callback<&Impl::destroy_model>;
    ops.create_context = detail::guarded_callback<&Impl::create_context>;
    ops.destroy_context = detail::guarded_callback<&Impl::destroy_context>;
    ops.submit = detail::guarded_callback<&Impl::submit_inference>;
    ops.poll = detail::guarded_callback<&Impl::poll_inference>;
    ops.wait = detail::guarded_callback<&Impl::wait_inference>;
    ops.collect = detail::guarded_callback<&Impl::collect_inference>;
    ops.cancel = detail::guarded_callback<&Impl::cancel_inference>;
    ops.reset = detail::guarded_callback<&Impl::reset_inference>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize_inference>;
    ops.memory_stats = detail::guarded_callback<&Impl::inference_memory>;

    SaccadeInferenceProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_INFERENCE, inference_provider_id,
                              state.config.capability_bits, "deterministic mock inference");
    desc.context = this;
    desc.ops = ops;
    return desc;
}

SaccadeCaptureProviderDesc Backend::capture_provider() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeCaptureOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_sources = detail::guarded_callback<&Impl::enumerate_sources>;
    ops.create = detail::guarded_callback<&Impl::create_stream>;
    ops.destroy = detail::guarded_callback<&Impl::destroy_stream>;
    ops.start = detail::guarded_callback<&Impl::start_stream>;
    ops.stop = detail::guarded_callback<&Impl::stop_stream>;
    ops.acquire = detail::guarded_callback<&Impl::acquire_frame>;
    ops.copy_damage = detail::guarded_callback<&Impl::copy_damage>;
    ops.release = detail::guarded_callback<&Impl::release_frame>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize_capture>;
    ops.memory_stats = detail::guarded_callback<&Impl::capture_memory>;

    SaccadeCaptureProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_CAPTURE, capture_provider_id,
                              state.config.capability_bits, "deterministic mock capture");
    desc.context = this;
    desc.ops = ops;
    return desc;
}

SaccadeOverlayProviderDesc Backend::overlay_provider() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeOverlayOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.create = detail::guarded_callback<&Impl::create_overlay>;
    ops.destroy = detail::guarded_callback<&Impl::destroy_overlay>;
    ops.submit = detail::guarded_callback<&Impl::submit_overlay>;
    ops.set_visible = detail::guarded_callback<&Impl::set_overlay_visible>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize_overlay>;
    ops.memory_stats = detail::guarded_callback<&Impl::overlay_memory>;
    ops.reset = detail::guarded_callback<&Impl::reset_overlay>;

    SaccadeOverlayProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_OVERLAY, overlay_provider_id,
                              state.config.capability_bits, "deterministic mock overlay");
    desc.context = this;
    desc.ops = ops;
    return desc;
}

SaccadeAccessibilityProviderDesc Backend::accessibility_provider() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeAccessibilityOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_windows = detail::guarded_callback<&Impl::enumerate_windows>;
    ops.request = detail::guarded_callback<&Impl::request_accessibility>;
    ops.poll = detail::guarded_callback<&Impl::poll_accessibility>;
    ops.wait = detail::guarded_callback<&Impl::wait_accessibility>;
    ops.collect = detail::guarded_callback<&Impl::collect_accessibility>;
    ops.cancel = detail::guarded_callback<&Impl::cancel_accessibility>;
    ops.release = detail::guarded_callback<&Impl::release_snapshot>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize_accessibility>;
    ops.memory_stats = detail::guarded_callback<&Impl::accessibility_memory>;

    SaccadeAccessibilityProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_ACCESSIBILITY, accessibility_provider_id,
                              state.config.capability_bits, "deterministic mock accessibility");
    desc.context = this;
    desc.ops = ops;
    return desc;
}

SaccadeInputProviderDesc Backend::input_provider() noexcept {
    Impl& state = impl();
    std::lock_guard<std::mutex> guard(state.lock);
    SaccadeInputOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.execute = detail::guarded_callback<&Impl::execute_input>;
    ops.poll = detail::guarded_callback<&Impl::poll_input>;
    ops.wait = detail::guarded_callback<&Impl::wait_input>;
    ops.cancel = detail::guarded_callback<&Impl::cancel_input>;
    ops.release_all = detail::guarded_callback<&Impl::release_all_input>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize_input>;
    ops.reset = detail::guarded_callback<&Impl::reset_input>;
    ops.memory_stats = detail::guarded_callback<&Impl::input_memory>;

    SaccadeInputProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_INPUT, input_provider_id,
                              state.config.capability_bits, "deterministic mock input");
    desc.context = this;
    desc.ops = ops;
    return desc;
}

} // namespace saccade::backend::mock

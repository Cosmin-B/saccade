#include "platform/macos/coreml_provider.hpp"

#include "backends/callback_guard.hpp"
#include "core/cache_line.hpp"
#include "model/coreml_contract.hpp"

#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <semaphore>
#include <thread>

namespace saccade::platform::macos {
namespace {

constexpr uint64_t provider_id = UINT64_C(0x434F52454D4C0001);
constexpr uint64_t device_id = UINT64_C(0x434F52454D4C1001);
constexpr uint32_t slot = 1;
constexpr uint32_t maximum_path_bytes = 1024;
constexpr uint32_t provider_base_capabilities = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                                SACCADE_PROVIDER_CAPABILITY_ASYNC |
                                                SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
constexpr uint32_t supported_precision_bits = SACCADE_PRECISION_FP16 | SACCADE_PRECISION_FP32;
constexpr size_t maximum_packet_bytes =
    sizeof(SaccadeTargetPacketHeader) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

uint64_t make_handle(uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32U) | slot;
}

bool decode_handle(uint64_t handle, uint32_t generation) noexcept {
    return static_cast<uint32_t>(handle) == slot && static_cast<uint32_t>(handle >> 32U) == generation &&
           generation != 0;
}

uint32_t next_generation(uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1U : value;
}

SaccadeSpanU8 literal_span(const char* text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text), std::strlen(text)};
}

template <typename Structure> SaccadeResult read_structure(const Structure* source, Structure* output) noexcept {
    if (source == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t size = 0;
    std::memcpy(&size, source, sizeof(size));
    if (size < offsetof(Structure, reserved)) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    const size_t copied = std::min(static_cast<size_t>(size), sizeof(*output));
    std::memcpy(output, source, copied);
    if (api_major(output->api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    const auto* bytes = static_cast<const uint8_t*>(static_cast<const void*>(output));
    for (size_t index = offsetof(Structure, reserved); index < copied; ++index) {
        if (bytes[index] != 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Structure> SaccadeResult write_structure(Structure* output, Structure value) noexcept {
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t size = 0;
    uint32_t version = 0;
    std::memcpy(&size, output, sizeof(size));
    std::memcpy(&version,
                static_cast<const uint8_t*>(static_cast<const void*>(output)) + offsetof(Structure, api_version),
                sizeof(version));
    if (size < offsetof(Structure, reserved)) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (api_major(version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    const size_t copied = std::min(static_cast<size_t>(size), sizeof(value));
    value.struct_size = static_cast<uint32_t>(copied);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(output, &value, copied);
    return SACCADE_OK;
}

bool copy_model_root(const char* source, std::array<char, maximum_path_bytes>* output) noexcept {
    if (source == nullptr || output == nullptr) return false;
    size_t size = 0;
    while (source[size] != '\0') {
        if (size + 1U == output->size()) return false;
        ++size;
    }
    if (size == 0) return false;
    std::memcpy(output->data(), source, size + 1U);
    return true;
}

bool config_valid(const CoreMlProviderConfig& config) noexcept {
    const bool compute_policy_valid = config.compute_policy == CoreMlComputePolicy::all ||
                                      config.compute_policy == CoreMlComputePolicy::cpu_and_gpu ||
                                      config.compute_policy == CoreMlComputePolicy::cpu_only ||
                                      config.compute_policy == CoreMlComputePolicy::cpu_and_neural_engine;
    return config.model_root != nullptr && config.verifier.verify != nullptr && compute_policy_valid &&
           config.reserved[0] == 0 && config.reserved[1] == 0 && config.reserved[2] == 0;
}

uint32_t available_compute_capabilities() noexcept {
    uint32_t capabilities = SACCADE_PROVIDER_CAPABILITY_CPU;
    @autoreleasepool {
        for (id<MLComputeDeviceProtocol> device in MLModel.availableComputeDevices) {
            if ([device isKindOfClass:MLGPUComputeDevice.class])
                capabilities |= SACCADE_PROVIDER_CAPABILITY_GPU;
            else if ([device isKindOfClass:MLNeuralEngineComputeDevice.class])
                capabilities |= SACCADE_PROVIDER_CAPABILITY_ACCELERATOR;
        }
    }
    return capabilities;
}

uint32_t capabilities_for_policy(CoreMlComputePolicy policy, uint32_t available) noexcept {
    uint32_t allowed = 0;
    switch (policy) {
    case CoreMlComputePolicy::all:
        allowed =
            SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_ACCELERATOR;
        break;
    case CoreMlComputePolicy::cpu_and_gpu:
        allowed = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_GPU;
        break;
    case CoreMlComputePolicy::cpu_only:
        allowed = SACCADE_PROVIDER_CAPABILITY_CPU;
        break;
    case CoreMlComputePolicy::cpu_and_neural_engine:
        allowed = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_ACCELERATOR;
        break;
    }
    return provider_base_capabilities | (available & allowed);
}

bool frame_valid(const SaccadeInferenceDispatchDesc& desc) noexcept {
    return desc.frame.storage == SACCADE_FRAME_STORAGE_IOSURFACE && desc.frame.native_id != 0 &&
           desc.frame.pixel_format == SACCADE_FORMAT_BGRA8 && desc.frame.width != 0 && desc.frame.height != 0 &&
           desc.frame.frame_id != 0 && desc.scope.x == 0 && desc.scope.y == 0 &&
           desc.scope.width == static_cast<int32_t>(desc.frame.width) &&
           desc.scope.height == static_cast<int32_t>(desc.frame.height) && desc.model_epoch != 0 &&
           desc.session_epoch != 0 && desc.transform_epoch != 0 && desc.topology_epoch != 0 && desc.source_id != 0 &&
           desc.flags == 0 && desc.priority_region_count == 0 && desc.priority_regions == nullptr;
}

CVPixelBufferRef pixel_buffer_from_iosurface(const SaccadeFrameResourceView& frame) noexcept {
    IOSurfaceRef surface = IOSurfaceLookup(static_cast<IOSurfaceID>(frame.native_id));
    if (surface == nullptr) return nullptr;
    if (IOSurfaceGetWidth(surface) != frame.width || IOSurfaceGetHeight(surface) != frame.height) {
        CFRelease(surface);
        return nullptr;
    }
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVReturn result = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &pixel_buffer);
    CFRelease(surface);
    return result == kCVReturnSuccess ? pixel_buffer : nullptr;
}

} // namespace

struct CoreMlInferenceProvider::Impl {
    struct Ticket {
        alignas(SaccadeTargetPacketHeader) std::array<uint8_t, maximum_packet_bytes> output_{};
        CVPixelBufferRef pixel_buffer_ = nullptr;
        SaccadeInferenceDispatchDesc dispatch_{};
        uint32_t output_size_ = 0;
        SaccadeResult result_ = SACCADE_OK;
        bool active_ = false;
    };

    struct alignas(core::destructive_interference_size) WorkerControl {
        std::atomic<uint32_t> state_{0};
        std::atomic<bool> cancel_requested_{false};
        std::atomic<bool> stop_requested_{false};
        std::array<std::byte,
                   core::destructive_interference_size - sizeof(std::atomic<uint32_t>) - 2U * sizeof(std::atomic<bool>)>
            padding_{};
    };

    static_assert(sizeof(WorkerControl) == core::destructive_interference_size);

    static Impl* from(void* context) noexcept {
        return context == nullptr ? nullptr : &static_cast<CoreMlInferenceProvider*>(context)->impl();
    }

    void release_ticket() noexcept {
        if (ticket_.pixel_buffer_ != nullptr) {
            CVPixelBufferRelease(ticket_.pixel_buffer_);
        }
        ticket_ = {};
        control_.cancel_requested_.store(false, std::memory_order_relaxed);
        control_.state_.store(0, std::memory_order_relaxed);
    }

    void process_ticket() noexcept {
        control_.state_.store(SACCADE_TICKET_RUNNING, std::memory_order_release);
        if (control_.cancel_requested_.load(std::memory_order_acquire)) {
            ticket_.result_ = SACCADE_ERROR_CANCELLED;
            control_.state_.store(SACCADE_TICKET_CANCELLED, std::memory_order_release);
            completion_.release();
            return;
        }
        CoreMlPrediction prediction{};
        prediction.pixel_buffer = ticket_.pixel_buffer_;
        prediction.width = ticket_.dispatch_.frame.width;
        prediction.height = ticket_.dispatch_.frame.height;
        prediction.pixel_format = ticket_.dispatch_.frame.pixel_format;
        prediction.scope = ticket_.dispatch_.scope;
        prediction.epochs = {ticket_.dispatch_.frame.frame_id, ticket_.dispatch_.model_epoch,
                             ticket_.dispatch_.session_epoch,  ticket_.dispatch_.transform_epoch,
                             ticket_.dispatch_.topology_epoch, ticket_.dispatch_.source_id};
        CoreMlPredictionResult output{};
        ticket_.result_ = model_.predict(prediction, {ticket_.output_.data(), ticket_.output_.size()}, &output);
        if (ticket_.result_ != SACCADE_OK) {
            control_.state_.store(SACCADE_TICKET_FAILED, std::memory_order_release);
            completion_.release();
            return;
        }
        if (control_.cancel_requested_.load(std::memory_order_acquire)) {
            ticket_.output_size_ = 0;
            ticket_.result_ = SACCADE_ERROR_CANCELLED;
            control_.state_.store(SACCADE_TICKET_CANCELLED, std::memory_order_release);
            completion_.release();
            return;
        }
        ticket_.output_size_ = static_cast<uint32_t>(output.byte_size);
        control_.state_.store(SACCADE_TICKET_COMPLETE, std::memory_order_release);
        completion_.release();
    }

    void worker_loop() noexcept {
        for (;;) {
            command_.acquire();
            if (control_.stop_requested_.load(std::memory_order_acquire)) return;
            if (control_.state_.load(std::memory_order_acquire) == SACCADE_TICKET_QUEUED) {
                process_ticket();
            }
        }
    }

    void stop_worker() noexcept {
        if (!worker_.joinable()) return;
        control_.stop_requested_.store(true, std::memory_order_release);
        command_.release();
        worker_.join();
        control_.stop_requested_.store(false, std::memory_order_relaxed);
    }

    static SaccadeInferenceStatus status(uint64_t handle, const Ticket& ticket, uint32_t state) noexcept {
        SaccadeInferenceStatus output{};
        output.struct_size = sizeof(output);
        output.api_version = SACCADE_API_VERSION;
        output.state = state;
        output.result = ticket.result_;
        output.ticket = handle;
        output.frame_id = ticket.dispatch_.frame.frame_id;
        output.model_epoch = ticket.dispatch_.model_epoch;
        output.session_epoch = ticket.dispatch_.session_epoch;
        output.transform_epoch = ticket.dispatch_.transform_epoch;
        output.topology_epoch = ticket.dispatch_.topology_epoch;
        output.source_id = ticket.dispatch_.source_id;
        output.produced_bytes = state == SACCADE_TICKET_COMPLETE ? ticket.output_size_ : 0;
        output.required_bytes = ticket.output_size_;
        return output;
    }

    static SaccadeResult SACCADE_CALL enumerate_devices(void*, uint32_t, SaccadeDeviceInfo*);
    static SaccadeResult SACCADE_CALL query_model(void*, SaccadeSpanU8, SaccadeModelInfo*);
    static SaccadeResult SACCADE_CALL create_model(void*, const SaccadeModelDesc*, SaccadeModelHandle*);
    static SaccadeResult SACCADE_CALL destroy_model(void*, SaccadeModelHandle);
    static SaccadeResult SACCADE_CALL create_context(void*, const SaccadeExecutionContextDesc*,
                                                     SaccadeExecutionContextHandle*);
    static SaccadeResult SACCADE_CALL destroy_context(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL submit(void*, SaccadeExecutionContextHandle, const SaccadeInferenceDispatchDesc*,
                                             SaccadeTicketHandle*);
    static SaccadeResult SACCADE_CALL poll(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                           SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL wait(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, uint64_t,
                                           SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL collect(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                              SaccadeMutableSpanU8, size_t*);
    static SaccadeResult SACCADE_CALL cancel(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle);
    static SaccadeResult SACCADE_CALL reset(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL synchronize(void*, SaccadeExecutionContextHandle, uint64_t);
    static SaccadeResult SACCADE_CALL memory_stats(void*, SaccadeExecutionContextHandle, SaccadeMemoryStats*);

    CoreMlModel model_{};
    std::array<char, maximum_path_bytes> model_root_{};
    model::ArtifactVerifier verifier_{};
    CoreMlComputePolicy compute_policy_ = CoreMlComputePolicy::cpu_and_neural_engine;
    uint32_t capability_bits_ = 0;
    bool allow_low_precision_gpu_ = false;
    WorkerControl control_{};
    std::binary_semaphore command_{0};
    std::binary_semaphore completion_{0};
    std::thread worker_{};
    Ticket ticket_{};
    uint64_t copied_bytes_ = 0;
    uint32_t model_generation_ = 1;
    uint32_t context_generation_ = 1;
    uint32_t ticket_generation_ = 1;
    bool initialized_ = false;
    bool model_live_ = false;
    bool context_live_ = false;
};

CoreMlInferenceProvider::CoreMlInferenceProvider() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 128);
    ::new (static_cast<void*>(storage_.data())) Impl{};
}

CoreMlInferenceProvider::~CoreMlInferenceProvider() {
    Impl& state = impl();
    state.stop_worker();
    if (state.ticket_.active_) state.release_ticket();
    if (state.model_live_) (void)state.model_.shutdown();
    state.~Impl();
}

CoreMlInferenceProvider::Impl& CoreMlInferenceProvider::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const CoreMlInferenceProvider::Impl& CoreMlInferenceProvider::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult CoreMlInferenceProvider::initialize(CoreMlProviderConfig config) noexcept {
    Impl& state = impl();
    if (state.initialized_ || !config_valid(config) || !copy_model_root(config.model_root, &state.model_root_)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state.verifier_ = config.verifier;
    state.compute_policy_ = config.compute_policy;
    state.capability_bits_ = capabilities_for_policy(config.compute_policy, available_compute_capabilities());
    state.allow_low_precision_gpu_ = config.allow_low_precision_gpu;
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult CoreMlInferenceProvider::shutdown() noexcept {
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_ERROR_STATE;
    if (state.ticket_.active_) return SACCADE_ERROR_BUSY;
    if (state.context_live_) return SACCADE_ERROR_BUSY;
    if (state.model_live_) {
        const SaccadeResult result = state.model_.shutdown();
        if (result != SACCADE_OK) return result;
        state.model_live_ = false;
    }
    state.verifier_ = {};
    state.model_root_ = {};
    state.capability_bits_ = 0;
    state.allow_low_precision_gpu_ = false;
    state.initialized_ = false;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::enumerate_devices(void* context, uint32_t index,
                                                                            SaccadeDeviceInfo* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (index != 0) return SACCADE_ERROR_NOT_FOUND;
    SaccadeDeviceInfo info{};
    info.stable_id = device_id;
    info.capability_bits = state->capability_bits_;
    info.format_bits = SACCADE_FORMAT_BGRA8;
    info.precision_bits = SACCADE_PRECISION_FP16 | SACCADE_PRECISION_FP32;
    info.import_bits = SACCADE_IMPORT_IOSURFACE;
    info.queue_capacity = 1;
    info.max_in_flight = 1;
    info.device_alignment = 64;
    info.name = literal_span("Core ML unified device");
    return write_structure(output, info);
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::query_model(void* context, SaccadeSpanU8 bytes,
                                                                      SaccadeModelInfo* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    model::ArtifactView artifact{};
    const SaccadeResult parsed = model::parse_artifact(bytes, &artifact);
    if (parsed != SACCADE_OK) return parsed;
    model::coreml::Contract contract{};
    const SaccadeResult contracted = model::coreml::parse_contract(artifact, &contract);
    if (contracted != SACCADE_OK) return contracted;
    if ((artifact.precision_bits & supported_precision_bits) == 0 ||
        (artifact.precision_bits & ~supported_precision_bits) != 0) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    SaccadeModelInfo info{};
    info.stable_id = artifact.stable_id;
    info.required_host_bytes = CoreMlInferenceProvider::storage_size;
    info.capability_bits = state->capability_bits_;
    info.max_output_bytes = artifact.max_output_bytes;
    info.name = literal_span("Core ML UI detector");
    return write_structure(output, info);
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::create_model(void* context,
                                                                       const SaccadeModelDesc* description,
                                                                       SaccadeModelHandle* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    if (state->model_live_) return SACCADE_ERROR_BUSY;
    SaccadeModelDesc value{};
    const SaccadeResult read = read_structure(description, &value);
    if (read != SACCADE_OK) return read;
    if (value.stable_id == 0 || value.device_id != device_id || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    model::ArtifactView artifact{};
    const SaccadeResult parsed = model::parse_artifact(value.bytes, &artifact);
    if (parsed != SACCADE_OK) return parsed;
    if (artifact.stable_id != value.stable_id) return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult verified = model::verify_artifact(artifact, state->verifier_);
    if (verified != SACCADE_OK) return verified;
    CoreMlModelConfig config{};
    config.model_root = state->model_root_.data();
    config.compute_policy = state->compute_policy_;
    config.allow_low_precision_gpu = state->allow_low_precision_gpu_;
    const SaccadeResult initialized = state->model_.initialize(artifact, config);
    if (initialized != SACCADE_OK) return initialized;
    state->model_live_ = true;
    *output = make_handle(state->model_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::destroy_model(void* context, SaccadeModelHandle model) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->model_live_ || !decode_handle(model, state->model_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->context_live_ || state->ticket_.active_) return SACCADE_ERROR_BUSY;
    const SaccadeResult result = state->model_.shutdown();
    if (result != SACCADE_OK) return result;
    state->model_live_ = false;
    state->model_generation_ = next_generation(state->model_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::create_context(void* context,
                                                                         const SaccadeExecutionContextDesc* description,
                                                                         SaccadeExecutionContextHandle* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    SaccadeExecutionContextDesc value{};
    const SaccadeResult read = read_structure(description, &value);
    if (read != SACCADE_OK) return read;
    if (!state->model_live_ || !decode_handle(value.model, state->model_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->context_live_ || value.device_id != device_id || value.queue_capacity != 1 || value.max_in_flight != 1 ||
        value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    state->worker_ = std::thread([state]() noexcept { state->worker_loop(); });
    state->context_live_ = true;
    *output = make_handle(state->context_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL
CoreMlInferenceProvider::Impl::destroy_context(void* context, SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->ticket_.active_) return SACCADE_ERROR_BUSY;
    state->stop_worker();
    state->context_live_ = false;
    state->context_generation_ = next_generation(state->context_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::submit(void* context,
                                                                 SaccadeExecutionContextHandle execution_context,
                                                                 const SaccadeInferenceDispatchDesc* description,
                                                                 SaccadeTicketHandle* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeInferenceDispatchDesc value{};
    const SaccadeResult read = read_structure(description, &value);
    if (read != SACCADE_OK) return read;
    if (!frame_valid(value) || value.output_capacity < state->model_.maximum_output_bytes()) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state->ticket_.active_) return SACCADE_ERROR_BUSY;
    CVPixelBufferRef pixel_buffer = pixel_buffer_from_iosurface(value.frame);
    if (pixel_buffer == nullptr) return SACCADE_ERROR_BACKEND;
    state->ticket_ = {};
    state->ticket_.pixel_buffer_ = pixel_buffer;
    state->ticket_.dispatch_ = value;
    state->ticket_.active_ = true;
    state->control_.cancel_requested_.store(false, std::memory_order_relaxed);
    (void)state->completion_.try_acquire();
    state->control_.state_.store(SACCADE_TICKET_QUEUED, std::memory_order_release);
    state->command_.release();
    *output = make_handle(state->ticket_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::poll(void* context,
                                                               SaccadeExecutionContextHandle execution_context,
                                                               SaccadeTicketHandle handle,
                                                               SaccadeInferenceStatus* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
    return write_structure(output, status(handle, state->ticket_, ticket_state));
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::wait(void* context,
                                                               SaccadeExecutionContextHandle execution_context,
                                                               SaccadeTicketHandle handle, uint64_t timeout_ns,
                                                               SaccadeInferenceStatus* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
    if (timeout_ns == 0 && (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING)) {
        const SaccadeResult wrote = write_structure(output, status(handle, state->ticket_, ticket_state));
        return wrote == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : wrote;
    }
    if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) {
        using Duration = std::chrono::nanoseconds;
        const uint64_t maximum = static_cast<uint64_t>(Duration::max().count());
        const Duration timeout(static_cast<Duration::rep>(std::min(timeout_ns, maximum)));
        if (!state->completion_.try_acquire_for(timeout)) {
            ticket_state = state->control_.state_.load(std::memory_order_acquire);
            const SaccadeResult wrote = write_structure(output, status(handle, state->ticket_, ticket_state));
            return wrote == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : wrote;
        }
        ticket_state = state->control_.state_.load(std::memory_order_acquire);
    }
    return write_structure(output, status(handle, state->ticket_, ticket_state));
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::collect(void* context,
                                                                  SaccadeExecutionContextHandle execution_context,
                                                                  SaccadeTicketHandle handle,
                                                                  SaccadeMutableSpanU8 output, size_t* required) {
    Impl* state = from(context);
    if (state == nullptr || required == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *required = 0;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
    *required = state->ticket_.output_size_;
    if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) return SACCADE_ERROR_BUSY;
    if (ticket_state == SACCADE_TICKET_CANCELLED || ticket_state == SACCADE_TICKET_FAILED) {
        const SaccadeResult result = state->ticket_.result_;
        state->release_ticket();
        state->ticket_generation_ = next_generation(state->ticket_generation_);
        return result;
    }
    if (output.data == nullptr || output.size < state->ticket_.output_size_) {
        return SACCADE_ERROR_CAPACITY;
    }
    std::memcpy(output.data, state->ticket_.output_.data(), state->ticket_.output_size_);
    state->copied_bytes_ += state->ticket_.output_size_;
    state->release_ticket();
    state->ticket_generation_ = next_generation(state->ticket_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::cancel(void* context,
                                                                 SaccadeExecutionContextHandle execution_context,
                                                                 SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
    if (ticket_state != SACCADE_TICKET_QUEUED && ticket_state != SACCADE_TICKET_RUNNING) return SACCADE_ERROR_STATE;
    state->control_.cancel_requested_.store(true, std::memory_order_release);
    state->completion_.acquire();
    state->ticket_.output_size_ = 0;
    state->ticket_.result_ = SACCADE_ERROR_CANCELLED;
    state->control_.state_.store(SACCADE_TICKET_CANCELLED, std::memory_order_release);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::reset(void* context,
                                                                SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->ticket_.active_) {
        const uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
        if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) {
            state->control_.cancel_requested_.store(true, std::memory_order_release);
            state->completion_.acquire();
        }
        state->release_ticket();
        state->ticket_generation_ = next_generation(state->ticket_generation_);
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::synchronize(void* context,
                                                                      SaccadeExecutionContextHandle execution_context,
                                                                      uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->ticket_.active_) {
        const uint32_t ticket_state = state->control_.state_.load(std::memory_order_acquire);
        if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) {
            if (timeout_ns == 0) return SACCADE_ERROR_TIMEOUT;
            using Duration = std::chrono::nanoseconds;
            const uint64_t maximum = static_cast<uint64_t>(Duration::max().count());
            const Duration timeout(static_cast<Duration::rep>(std::min(timeout_ns, maximum)));
            if (!state->completion_.try_acquire_for(timeout)) {
                return SACCADE_ERROR_TIMEOUT;
            }
        }
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL CoreMlInferenceProvider::Impl::memory_stats(void* context,
                                                                       SaccadeExecutionContextHandle execution_context,
                                                                       SaccadeMemoryStats* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeMemoryStats stats{};
    stats.host_committed = sizeof(Impl);
    stats.host_reserved = CoreMlInferenceProvider::storage_size;
    stats.device_imported = state->ticket_.active_ ? static_cast<uint64_t>(state->ticket_.dispatch_.frame.width) *
                                                         state->ticket_.dispatch_.frame.height * 4U
                                                   : 0;
    stats.copied_bytes = state->copied_bytes_;
    stats.high_water_bytes = stats.host_committed + stats.device_imported;
    return write_structure(output, stats);
}

SaccadeInferenceProviderDesc CoreMlInferenceProvider::descriptor() noexcept {
    Impl& state = impl();
    SaccadeInferenceOps ops{};
    ops.struct_size = sizeof(ops);
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = backend::detail::guarded_callback<&Impl::enumerate_devices>;
    ops.query_model = backend::detail::guarded_callback<&Impl::query_model>;
    ops.create_model = backend::detail::guarded_callback<&Impl::create_model>;
    ops.destroy_model = backend::detail::guarded_callback<&Impl::destroy_model>;
    ops.create_context = backend::detail::guarded_callback<&Impl::create_context>;
    ops.destroy_context = backend::detail::guarded_callback<&Impl::destroy_context>;
    ops.submit = backend::detail::guarded_callback<&Impl::submit>;
    ops.poll = backend::detail::guarded_callback<&Impl::poll>;
    ops.wait = backend::detail::guarded_callback<&Impl::wait>;
    ops.collect = backend::detail::guarded_callback<&Impl::collect>;
    ops.cancel = backend::detail::guarded_callback<&Impl::cancel>;
    ops.reset = backend::detail::guarded_callback<&Impl::reset>;
    ops.synchronize = backend::detail::guarded_callback<&Impl::synchronize>;
    ops.memory_stats = backend::detail::guarded_callback<&Impl::memory_stats>;

    SaccadeProviderInfo info{};
    info.struct_size = sizeof(info);
    info.api_version = SACCADE_API_VERSION;
    info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    info.capability_bits = state.capability_bits_;
    info.stable_id = provider_id;
    info.name = literal_span("Core ML inference");

    SaccadeInferenceProviderDesc description{};
    description.struct_size = sizeof(description);
    description.api_version = SACCADE_API_VERSION;
    description.info = info;
    description.context = this;
    description.ops = ops;
    return description;
}

} // namespace saccade::platform::macos

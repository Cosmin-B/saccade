#include "backends/d3d12/directml_provider.hpp"

#include "backends/callback_guard.hpp"
#include "backends/d3d12/directml_inference.hpp"
#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/preprocessor.hpp"
#include "backends/d3d12/target_postprocessor.hpp"
#include "model/directml_contract.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>

namespace saccade::backend::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t provider_id = UINT64_C(0x4449524543544D4C);
constexpr uint32_t slot = 1;
constexpr size_t maximum_path_bytes = 1024;
constexpr size_t maximum_name_bytes = 64;
constexpr size_t maximum_packet_bytes =
    sizeof(SaccadeTargetPacketHeader) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
constexpr uint32_t base_capability_bits = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                          SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_CANCELLATION;

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

uint64_t performance_counter() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) != 0 ? static_cast<uint64_t>(value.QuadPart) : 0;
}

uint64_t elapsed_ns(uint64_t start, uint64_t end, uint64_t frequency) noexcept {
    if (start == 0 || end < start || frequency == 0) return 0;
    const uint64_t elapsed = end - start;
    return elapsed / frequency * UINT64_C(1'000'000'000) + elapsed % frequency * UINT64_C(1'000'000'000) / frequency;
}

SaccadeSpanU8 literal_span(const char* text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text), std::strlen(text)};
}

template <typename Structure> SaccadeResult read_structure(const Structure* source, Structure* output) noexcept {
    if (source == nullptr || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t size = 0;
    std::memcpy(&size, source, sizeof(size));
    if (size < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
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
    if (size < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (api_major(version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    const size_t copied = std::min(static_cast<size_t>(size), sizeof(value));
    value.struct_size = static_cast<uint32_t>(copied);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(output, &value, copied);
    return SACCADE_OK;
}

bool copy_text(const char* source, std::array<char, maximum_path_bytes>* output) noexcept {
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

bool copy_name(SaccadeSpanU8 source, std::array<char, maximum_name_bytes + 1U>* output) noexcept {
    if (source.data == nullptr || source.size == 0 || source.size > maximum_name_bytes) return false;
    std::memcpy(output->data(), source.data, source.size);
    (*output)[source.size] = '\0';
    return true;
}

DWORD timeout_milliseconds(uint64_t timeout_ns) noexcept {
    constexpr uint64_t nanoseconds_per_millisecond = 1'000'000;
    if (timeout_ns == 0) return 0;
    const uint64_t milliseconds =
        timeout_ns / nanoseconds_per_millisecond + (timeout_ns % nanoseconds_per_millisecond != 0 ? 1U : 0U);
    return static_cast<DWORD>(std::min<uint64_t>(milliseconds, INFINITE - 1U));
}

bool frame_valid(const SaccadeInferenceDispatchDesc& desc) noexcept {
    return desc.frame.storage == SACCADE_FRAME_STORAGE_WIN32_CAPTURE && desc.frame.native_id != 0 &&
           desc.frame.pixel_format == SACCADE_FORMAT_BGRA8 && desc.frame.width != 0 && desc.frame.height != 0 &&
           (desc.frame.ready_fence == 0) == (desc.frame.ready_value == 0) && desc.frame.frame_id != 0 &&
           desc.scope.x == 0 && desc.scope.y == 0 && desc.scope.width == static_cast<int32_t>(desc.frame.width) &&
           desc.scope.height == static_cast<int32_t>(desc.frame.height) && desc.model_epoch != 0 &&
           desc.session_epoch != 0 && desc.transform_epoch != 0 && desc.topology_epoch != 0 && desc.source_id != 0 &&
           desc.flags == 0 && desc.priority_regions == nullptr && desc.priority_region_count == 0;
}

} // namespace

struct DirectMlInferenceProvider::Impl {
    struct Ticket {
        alignas(SaccadeTargetPacketHeader) std::array<uint8_t, maximum_packet_bytes> output_{};
        SaccadeInferenceDispatchDesc dispatch_{};
        uint32_t output_size_ = 0;
        SaccadeResult result_ = SACCADE_OK;
        bool active_ = false;
    };

    static Impl* from(void* context) noexcept {
        return context == nullptr ? nullptr : &static_cast<DirectMlInferenceProvider*>(context)->impl();
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

    void complete(SaccadeResult result, uint32_t state) noexcept {
        ticket_.result_ = result;
        control_state_.store(state, std::memory_order_release);
        (void)SetEvent(completion_event_);
    }

    void process_ticket() noexcept {
        control_state_.store(SACCADE_TICKET_RUNNING, std::memory_order_release);
        if (cancel_requested_.load(std::memory_order_acquire)) {
            complete(SACCADE_ERROR_CANCELLED, SACCADE_TICKET_CANCELLED);
            return;
        }
        const bool profiling = profile_stages_;
        uint64_t started = profiling ? performance_counter() : 0;
        TextureLease texture{};
        ComPtr<ID3D12Resource> direct_texture;
        IUnknown* native_texture = reinterpret_cast<IUnknown*>(ticket_.dispatch_.frame.native_id);
        SaccadeResult result = native_texture->QueryInterface(IID_PPV_ARGS(direct_texture.GetAddressOf())) == S_OK
                                   ? SACCADE_OK
                                   : graphics_.unwrap(reinterpret_cast<ID3D11Texture2D*>(native_texture), &texture);
        if (profiling) {
            pipeline_stats_.import_ns += elapsed_ns(started, performance_counter(), performance_frequency_);
            started = performance_counter();
        }
        if (direct_texture != nullptr) {
            ++pipeline_stats_.direct_imports;
        } else if (result == SACCADE_OK) {
            ++pipeline_stats_.wrapped_imports;
        }
        ID3D12Resource* input_texture = direct_texture != nullptr ? direct_texture.Get() : texture.texture;
        PreprocessSubmission preprocess_submission{};
        if (result == SACCADE_OK && ticket_.dispatch_.frame.ready_fence != 0) {
            result =
                SUCCEEDED(graphics_.queue()->Wait(reinterpret_cast<ID3D12Fence*>(ticket_.dispatch_.frame.ready_fence),
                                                  ticket_.dispatch_.frame.ready_value))
                    ? SACCADE_OK
                    : SACCADE_ERROR_BACKEND;
        }
        if (result == SACCADE_OK) {
            result = preprocessor_.submit(input_texture, ticket_.dispatch_.frame.width, ticket_.dispatch_.frame.height,
                                          {0, 0, ticket_.dispatch_.frame.width, ticket_.dispatch_.frame.height},
                                          ticket_.dispatch_.frame.frame_id, ticket_.dispatch_.transform_epoch,
                                          &preprocess_submission);
        }
        if (profiling && result == SACCADE_OK) {
            result = preprocessor_.wait(&preprocess_submission, UINT64_MAX);
        }
        if (texture.texture != nullptr) {
            ID3D12Fence* fence = nullptr;
            uint64_t fence_value = 0;
            if (result == SACCADE_OK) {
                result = preprocessor_.completion_dependency(&preprocess_submission, &fence, &fence_value);
            }
            const SaccadeResult returned = graphics_.return_texture(&texture, fence, fence_value);
            if (result == SACCADE_OK) result = returned;
        }
        if (profiling) {
            pipeline_stats_.preprocess_ns += elapsed_ns(started, performance_counter(), performance_frequency_);
            started = performance_counter();
        }
        if (result == SACCADE_OK) {
            result = inference_.run();
            // ORT must publish the bound output before our postprocess list consumes it on the shared queue.
            if (result == SACCADE_OK) result = inference_.synchronize_outputs();
        }
        if (profiling) {
            pipeline_stats_.inference_ns += elapsed_ns(started, performance_counter(), performance_frequency_);
            started = performance_counter();
        }
        TargetPostprocessSubmission postprocess_submission{};
        if (result == SACCADE_OK) {
            kernels::targets::PostprocessConfig config{};
            config.maximum_targets = target_capacity_;
            config.minimum_confidence_q16 = minimum_confidence_q16_;
            config.band_minimum_confidence_q16 = band_minimum_confidence_q16_;
            config.band_min_short_side_q3 = band_min_short_side_q3_;
            config.band_max_short_side_q3 = band_max_short_side_q3_;
            config.iou_threshold_q16 = iou_threshold_q16_;
            config.coordinate_space = SACCADE_COORDINATE_SPACE_SOURCE_Q8;
            const kernels::targets::PostprocessEpochs epochs{
                ticket_.dispatch_.frame.frame_id,  ticket_.dispatch_.model_epoch,    ticket_.dispatch_.session_epoch,
                ticket_.dispatch_.transform_epoch, ticket_.dispatch_.topology_epoch, ticket_.dispatch_.source_id};
            result = postprocessor_.submit(candidate_capacity_, ticket_.dispatch_.frame.width,
                                           ticket_.dispatch_.frame.height, config, epochs, &postprocess_submission);
        }
        if (result == SACCADE_OK) {
            result = postprocessor_.wait(postprocess_submission, UINT64_MAX);
        }
        TargetPacketSpan packet{};
        if (result == SACCADE_OK) {
            result = postprocessor_.packet(postprocess_submission, &packet);
        }
        if (result == SACCADE_OK && packet.size <= ticket_.output_.size()) {
            std::memcpy(ticket_.output_.data(), packet.data, packet.size);
            ticket_.output_size_ = static_cast<uint32_t>(packet.size);
        } else if (result == SACCADE_OK) {
            result = SACCADE_ERROR_CAPACITY;
        }
        if (profiling) {
            pipeline_stats_.postprocess_ns += elapsed_ns(started, performance_counter(), performance_frequency_);
        }
        ++pipeline_stats_.tickets;
        if (cancel_requested_.load(std::memory_order_acquire)) {
            ticket_.output_size_ = 0;
            complete(SACCADE_ERROR_CANCELLED, SACCADE_TICKET_CANCELLED);
        } else if (result != SACCADE_OK) {
            complete(result, SACCADE_TICKET_FAILED);
        } else {
            complete(SACCADE_OK, SACCADE_TICKET_COMPLETE);
        }
    }

    void worker_loop() noexcept {
        const SaccadeResult adopted_graphics = graphics_.adopt_current_thread();
        const SaccadeResult adopted_preprocessor = preprocessor_.adopt_current_thread();
        const SaccadeResult adopted_inference = inference_.adopt_current_thread();
        const SaccadeResult adopted_postprocessor = postprocessor_.adopt_current_thread();
        worker_start_result_ = adopted_graphics != SACCADE_OK       ? adopted_graphics
                               : adopted_preprocessor != SACCADE_OK ? adopted_preprocessor
                               : adopted_inference != SACCADE_OK    ? adopted_inference
                                                                    : adopted_postprocessor;
        (void)SetEvent(worker_ready_event_);
        if (worker_start_result_ != SACCADE_OK) return;
        for (;;) {
            (void)WaitForSingleObject(command_event_, INFINITE);
            if (stop_requested_.load(std::memory_order_acquire)) return;
            if (control_state_.load(std::memory_order_acquire) == SACCADE_TICKET_QUEUED) {
                process_ticket();
            }
        }
    }

    void release_ticket() noexcept {
        ticket_ = {};
        cancel_requested_.store(false, std::memory_order_relaxed);
        control_state_.store(0, std::memory_order_relaxed);
        (void)ResetEvent(completion_event_);
    }

    void stop_worker() noexcept {
        if (!worker_.joinable()) return;
        stop_requested_.store(true, std::memory_order_release);
        (void)SetEvent(command_event_);
        worker_.join();
        stop_requested_.store(false, std::memory_order_relaxed);
    }

    void reset_model_objects() noexcept {
        (void)graphics_.adopt_current_thread();
        (void)postprocessor_.adopt_current_thread();
        (void)inference_.adopt_current_thread();
        (void)preprocessor_.adopt_current_thread();
        postprocessor_.~TargetPostprocessor();
        inference_.~DirectMlInference();
        preprocessor_.~ImagePreprocessor();
        new (&postprocessor_) TargetPostprocessor{};
        new (&inference_) DirectMlInference{};
        new (&preprocessor_) ImagePreprocessor{};
        candidate_buffer_.Reset();
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

    GraphicsDevice graphics_{};
    ImagePreprocessor preprocessor_{};
    DirectMlInference inference_{};
    TargetPostprocessor postprocessor_{};
    ComPtr<ID3D12Resource> candidate_buffer_{};
    std::array<char, maximum_path_bytes> shader_directory_{};
    std::array<char, maximum_name_bytes + 1U> input_name_{};
    std::array<char, maximum_name_bytes + 1U> candidate_name_{};
    model::ArtifactVerifier verifier_{};
    DirectMlPipelineStats pipeline_stats_{};
    Ticket ticket_{};
    std::atomic<uint32_t> control_state_{0};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> stop_requested_{false};
    HANDLE command_event_ = nullptr;
    HANDLE completion_event_ = nullptr;
    HANDLE worker_ready_event_ = nullptr;
    std::thread worker_{};
    SaccadeResult worker_start_result_ = SACCADE_OK;
    uint64_t copied_bytes_ = 0;
    uint64_t model_stable_id_ = 0;
    uint32_t candidate_capacity_ = 0;
    uint32_t target_capacity_ = 0;
    uint16_t minimum_confidence_q16_ = 0;
    uint16_t band_minimum_confidence_q16_ = 0;
    uint16_t band_min_short_side_q3_ = 0;
    uint16_t band_max_short_side_q3_ = 0;
    uint16_t iou_threshold_q16_ = 0;
    uint32_t maximum_output_bytes_ = 0;
    uint32_t precision_bits_ = 0;
    uint32_t capability_bits_ = base_capability_bits | SACCADE_PROVIDER_CAPABILITY_GPU;
    uint64_t device_id_ = 0;
    uint64_t performance_frequency_ = 0;
    uint32_t model_generation_ = 1;
    uint32_t context_generation_ = 1;
    uint32_t ticket_generation_ = 1;
    DirectMlModelStage model_stage_ = DirectMlModelStage::none;
    bool profile_stages_ = false;
    bool initialized_ = false;
    bool model_live_ = false;
    bool context_live_ = false;
};

static_assert(sizeof(DirectMlInferenceProvider::Impl) <= DirectMlInferenceProvider::storage_size);
static_assert(alignof(DirectMlInferenceProvider::Impl) <= 64);

DirectMlInferenceProvider::DirectMlInferenceProvider() noexcept {
    new (storage_.data()) Impl{};
}

DirectMlInferenceProvider::~DirectMlInferenceProvider() {
    Impl& state = impl();
    state.stop_worker();
    if (state.model_live_) state.reset_model_objects();
    if (state.command_event_ != nullptr) (void)CloseHandle(state.command_event_);
    if (state.completion_event_ != nullptr) (void)CloseHandle(state.completion_event_);
    if (state.worker_ready_event_ != nullptr) (void)CloseHandle(state.worker_ready_event_);
    state.~Impl();
}

DirectMlInferenceProvider::Impl& DirectMlInferenceProvider::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const DirectMlInferenceProvider::Impl& DirectMlInferenceProvider::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult DirectMlInferenceProvider::initialize(DirectMlProviderConfig config) noexcept {
    Impl& state = impl();
    if (state.initialized_ || config.verifier.verify == nullptr ||
        config.execution_policy < DirectMlExecutionPolicy::hardware_only ||
        config.execution_policy > DirectMlExecutionPolicy::hardware_then_software || config.reserved[0] != 0 ||
        config.reserved[1] != 0 || config.reserved[2] != 0 ||
        !copy_text(config.shader_directory, &state.shader_directory_)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const DevicePreference preference = config.execution_policy == DirectMlExecutionPolicy::software_only
                                            ? DevicePreference::software_only
                                        : config.execution_policy == DirectMlExecutionPolicy::hardware_then_software
                                            ? DevicePreference::hardware_then_software
                                            : DevicePreference::hardware_only;
    const SaccadeResult initialized = state.graphics_.initialize(preference, config.device_stable_id);
    if (initialized != SACCADE_OK) return initialized;
    const bool software = state.graphics_.software_device();
    state.capability_bits_ =
        base_capability_bits | (software ? SACCADE_PROVIDER_CAPABILITY_CPU : SACCADE_PROVIDER_CAPABILITY_GPU);
    state.device_id_ = state.graphics_.adapter_luid();
    if (state.device_id_ == 0) {
        (void)state.graphics_.shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    state.command_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state.completion_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state.worker_ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.command_event_ == nullptr || state.completion_event_ == nullptr || state.worker_ready_event_ == nullptr) {
        return SACCADE_ERROR_BACKEND;
    }
    state.verifier_ = config.verifier;
    LARGE_INTEGER performance_frequency{};
    if (config.profile_stages && QueryPerformanceFrequency(&performance_frequency) == 0) {
        return SACCADE_ERROR_BACKEND;
    }
    state.performance_frequency_ = static_cast<uint64_t>(performance_frequency.QuadPart);
    state.profile_stages_ = config.profile_stages;
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DirectMlInferenceProvider::shutdown() noexcept {
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_ERROR_STATE;
    if (state.context_live_ || state.ticket_.active_) return SACCADE_ERROR_BUSY;
    if (state.model_live_) {
        state.reset_model_objects();
        state.model_live_ = false;
    }
    state.stop_worker();
    if (state.command_event_ != nullptr) (void)CloseHandle(state.command_event_);
    if (state.completion_event_ != nullptr) (void)CloseHandle(state.completion_event_);
    if (state.worker_ready_event_ != nullptr) (void)CloseHandle(state.worker_ready_event_);
    state.command_event_ = nullptr;
    state.completion_event_ = nullptr;
    state.worker_ready_event_ = nullptr;
    const SaccadeResult graphics_shutdown = state.graphics_.shutdown();
    if (graphics_shutdown != SACCADE_OK) return graphics_shutdown;
    state.verifier_ = {};
    state.shader_directory_ = {};
    state.capability_bits_ = base_capability_bits | SACCADE_PROVIDER_CAPABILITY_GPU;
    state.device_id_ = 0;
    state.initialized_ = false;
    return SACCADE_OK;
}

ID3D11Device* DirectMlInferenceProvider::capture_device() const noexcept {
    return impl().graphics_.capture_device();
}

ID3D12Device* DirectMlInferenceProvider::graphics_device() const noexcept {
    return impl().initialized_ ? impl().graphics_.device() : nullptr;
}

uint64_t DirectMlInferenceProvider::adapter_luid() const noexcept {
    return impl().initialized_ ? impl().graphics_.adapter_luid() : 0;
}

DirectMlModelStage DirectMlInferenceProvider::model_stage() const noexcept {
    return impl().model_stage_;
}

DirectMlPipelineStats DirectMlInferenceProvider::pipeline_stats() const noexcept {
    return impl().pipeline_stats_;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::enumerate_devices(void* context, uint32_t index,
                                                                              SaccadeDeviceInfo* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) return SACCADE_ERROR_NOT_FOUND;
    SaccadeDeviceInfo info{};
    info.stable_id = state->device_id_;
    info.capability_bits = state->capability_bits_;
    info.format_bits = SACCADE_FORMAT_BGRA8;
    info.precision_bits = SACCADE_PRECISION_FP16 | SACCADE_PRECISION_INT8;
    info.import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    info.queue_capacity = 1;
    info.max_in_flight = 1;
    info.device_alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    info.name = state->graphics_.software_device() ? literal_span("DirectML WARP CPU device")
                                                   : literal_span("DirectML D3D12 GPU device");
    return write_structure(output, info);
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::query_model(void* context, SaccadeSpanU8 bytes,
                                                                        SaccadeModelInfo* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    model::ArtifactView artifact{};
    SaccadeResult result = model::parse_artifact(bytes, &artifact);
    model::directml::Contract contract{};
    if (result == SACCADE_OK) {
        result = model::directml::parse_contract(artifact, &contract);
    }
    if (result != SACCADE_OK) return result;
    SaccadeModelInfo info{};
    info.stable_id = artifact.stable_id;
    info.required_host_bytes = DirectMlInferenceProvider::storage_size;
    info.required_device_bytes =
        static_cast<uint64_t>(artifact.input_width) * artifact.input_height * artifact.input_channels *
            (contract.input_kind == model::directml::InputKind::planar_fp16 ? 2U : 1U) +
        static_cast<uint64_t>(contract.candidate_capacity) *
            (model::directml::normalized_target_row_bytes + sizeof(kernels::targets::DenseCandidate));
    info.capability_bits = state->capability_bits_;
    info.max_output_bytes = artifact.max_output_bytes;
    info.name = literal_span("DirectML UI detector");
    return write_structure(output, info);
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::create_model(void* context,
                                                                         const SaccadeModelDesc* description,
                                                                         SaccadeModelHandle* output) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = 0;
    if (state->model_live_) return SACCADE_ERROR_BUSY;
    SaccadeModelDesc value{};
    SaccadeResult result = read_structure(description, &value);
    if (result != SACCADE_OK) return result;
    if (value.stable_id == 0 || value.device_id != state->device_id_ || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    model::ArtifactView artifact{};
    state->model_stage_ = DirectMlModelStage::artifact;
    result = model::parse_artifact(value.bytes, &artifact);
    model::directml::Contract contract{};
    if (result == SACCADE_OK) {
        result = model::directml::parse_contract(artifact, &contract);
    }
    if (result == SACCADE_OK && artifact.stable_id != value.stable_id) {
        result = SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (result == SACCADE_OK) {
        result = model::verify_artifact(artifact, state->verifier_);
    }
    if (result != SACCADE_OK || !copy_name(contract.input_name, &state->input_name_) ||
        !copy_name(contract.candidate_name, &state->candidate_name_)) {
        return result != SACCADE_OK ? result : SACCADE_ERROR_INVALID_ARGUMENT;
    }

    state->model_stage_ = DirectMlModelStage::candidate_buffer;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC candidate_desc{};
    candidate_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    candidate_desc.Width =
        static_cast<uint64_t>(contract.candidate_capacity) * model::directml::normalized_target_row_bytes;
    candidate_desc.Height = 1;
    candidate_desc.DepthOrArraySize = 1;
    candidate_desc.MipLevels = 1;
    candidate_desc.SampleDesc.Count = 1;
    candidate_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    candidate_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(state->graphics_.device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &candidate_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(state->candidate_buffer_.GetAddressOf())))) {
        return SACCADE_ERROR_BACKEND;
    }
    image::TensorSpec tensor_spec{};
    tensor_spec.width = artifact.input_width;
    tensor_spec.height = artifact.input_height;
    tensor_spec.format = contract.input_kind == model::directml::InputKind::planar_fp16
                             ? image::TensorFormat::planar_fp16
                             : image::TensorFormat::planar_int8;
    tensor_spec.channel_scale = contract.channel_scale;
    tensor_spec.channel_bias = contract.channel_bias;
    tensor_spec.letterbox_rgb = contract.letterbox_rgb;
    state->model_stage_ = DirectMlModelStage::preprocessor;
    result = state->preprocessor_.initialize(state->graphics_.device(), state->graphics_.queue(),
                                             state->shader_directory_.data(), tensor_spec);
    image::TensorView input_tensor{};
    if (result == SACCADE_OK) {
        state->model_stage_ = DirectMlModelStage::input_tensor;
        result = state->preprocessor_.tensor_storage(&input_tensor);
    }
    DirectMlBindingDesc input_binding{};
    input_binding.name = state->input_name_.data();
    input_binding.resource = static_cast<ID3D12Resource*>(input_tensor.buffer);
    input_binding.byte_size = input_tensor.byte_size;
    input_binding.shape = {1, 3, artifact.input_height, artifact.input_width};
    input_binding.rank = 4;
    input_binding.element_type =
        tensor_spec.format == image::TensorFormat::planar_fp16 ? TensorElementType::fp16 : TensorElementType::int8;
    DirectMlBindingDesc output_binding{};
    output_binding.name = state->candidate_name_.data();
    output_binding.resource = state->candidate_buffer_.Get();
    output_binding.byte_size = static_cast<size_t>(candidate_desc.Width);
    output_binding.shape = {contract.candidate_capacity, model::directml::target_row_components};
    output_binding.rank = 2;
    output_binding.element_type = TensorElementType::fp16;
    const DirectMlSessionDesc session{contract.graph, &input_binding, &output_binding, 1, 1};
    if (result == SACCADE_OK) {
        state->model_stage_ = DirectMlModelStage::inference;
        result = state->inference_.initialize(state->graphics_.device(), state->graphics_.queue(), session);
    }
    if (result == SACCADE_OK) {
        state->model_stage_ = DirectMlModelStage::postprocessor;
        const TargetPostprocessorSpec postprocess_spec{
            contract.candidate_capacity,     artifact.max_targets, state->candidate_buffer_.Get(), 0,
            CandidateInput::normalized_fp16, artifact.input_width, artifact.input_height,          0};
        result = state->postprocessor_.initialize(state->graphics_.device(), state->graphics_.queue(),
                                                  state->shader_directory_.data(), postprocess_spec);
    }
    if (result != SACCADE_OK) {
        state->reset_model_objects();
        return result;
    }
    state->model_stable_id_ = artifact.stable_id;
    state->candidate_capacity_ = contract.candidate_capacity;
    state->target_capacity_ = artifact.max_targets;
    state->minimum_confidence_q16_ = contract.minimum_confidence_q16;
    state->band_minimum_confidence_q16_ = contract.band_minimum_confidence_q16;
    state->band_min_short_side_q3_ = contract.band_min_short_side_q3;
    state->band_max_short_side_q3_ = contract.band_max_short_side_q3;
    state->iou_threshold_q16_ = contract.iou_threshold_q16;
    state->maximum_output_bytes_ = artifact.max_output_bytes;
    state->precision_bits_ = artifact.precision_bits;
    state->model_live_ = true;
    state->model_stage_ = DirectMlModelStage::ready;
    *output = make_handle(state->model_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::destroy_model(void* context, SaccadeModelHandle model) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!state->model_live_ || !decode_handle(model, state->model_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->context_live_ || state->ticket_.active_) return SACCADE_ERROR_BUSY;
    state->reset_model_objects();
    state->model_live_ = false;
    state->model_generation_ = next_generation(state->model_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::create_context(
    void* context, const SaccadeExecutionContextDesc* description, SaccadeExecutionContextHandle* output) {
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
    if (state->context_live_ || value.device_id != state->device_id_ || value.queue_capacity != 1 ||
        value.max_in_flight != 1 || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    (void)ResetEvent(state->worker_ready_event_);
    state->worker_ = std::thread([state]() noexcept { state->worker_loop(); });
    if (WaitForSingleObject(state->worker_ready_event_, 5'000) != WAIT_OBJECT_0 ||
        state->worker_start_result_ != SACCADE_OK) {
        state->stop_worker();
        return state->worker_start_result_ != SACCADE_OK ? state->worker_start_result_ : SACCADE_ERROR_TIMEOUT;
    }
    state->context_live_ = true;
    *output = make_handle(state->context_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL
DirectMlInferenceProvider::Impl::destroy_context(void* context, SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr || !state->initialized_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->ticket_.active_) return SACCADE_ERROR_BUSY;
    state->stop_worker();
    state->context_live_ = false;
    state->context_generation_ = next_generation(state->context_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::submit(void* context,
                                                                   SaccadeExecutionContextHandle execution_context,
                                                                   const SaccadeInferenceDispatchDesc* description,
                                                                   SaccadeTicketHandle* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = 0;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeInferenceDispatchDesc value{};
    const SaccadeResult read = read_structure(description, &value);
    if (read != SACCADE_OK) return read;
    if (!frame_valid(value) || value.output_capacity < state->maximum_output_bytes_) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state->ticket_.active_) return SACCADE_ERROR_BUSY;
    state->ticket_ = {};
    state->ticket_.dispatch_ = value;
    state->ticket_.active_ = true;
    state->cancel_requested_.store(false, std::memory_order_relaxed);
    (void)ResetEvent(state->completion_event_);
    state->control_state_.store(SACCADE_TICKET_QUEUED, std::memory_order_release);
    (void)SetEvent(state->command_event_);
    *output = make_handle(state->ticket_generation_);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::poll(void* context,
                                                                 SaccadeExecutionContextHandle execution_context,
                                                                 SaccadeTicketHandle handle,
                                                                 SaccadeInferenceStatus* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return write_structure(output,
                           status(handle, state->ticket_, state->control_state_.load(std::memory_order_acquire)));
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::wait(void* context,
                                                                 SaccadeExecutionContextHandle execution_context,
                                                                 SaccadeTicketHandle handle, uint64_t timeout_ns,
                                                                 SaccadeInferenceStatus* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    uint32_t ticket_state = state->control_state_.load(std::memory_order_acquire);
    if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) {
        const DWORD waited = WaitForSingleObject(state->completion_event_, timeout_milliseconds(timeout_ns));
        if (waited == WAIT_TIMEOUT) {
            const SaccadeResult wrote = write_structure(output, status(handle, state->ticket_, ticket_state));
            return wrote == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : wrote;
        }
        if (waited != WAIT_OBJECT_0) return SACCADE_ERROR_BACKEND;
        ticket_state = state->control_state_.load(std::memory_order_acquire);
    }
    return write_structure(output, status(handle, state->ticket_, ticket_state));
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::collect(void* context,
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
    const uint32_t ticket_state = state->control_state_.load(std::memory_order_acquire);
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

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::cancel(void* context,
                                                                   SaccadeExecutionContextHandle execution_context,
                                                                   SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_) ||
        !state->ticket_.active_ || !decode_handle(handle, state->ticket_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const uint32_t ticket_state = state->control_state_.load(std::memory_order_acquire);
    if (ticket_state != SACCADE_TICKET_QUEUED && ticket_state != SACCADE_TICKET_RUNNING) return SACCADE_ERROR_STATE;
    state->cancel_requested_.store(true, std::memory_order_release);
    if (WaitForSingleObject(state->completion_event_, INFINITE) != WAIT_OBJECT_0) {
        return SACCADE_ERROR_BACKEND;
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::reset(void* context,
                                                                  SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (state->ticket_.active_) {
        const uint32_t ticket_state = state->control_state_.load(std::memory_order_acquire);
        if (ticket_state == SACCADE_TICKET_QUEUED || ticket_state == SACCADE_TICKET_RUNNING) {
            state->cancel_requested_.store(true, std::memory_order_release);
            (void)WaitForSingleObject(state->completion_event_, INFINITE);
        }
        state->release_ticket();
        state->ticket_generation_ = next_generation(state->ticket_generation_);
    }
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::synchronize(void* context,
                                                                        SaccadeExecutionContextHandle execution_context,
                                                                        uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (!state->ticket_.active_) return SACCADE_OK;
    const uint32_t ticket_state = state->control_state_.load(std::memory_order_acquire);
    if (ticket_state != SACCADE_TICKET_QUEUED && ticket_state != SACCADE_TICKET_RUNNING) return SACCADE_OK;
    const DWORD waited = WaitForSingleObject(state->completion_event_, timeout_milliseconds(timeout_ns));
    if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_TIMEOUT;
    return waited == WAIT_OBJECT_0 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

SaccadeResult SACCADE_CALL DirectMlInferenceProvider::Impl::memory_stats(
    void* context, SaccadeExecutionContextHandle execution_context, SaccadeMemoryStats* output) {
    Impl* state = from(context);
    if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!state->context_live_ || !decode_handle(execution_context, state->context_generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeMemoryStats stats{};
    stats.host_committed = sizeof(Impl);
    stats.host_reserved = DirectMlInferenceProvider::storage_size;
    stats.device_imported = state->ticket_.active_ ? static_cast<uint64_t>(state->ticket_.dispatch_.frame.width) *
                                                         state->ticket_.dispatch_.frame.height * 4U
                                                   : 0;
    stats.device_owned = state->postprocessor_.stats().workspace_bytes + state->inference_.stats().input_bytes +
                         state->inference_.stats().output_bytes;
    stats.framework_opaque = state->inference_.stats().model_bytes;
    stats.copied_bytes = state->copied_bytes_;
    stats.high_water_bytes = stats.host_committed + stats.device_imported + stats.device_owned + stats.framework_opaque;
    return write_structure(output, stats);
}

SaccadeInferenceProviderDesc DirectMlInferenceProvider::descriptor() noexcept {
    SaccadeInferenceOps ops{};
    ops.struct_size = sizeof(ops);
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = detail::guarded_callback<&Impl::enumerate_devices>;
    ops.query_model = detail::guarded_callback<&Impl::query_model>;
    ops.create_model = detail::guarded_callback<&Impl::create_model>;
    ops.destroy_model = detail::guarded_callback<&Impl::destroy_model>;
    ops.create_context = detail::guarded_callback<&Impl::create_context>;
    ops.destroy_context = detail::guarded_callback<&Impl::destroy_context>;
    ops.submit = detail::guarded_callback<&Impl::submit>;
    ops.poll = detail::guarded_callback<&Impl::poll>;
    ops.wait = detail::guarded_callback<&Impl::wait>;
    ops.collect = detail::guarded_callback<&Impl::collect>;
    ops.cancel = detail::guarded_callback<&Impl::cancel>;
    ops.reset = detail::guarded_callback<&Impl::reset>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize>;
    ops.memory_stats = detail::guarded_callback<&Impl::memory_stats>;
    SaccadeProviderInfo info{};
    info.struct_size = sizeof(info);
    info.api_version = SACCADE_API_VERSION;
    info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    info.capability_bits = impl().capability_bits_;
    info.stable_id = provider_id;
    info.name = literal_span("DirectML inference");
    SaccadeInferenceProviderDesc description{};
    description.struct_size = sizeof(description);
    description.api_version = SACCADE_API_VERSION;
    description.info = info;
    description.context = this;
    description.ops = ops;
    return description;
}

} // namespace saccade::backend::d3d12

#include "application/inference_runtime.hpp"

namespace saccade::application {

InferenceRuntime::~InferenceRuntime() {
    (void)shutdown();
}

SaccadeResult InferenceRuntime::initialize(const InferenceRuntimeConfig& config) noexcept {
    if (runtime_ != 0) return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.provider.context == nullptr || config.provider.info.struct_size < sizeof(SaccadeProviderInfo) ||
        config.provider.info.api_version != SACCADE_API_VERSION || config.artifact.data == nullptr ||
        config.artifact.size == 0 || config.required_capability_bits == 0 || config.required_format_bits == 0 ||
        config.required_precision_bits == 0 || config.required_import_bits == 0 || config.queue_capacity == 0 ||
        config.max_in_flight == 0 || config.max_in_flight > config.queue_capacity) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeResult result = saccade_runtime_create(&runtime_desc, &runtime_);
    if (result == SACCADE_OK) {
        result = saccade_register_inference_provider(runtime_, &config.provider);
    }
    if (result == SACCADE_OK) result = saccade_runtime_freeze(runtime_);
    if (result == SACCADE_OK) {
        SaccadeInferenceSessionDesc session_desc{};
        session_desc.struct_size = sizeof(session_desc);
        session_desc.api_version = SACCADE_API_VERSION;
        session_desc.model_bytes = config.artifact;
        session_desc.model_stable_id = config.model_stable_id;
        session_desc.provider_stable_id = config.provider_stable_id;
        session_desc.device_stable_id = config.device_stable_id;
        session_desc.required_capability_bits = config.required_capability_bits;
        session_desc.preferred_capability_bits = config.preferred_capability_bits;
        session_desc.required_format_bits = config.required_format_bits;
        session_desc.required_precision_bits = config.required_precision_bits;
        session_desc.required_import_bits = config.required_import_bits;
        session_desc.queue_capacity = config.queue_capacity;
        session_desc.max_in_flight = config.max_in_flight;
        info_.struct_size = sizeof(info_);
        info_.api_version = SACCADE_API_VERSION;
        result = saccade_inference_session_create(runtime_, &session_desc, &session_, &info_);
    }
    if (result != SACCADE_OK) {
        ++stats_.failures;
        if (session_ != 0) {
            (void)saccade_inference_session_destroy(runtime_, session_);
            session_ = 0;
        }
        if (runtime_ != 0) {
            (void)saccade_runtime_destroy(runtime_);
            runtime_ = 0;
        }
        info_ = {};
        return result;
    }
    ++stats_.initializations;
    return SACCADE_OK;
}

SaccadeResult InferenceRuntime::shutdown() noexcept {
    if (runtime_ == 0) return SACCADE_OK;
    if (session_ != 0) {
        const SaccadeResult destroyed = saccade_inference_session_destroy(runtime_, session_);
        if (destroyed != SACCADE_OK) {
            ++stats_.failures;
            return destroyed;
        }
        session_ = 0;
    }
    const SaccadeResult destroyed = saccade_runtime_destroy(runtime_);
    if (destroyed != SACCADE_OK) {
        ++stats_.failures;
        return destroyed;
    }
    runtime_ = 0;
    info_ = {};
    ++stats_.shutdowns;
    return SACCADE_OK;
}

} // namespace saccade::application

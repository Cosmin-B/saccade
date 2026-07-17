#include "application/inference_runtime.hpp"
#include "apps/model_trust.hpp"
#include "core/stack_string_builder.hpp"
#include "model/mapped_artifact.hpp"
#include "model/p256_verifier.hpp"

#if defined(__APPLE__)
#include "platform/macos/coreml_provider.hpp"
#elif defined(_WIN32)
#include "backends/d3d12/directml_provider.hpp"
#endif

#include <cstdio>
#include <string_view>

namespace {

enum class ExitCode : int {
    success,
    usage,
    trust_not_configured,
    verifier_failed,
    artifact_failed,
    provider_failed,
    runtime_failed,
    memory_failed,
    output_failed,
    shutdown_failed
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

std::string_view text(SaccadeSpanU8 value) noexcept {
    return {reinterpret_cast<const char*>(value.data), value.size};
}

int fail(ExitCode code, std::string_view stage, SaccadeResult result) noexcept {
    saccade::core::StackStringBuilder<512> output;
    const SaccadeSpanU8 detail = saccade_last_error();
    (void)output.append(stage);
    (void)output.append(" failed (");
    (void)output.append_signed(result);
    (void)output.append(')');
    if (detail.size != 0) {
        (void)output.append(": ");
        (void)output.append(text(detail));
    }
    (void)output.append('\n');
    (void)std::fwrite(output.view().data(), 1, output.view().size(), stderr);
    return exit_code(code);
}

bool append_result(saccade::core::StackStringBuilder<2048>* output, const saccade::model::ArtifactView& artifact,
                   const SaccadeInferenceProviderDesc& provider, const SaccadeInferenceSessionInfo& session,
                   const SaccadeMemoryStats& memory) noexcept {
    return output->append("{\n  \"provider\": \"") && output->append(text(provider.info.name)) &&
           output->append("\",\n  \"provider_stable_id\": ") && output->append_unsigned(session.provider_stable_id) &&
           output->append(",\n  \"device_stable_id\": ") && output->append_unsigned(session.device_stable_id) &&
           output->append(",\n  \"model_stable_id\": ") && output->append_unsigned(session.model_stable_id) &&
           output->append(",\n  \"input_width\": ") && output->append_unsigned(artifact.input_width) &&
           output->append(",\n  \"input_height\": ") && output->append_unsigned(artifact.input_height) &&
           output->append(",\n  \"precision_bits\": ") && output->append_unsigned(session.precision_bits) &&
           output->append(",\n  \"capability_bits\": ") && output->append_unsigned(session.capability_bits) &&
           output->append(",\n  \"import_bits\": ") && output->append_unsigned(session.import_bits) &&
           output->append(",\n  \"maximum_output_bytes\": ") && output->append_unsigned(session.max_output_bytes) &&
           output->append(",\n  \"host_committed\": ") && output->append_unsigned(memory.host_committed) &&
           output->append(",\n  \"device_owned\": ") && output->append_unsigned(memory.device_owned) &&
           output->append(",\n  \"framework_opaque\": ") && output->append_unsigned(memory.framework_opaque) &&
           output->append("\n}\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return exit_code(ExitCode::usage);
    if (!saccade::apps::model_trust::configured) return exit_code(ExitCode::trust_not_configured);

    static saccade::model::P256ArtifactVerifier verifier;
    SaccadeResult result = verifier.initialize(saccade::apps::model_trust::public_key);
    if (result != SACCADE_OK) return fail(ExitCode::verifier_failed, "verifier initialization", result);

    static saccade::model::MappedArtifact artifact;
    result = artifact.initialize(argv[1], verifier.descriptor());
    if (result != SACCADE_OK) return fail(ExitCode::artifact_failed, "artifact admission", result);

#if defined(__APPLE__)
    static saccade::platform::macos::CoreMlInferenceProvider native_provider;
    result = native_provider.initialize({argv[2],
                                         saccade::platform::macos::CoreMlComputePolicy::cpu_and_neural_engine,
                                         false,
                                         {},
                                         verifier.descriptor()});
    if (result != SACCADE_OK) return fail(ExitCode::provider_failed, "Core ML provider initialization", result);
    constexpr uint32_t import_bits = SACCADE_IMPORT_IOSURFACE;
#elif defined(_WIN32)
    static saccade::backend::d3d12::DirectMlInferenceProvider native_provider;
    result = native_provider.initialize(
        {argv[2], verifier.descriptor(), saccade::backend::d3d12::DirectMlExecutionPolicy::hardware_then_software, 0});
    if (result != SACCADE_OK) return fail(ExitCode::provider_failed, "DirectML provider initialization", result);
    constexpr uint32_t import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
#endif

    const SaccadeInferenceProviderDesc provider = native_provider.descriptor();
    saccade::application::InferenceRuntimeConfig config{};
    config.provider = provider;
    config.artifact = artifact.bytes();
    config.model_stable_id = artifact.view().stable_id;
    config.provider_stable_id = provider.info.stable_id;
    config.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC;
    config.preferred_capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_ACCELERATOR |
                                       SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    config.required_format_bits = SACCADE_FORMAT_BGRA8;
    config.required_precision_bits = artifact.view().precision_bits;
    config.required_import_bits = import_bits;

    static saccade::application::InferenceRuntime runtime;
    result = runtime.initialize(config);
    if (result != SACCADE_OK) return fail(ExitCode::runtime_failed, "runtime session initialization", result);

    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    result = provider.ops.memory_stats(provider.context, runtime.session(), &memory);
    if (result != SACCADE_OK) return fail(ExitCode::memory_failed, "memory query", result);

    saccade::core::StackStringBuilder<2048> output;
    if (!append_result(&output, artifact.view(), provider, runtime.info(), memory) || output.truncated() ||
        std::fwrite(output.view().data(), 1, output.view().size(), stdout) != output.view().size()) {
        return exit_code(ExitCode::output_failed);
    }

    if (runtime.shutdown() != SACCADE_OK || native_provider.shutdown() != SACCADE_OK ||
        artifact.shutdown() != SACCADE_OK || verifier.shutdown() != SACCADE_OK) {
        return exit_code(ExitCode::shutdown_failed);
    }
    return exit_code(ExitCode::success);
}

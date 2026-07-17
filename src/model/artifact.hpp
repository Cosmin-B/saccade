#ifndef SACCADE_MODEL_ARTIFACT_HPP
#define SACCADE_MODEL_ARTIFACT_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_scene.h>

#include <cstdint>

namespace saccade::model {

constexpr uint32_t artifact_version = 1;
constexpr uint32_t artifact_header_bytes = 96;
constexpr uint32_t artifact_signature_bytes = 64;

enum class GraphKind : uint32_t { ui_detector = 1 };

enum class ArtifactKind : uint32_t { fixed_graph = 1, coreml_compiled_bundle = 2, onnx = 3 };

enum : uint32_t { artifact_has_signature = UINT32_C(1) << 0, artifact_relative_locator = UINT32_C(1) << 1 };

struct ArtifactView {
    uint64_t stable_id = 0;
    GraphKind graph = GraphKind::ui_detector;
    ArtifactKind artifact = ArtifactKind::fixed_graph;
    uint32_t precision_bits = 0;
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t input_channels = 0;
    uint32_t max_targets = 0;
    uint32_t max_output_bytes = 0;
    uint32_t flags = 0;
    uint64_t provider_compatibility_bits = 0;
    SaccadeSpanU8 payload{};
    SaccadeSpanU8 signed_message{};
    SaccadeSpanU8 signature{};
};

using VerifyArtifactFn = SaccadeResult (*)(void*, const ArtifactView&) noexcept;

struct ArtifactVerifier {
    void* context = nullptr;
    VerifyArtifactFn verify = nullptr;
};

SaccadeResult parse_artifact(SaccadeSpanU8, ArtifactView*) noexcept;
SaccadeResult verify_artifact(const ArtifactView&, ArtifactVerifier) noexcept;

} // namespace saccade::model

#endif

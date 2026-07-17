#include "model/artifact.hpp"
#include "model/mapped_artifact.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

enum class TestResult : int {
    success,
    parse_failed,
    verification_failed,
    unsigned_rejection_failed,
    malformed_rejection_failed,
    mapping_failed
};

constexpr size_t version_offset = 4;
constexpr size_t header_size_offset = 8;
constexpr size_t total_size_offset = 12;
constexpr size_t stable_id_offset = 16;
constexpr size_t graph_offset = 24;
constexpr size_t artifact_offset = 28;
constexpr size_t precision_offset = 32;
constexpr size_t width_offset = 36;
constexpr size_t height_offset = 40;
constexpr size_t channels_offset = 44;
constexpr size_t maximum_targets_offset = 48;
constexpr size_t maximum_output_offset = 52;
constexpr size_t payload_offset_offset = 56;
constexpr size_t payload_size_offset = 64;
constexpr size_t signature_offset_offset = 72;
constexpr size_t signature_size_offset = 80;
constexpr size_t flags_offset = 84;
constexpr size_t compatibility_offset = 88;
constexpr uint64_t stable_id = 101;
constexpr uint64_t provider_compatibility = 102;
constexpr uint32_t input_width = 320;
constexpr uint32_t input_height = 320;
constexpr uint32_t input_channels = 3;
constexpr uint32_t maximum_targets = 64;
constexpr uint32_t precision_bits = UINT32_C(1) << 1;
constexpr std::array<uint8_t, 4> payload{{1, 2, 3, 4}};
constexpr size_t payload_offset = saccade::model::artifact_header_bytes;
constexpr size_t signature_offset = payload_offset + payload.size();
constexpr size_t artifact_size = signature_offset + saccade::model::artifact_signature_bytes;
constexpr uint32_t maximum_output_bytes =
    sizeof(SaccadeTargetPacketHeader) + maximum_targets * sizeof(SaccadeTargetRecord);

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void write_u32(uint8_t* data, size_t offset, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index)
        data[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void write_u64(uint8_t* data, size_t offset, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index)
        data[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

std::array<uint8_t, artifact_size> artifact() noexcept {
    std::array<uint8_t, artifact_size> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = 'D';
    write_u32(bytes.data(), version_offset, saccade::model::artifact_version);
    write_u32(bytes.data(), header_size_offset, saccade::model::artifact_header_bytes);
    write_u32(bytes.data(), total_size_offset, static_cast<uint32_t>(bytes.size()));
    write_u64(bytes.data(), stable_id_offset, stable_id);
    write_u32(bytes.data(), graph_offset, static_cast<uint32_t>(saccade::model::GraphKind::ui_detector));
    write_u32(bytes.data(), artifact_offset, static_cast<uint32_t>(saccade::model::ArtifactKind::fixed_graph));
    write_u32(bytes.data(), precision_offset, precision_bits);
    write_u32(bytes.data(), width_offset, input_width);
    write_u32(bytes.data(), height_offset, input_height);
    write_u32(bytes.data(), channels_offset, input_channels);
    write_u32(bytes.data(), maximum_targets_offset, maximum_targets);
    write_u32(bytes.data(), maximum_output_offset, maximum_output_bytes);
    write_u64(bytes.data(), payload_offset_offset, payload_offset);
    write_u64(bytes.data(), payload_size_offset, payload.size());
    write_u64(bytes.data(), signature_offset_offset, signature_offset);
    write_u32(bytes.data(), signature_size_offset, saccade::model::artifact_signature_bytes);
    write_u32(bytes.data(), flags_offset, saccade::model::artifact_has_signature);
    write_u64(bytes.data(), compatibility_offset, provider_compatibility);
    for (size_t index = 0; index < payload.size(); ++index)
        bytes[payload_offset + index] = payload[index];
    return bytes;
}

struct VerificationCapture {
    uint32_t calls = 0;
};

SaccadeResult verify(void* context, const saccade::model::ArtifactView& view) noexcept {
    auto* capture = static_cast<VerificationCapture*>(context);
    ++capture->calls;
    return view.stable_id == stable_id && view.payload.size == payload.size() && view.signed_message.data != nullptr &&
                   view.signed_message.size == signature_offset &&
                   view.signature.size == saccade::model::artifact_signature_bytes
               ? SACCADE_OK
               : SACCADE_ERROR_INVALID_ARGUMENT;
}

} // namespace

int main() {
    auto bytes = artifact();
    saccade::model::ArtifactView view{};
    if (saccade::model::parse_artifact({bytes.data(), bytes.size()}, &view) != SACCADE_OK ||
        view.stable_id != stable_id || view.input_width != input_width || view.input_height != input_height ||
        view.max_targets != maximum_targets || view.payload.data != bytes.data() + payload_offset)
        return result(TestResult::parse_failed);
    VerificationCapture capture{};
    if (saccade::model::verify_artifact(view, {&capture, verify}) != SACCADE_OK || capture.calls != 1)
        return result(TestResult::verification_failed);
    write_u32(bytes.data(), flags_offset, 0);
    write_u64(bytes.data(), signature_offset_offset, 0);
    write_u32(bytes.data(), signature_size_offset, 0);
    write_u32(bytes.data(), total_size_offset, static_cast<uint32_t>(signature_offset));
    if (saccade::model::parse_artifact({bytes.data(), signature_offset}, &view) != SACCADE_OK ||
        saccade::model::verify_artifact(view, {&capture, verify}) != SACCADE_ERROR_PERMISSION)
        return result(TestResult::unsigned_rejection_failed);
    bytes = artifact();
    write_u64(bytes.data(), signature_offset_offset, payload_offset);
    if (saccade::model::parse_artifact({bytes.data(), bytes.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::malformed_rejection_failed);
    bytes = artifact();
    constexpr char mapped_path[] = "saccade-mapped-artifact-test.bin";
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, mapped_path, "wb") != 0) return result(TestResult::mapping_failed);
#else
    file = std::fopen(mapped_path, "wb");
#endif
    if (file == nullptr || std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() || std::fclose(file) != 0)
        return result(TestResult::mapping_failed);
    saccade::model::MappedArtifact mapped;
    if (mapped.initialize(mapped_path, {&capture, verify}) != SACCADE_OK || mapped.bytes().size != bytes.size() ||
        mapped.view().stable_id != stable_id || capture.calls != 2 || mapped.shutdown() != SACCADE_OK ||
        std::remove(mapped_path) != 0)
        return result(TestResult::mapping_failed);
    return result(TestResult::success);
}

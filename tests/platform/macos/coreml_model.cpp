#include "platform/macos/coreml_model.hpp"

#include "model/coreml_contract.hpp"
#include "scene/packet.hpp"

#include <CoreVideo/CoreVideo.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

enum class TestResult : int {
    success,
    invalid_admission_failed,
    bundle_fixture_failed,
    bundle_digest_contract_failed,
    bundle_mismatch_not_rejected,
    bundle_digest_failed,
    bundle_tamper_not_rejected,
    bundle_symlink_not_rejected,
    pixel_buffer_failed,
    model_load_failed,
    prediction_failed,
    packet_failed,
    shutdown_failed
};

constexpr uint64_t stable_id = 701;
constexpr uint32_t input_width = 320;
constexpr uint32_t input_height = 320;
constexpr uint32_t maximum_targets = 8;
constexpr uint32_t candidate_capacity = 16;
constexpr uint16_t minimum_confidence_q16 = 4096;
constexpr uint16_t iou_threshold_q16 = 32768;
constexpr uint64_t frame_id = 702;
constexpr uint64_t model_epoch = 703;
constexpr uint64_t session_epoch = 704;
constexpr uint64_t transform_epoch = 705;
constexpr uint64_t topology_epoch = 706;
constexpr uint64_t source_id = 707;
constexpr std::array<uint8_t, 31> locator{'c', 'o', 'r', 'e', 'm', 'l', '-', 'c', 'o', 'n', 't',
                                          'r', 'a', 'c', 't', '-', 'f', 'i', 'x', 't', 'u', 'r',
                                          'e', '.', 'm', 'l', 'm', 'o', 'd', 'e', 'l'};
constexpr std::array<uint8_t, 1> locator_suffix{'c'};
constexpr std::array<uint8_t, 5> input_name{'i', 'm', 'a', 'g', 'e'};
constexpr std::array<uint8_t, 7> rows_name{'t', 'a', 'r', 'g', 'e', 't', 's'};
constexpr std::array<uint8_t, 5> count_name{'c', 'o', 'u', 'n', 't'};
constexpr std::array<uint8_t, 32> bundle_sha256{0x99, 0x69, 0x82, 0x75, 0xa9, 0xfa, 0xe7, 0x70, 0x68, 0x75, 0x5f,
                                                0x2d, 0x46, 0x87, 0xee, 0x71, 0x2f, 0x55, 0x0f, 0xcf, 0xde, 0xe3,
                                                0x01, 0xae, 0xef, 0xbb, 0x33, 0xb9, 0x55, 0x74, 0xd4, 0x1e};
constexpr std::array<uint8_t, 32> compiled_fixture_sha256{
    0x35, 0xac, 0x63, 0x1f, 0xd2, 0x0e, 0x68, 0x76, 0xea, 0x29, 0x07, 0x4b, 0xd6, 0x4e, 0x1b, 0xab,
    0x67, 0xbc, 0x68, 0x48, 0x97, 0x91, 0x40, 0x88, 0x34, 0xc1, 0xd8, 0x7e, 0xc2, 0x90, 0x3a, 0x07};
constexpr size_t payload_size = saccade::model::coreml::payload_header_bytes + locator.size() + locator_suffix.size() +
                                input_name.size() + rows_name.size() + count_name.size();
constexpr size_t output_size = sizeof(SaccadeTargetPacketHeader) + maximum_targets * sizeof(SaccadeTargetRecord);

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void write_u32(uint8_t* bytes, size_t offset, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

struct ArtifactFixture {
    std::array<uint8_t, payload_size> payload{};

    explicit ArtifactFixture(const std::array<uint8_t, 32>& digest = {}) noexcept {
        payload[0] = 'S';
        payload[1] = 'C';
        payload[2] = 'M';
        payload[3] = 'C';
        write_u32(payload.data(), 4, saccade::model::coreml::contract_version);
        write_u32(payload.data(), 8, static_cast<uint32_t>(saccade::model::coreml::InputKind::image_bgra8));
        write_u32(payload.data(), 12,
                  static_cast<uint32_t>(saccade::model::coreml::OutputLayout::normalized_target_rows_v1));
        write_u32(payload.data(), 16, candidate_capacity);
        write_u32(payload.data(), 20, maximum_targets);
        write_u32(payload.data(), 24, minimum_confidence_q16);
        write_u32(payload.data(), 28, iou_threshold_q16);
        write_u32(payload.data(), 32, static_cast<uint32_t>(locator.size() + locator_suffix.size()));
        write_u32(payload.data(), 36, static_cast<uint32_t>(input_name.size()));
        write_u32(payload.data(), 40, static_cast<uint32_t>(rows_name.size()));
        write_u32(payload.data(), 44, static_cast<uint32_t>(count_name.size()));
        std::memcpy(payload.data() + 48, digest.data(), digest.size());
        uint8_t* strings = payload.data() + saccade::model::coreml::payload_header_bytes;
        std::memcpy(strings, locator.data(), locator.size());
        strings += locator.size();
        std::memcpy(strings, locator_suffix.data(), locator_suffix.size());
        strings += locator_suffix.size();
        std::memcpy(strings, input_name.data(), input_name.size());
        strings += input_name.size();
        std::memcpy(strings, rows_name.data(), rows_name.size());
        strings += rows_name.size();
        std::memcpy(strings, count_name.data(), count_name.size());
    }

    [[nodiscard]] saccade::model::ArtifactView view() const noexcept {
        return {stable_id,
                saccade::model::GraphKind::ui_detector,
                saccade::model::ArtifactKind::coreml_compiled_bundle,
                SACCADE_PRECISION_FP16,
                input_width,
                input_height,
                3,
                maximum_targets,
                static_cast<uint32_t>(output_size),
                saccade::model::artifact_relative_locator,
                saccade::model::coreml::provider_compatibility_bit,
                {payload.data(), payload.size()},
                {}};
    }
};

struct TemporaryBundle {
    std::string root_{};

    [[nodiscard]] std::filesystem::path path() const {
        return std::filesystem::path(root_) / "coreml-contract-fixture.mlmodelc";
    }

    bool create() {
        std::array<char, 40> pattern{};
        constexpr char root_pattern[] = "/tmp/saccade-coreml-model-XXXXXX";
        std::memcpy(pattern.data(), root_pattern, sizeof(root_pattern));
        char* root = mkdtemp(pattern.data());
        if (root == nullptr) return false;
        root_ = root;

        std::error_code error;
        const std::filesystem::path bundle = path();
        if (!std::filesystem::create_directory(bundle, error) || error) return false;
        std::ofstream model(bundle / "model.bin", std::ios::binary);
        constexpr char contents[] = "model";
        model.write(contents, sizeof(contents) - 1U);
        return model.good();
    }

    bool tamper() const {
        std::ofstream model(path() / "model.bin", std::ios::binary | std::ios::app);
        model.put('!');
        return model.good();
    }

    bool add_symlink() const {
        std::error_code error;
        std::filesystem::create_symlink("model.bin", path() / "alias.bin", error);
        return !error;
    }

    ~TemporaryBundle() {
        std::error_code ignored;
        if (!root_.empty()) std::filesystem::remove_all(root_, ignored);
    }
};

} // namespace

int main(int argc, char** argv) {
    saccade::platform::macos::CoreMlModel model;
    if (model.initialize({}, {}) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return result(TestResult::invalid_admission_failed);
    }

    TemporaryBundle bundle;
    if (!bundle.create()) return result(TestResult::bundle_fixture_failed);
    std::array<uint8_t, 32> digest{};
    const std::string bundle_path = bundle.path().string();
    if (saccade::platform::macos::coreml_bundle_digest(bundle_path.c_str(), &digest) != SACCADE_OK ||
        digest != bundle_sha256) {
        return result(TestResult::bundle_digest_contract_failed);
    }
    const saccade::platform::macos::CoreMlModelConfig bundle_config{
        bundle.root_.c_str(), saccade::platform::macos::CoreMlComputePolicy::cpu_only, false, {}};
    ArtifactFixture mismatch_fixture{};
    if (model.initialize(mismatch_fixture.view(), bundle_config) != SACCADE_ERROR_PERMISSION) {
        return result(TestResult::bundle_mismatch_not_rejected);
    }
    ArtifactFixture matching_fixture{bundle_sha256};
    if (model.initialize(matching_fixture.view(), bundle_config) != SACCADE_ERROR_BACKEND) {
        return result(TestResult::bundle_digest_failed);
    }
    if (!bundle.tamper() || model.initialize(matching_fixture.view(), bundle_config) != SACCADE_ERROR_PERMISSION) {
        return result(TestResult::bundle_tamper_not_rejected);
    }
    if (!bundle.add_symlink() ||
        saccade::platform::macos::coreml_bundle_digest(bundle_path.c_str(), &digest) != SACCADE_ERROR_PERMISSION) {
        return result(TestResult::bundle_symlink_not_rejected);
    }

    if (argc == 1) return result(TestResult::success);

    ArtifactFixture fixture{compiled_fixture_sha256};
    const saccade::platform::macos::CoreMlModelConfig config{
        argv[1], saccade::platform::macos::CoreMlComputePolicy::all, false, {}};
    if (model.initialize(fixture.view(), config) != SACCADE_OK || model.stable_id() != stable_id ||
        model.maximum_output_bytes() != output_size) {
        return result(TestResult::model_load_failed);
    }
    CVPixelBufferRef pixel_buffer = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, input_width, input_height, kCVPixelFormatType_32BGRA, nullptr,
                            &pixel_buffer) != kCVReturnSuccess ||
        pixel_buffer == nullptr) {
        return result(TestResult::pixel_buffer_failed);
    }
    alignas(SaccadeTargetPacketHeader) std::array<uint8_t, output_size> output{};
    saccade::platform::macos::CoreMlPredictionResult prediction{};
    const saccade::platform::macos::CoreMlPrediction request{
        pixel_buffer,
        input_width,
        input_height,
        SACCADE_FORMAT_BGRA8,
        {0, 0, static_cast<int32_t>(input_width), static_cast<int32_t>(input_height)},
        {frame_id, model_epoch, session_epoch, transform_epoch, topology_epoch, source_id}};
    const SaccadeResult predicted = model.predict(request, {output.data(), output.size()}, &prediction);
    CVPixelBufferRelease(pixel_buffer);
    if (predicted != SACCADE_OK || prediction.target_count != 2 || prediction.candidate_count != 2) {
        return result(TestResult::prediction_failed);
    }
    saccade::scene::PacketView packet{};
    if (saccade::scene::validate_packet({output.data(), prediction.byte_size}, &packet) != SACCADE_OK ||
        packet.header->coordinate_space != SACCADE_COORDINATE_SPACE_SOURCE_Q8 || packet.targets[0].x_q8 != 10240 ||
        packet.targets[0].y_q8 != 20480 || packet.targets[0].width_q8 != 40960 ||
        packet.targets[0].height_q8 != 20480 ||
        packet.targets[0].capability_bits != (SACCADE_TARGET_CAPABILITY_POINTER_MOVE |
                                              SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_INVOKE) ||
        packet.targets[1].capability_bits != (SACCADE_TARGET_CAPABILITY_POINTER_MOVE |
                                              SACCADE_TARGET_CAPABILITY_BUTTON | SACCADE_TARGET_CAPABILITY_TEXT)) {
        return result(TestResult::packet_failed);
    }
    return model.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

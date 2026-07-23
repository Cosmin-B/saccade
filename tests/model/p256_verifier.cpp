#include "model/artifact.hpp"
#include "model/p256_verifier.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    initialize_failed,
    parse_failed,
    verification_failed,
    message_tamper_failed,
    signature_tamper_failed,
    shutdown_failed
};

constexpr size_t payload_offset = 96;
constexpr size_t payload_size = 4;
constexpr size_t signature_offset = payload_offset + payload_size;
constexpr size_t artifact_size = signature_offset + 64;

constexpr std::array<uint8_t, 64> public_key{
    0xbe, 0xbb, 0x50, 0xee, 0xd3, 0x6f, 0xae, 0x5c, 0x05, 0x88, 0x98, 0xf2, 0xac, 0x94, 0xfc, 0x95,
    0x92, 0xc0, 0xf2, 0xd9, 0x1a, 0x7e, 0x1b, 0xbd, 0x8d, 0x22, 0xad, 0xbc, 0xa8, 0xa8, 0xab, 0x11,
    0x1a, 0x3c, 0x9e, 0x8c, 0x54, 0xb5, 0x7e, 0x7c, 0xd6, 0xc6, 0x9c, 0x4d, 0xa7, 0x71, 0x4e, 0x95,
    0xed, 0x9f, 0xeb, 0xa8, 0x39, 0x07, 0x2c, 0xb0, 0xc9, 0x44, 0xb2, 0xaa, 0x68, 0x36, 0xda, 0x55};

constexpr std::array<uint8_t, 64> signature{
    0x6d, 0x9c, 0xba, 0x11, 0x0a, 0x3c, 0xaa, 0xf5, 0x1e, 0xf8, 0x86, 0x05, 0x05, 0x74, 0xb5, 0x6e,
    0x92, 0xf9, 0x4a, 0x6a, 0x98, 0xd8, 0x99, 0xff, 0x0c, 0xce, 0xae, 0x85, 0xe6, 0x8f, 0x95, 0xa0,
    0xcf, 0xa5, 0x81, 0x1c, 0x83, 0xe1, 0xfb, 0xb4, 0x74, 0x84, 0x2a, 0x2c, 0xdf, 0x77, 0x1e, 0x51,
    0x3d, 0x3e, 0x08, 0x93, 0xbd, 0x43, 0xe3, 0xd0, 0x9d, 0x0e, 0xa0, 0x8b, 0x0a, 0x17, 0xf5, 0x93};

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
    static_assert(sizeof(SaccadeTargetPacketHeader) == 104);
    static_assert(sizeof(SaccadeTargetRecord) == 80);
    std::array<uint8_t, artifact_size> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = 'D';
    write_u32(bytes.data(), 4, 1);
    write_u32(bytes.data(), 8, 96);
    write_u32(bytes.data(), 12, artifact_size);
    write_u64(bytes.data(), 16, UINT64_C(0x123456789abcdef0));
    write_u32(bytes.data(), 24, 1);
    write_u32(bytes.data(), 28, 1);
    write_u32(bytes.data(), 32, SACCADE_PRECISION_FP16);
    write_u32(bytes.data(), 36, 320);
    write_u32(bytes.data(), 40, 320);
    write_u32(bytes.data(), 44, 3);
    write_u32(bytes.data(), 48, 64);
    write_u32(bytes.data(), 52, 128 + 64 * 80);
    write_u64(bytes.data(), 56, payload_offset);
    write_u64(bytes.data(), 64, payload_size);
    write_u64(bytes.data(), 72, signature_offset);
    write_u32(bytes.data(), 80, 64);
    write_u32(bytes.data(), 84, saccade::model::artifact_has_signature);
    write_u64(bytes.data(), 88, 1);
    bytes[payload_offset + 0] = 'P';
    bytes[payload_offset + 1] = '2';
    bytes[payload_offset + 2] = '5';
    bytes[payload_offset + 3] = '6';
    std::memcpy(bytes.data() + signature_offset, signature.data(), signature.size());
    return bytes;
}

} // namespace

int main() {
    saccade::model::P256ArtifactVerifier verifier;
    saccade::model::P256PublicKey key{};
    key.xy = public_key;
    if (verifier.initialize(key) != SACCADE_OK) return result(TestResult::initialize_failed);
    auto bytes = artifact();
    saccade::model::ArtifactView view{};
    if (saccade::model::parse_artifact({bytes.data(), bytes.size()}, &view) != SACCADE_OK)
        return result(TestResult::parse_failed);
    if (saccade::model::verify_artifact(view, verifier.descriptor()) != SACCADE_OK)
        return result(TestResult::verification_failed);
    bytes[payload_offset] ^= 1U;
    if (saccade::model::verify_artifact(view, verifier.descriptor()) == SACCADE_OK)
        return result(TestResult::message_tamper_failed);
    bytes[payload_offset] ^= 1U;
    bytes[signature_offset] ^= 1U;
    if (saccade::model::verify_artifact(view, verifier.descriptor()) == SACCADE_OK)
        return result(TestResult::signature_tamper_failed);
    return verifier.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
}

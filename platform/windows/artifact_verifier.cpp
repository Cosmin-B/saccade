#include "model/p256_verifier.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace saccade::model {
namespace {

bool success(NTSTATUS status) noexcept {
    return status >= 0;
}

bool key_valid(const P256PublicKey& key) noexcept {
    uint8_t combined = 0;
    for (uint8_t byte : key.xy)
        combined |= byte;
    return combined != 0;
}

} // namespace

struct P256ArtifactVerifier::Impl {
    struct PublicBlob {
        BCRYPT_ECCKEY_BLOB header_{};
        std::array<uint8_t, 64> xy_{};
    };

    static SaccadeResult verify(void* context, const ArtifactView& artifact) noexcept {
        auto* owner = static_cast<P256ArtifactVerifier*>(context);
        if (owner == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        Impl& self = owner->impl();
        if (!self.initialized_ || artifact.signed_message.data == nullptr || artifact.signed_message.size == 0 ||
            artifact.signed_message.size > UINT32_MAX || artifact.signature.data == nullptr ||
            artifact.signature.size != artifact_signature_bytes) {
            return SACCADE_ERROR_PERMISSION;
        }
        std::array<uint8_t, 32> digest{};
        NTSTATUS status = BCryptHash(self.sha256_, nullptr, 0, const_cast<PUCHAR>(artifact.signed_message.data),
                                     static_cast<ULONG>(artifact.signed_message.size), digest.data(),
                                     static_cast<ULONG>(digest.size()));
        if (!success(status)) return SACCADE_ERROR_BACKEND;
        status = BCryptVerifySignature(self.key_, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                                       const_cast<PUCHAR>(artifact.signature.data),
                                       static_cast<ULONG>(artifact.signature.size), 0);
        return success(status) ? SACCADE_OK : SACCADE_ERROR_PERMISSION;
    }

    BCRYPT_ALG_HANDLE ecdsa_ = nullptr;
    BCRYPT_ALG_HANDLE sha256_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
    bool initialized_ = false;
};

P256ArtifactVerifier::P256ArtifactVerifier() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    ::new (static_cast<void*>(storage_.data())) Impl{};
}

P256ArtifactVerifier::~P256ArtifactVerifier() {
    (void)shutdown();
    impl().~Impl();
}

P256ArtifactVerifier::Impl& P256ArtifactVerifier::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const P256ArtifactVerifier::Impl& P256ArtifactVerifier::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult P256ArtifactVerifier::initialize(const P256PublicKey& public_key) noexcept {
    Impl& self = impl();
    if (self.initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (!key_valid(public_key)) return SACCADE_ERROR_INVALID_ARGUMENT;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&self.ecdsa_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    if (success(status)) {
        status = BCryptOpenAlgorithmProvider(&self.sha256_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    }
    Impl::PublicBlob blob{};
    blob.header_.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header_.cbKey = 32;
    blob.xy_ = public_key.xy;
    if (success(status)) {
        status = BCryptImportKeyPair(self.ecdsa_, nullptr, BCRYPT_ECCPUBLIC_BLOB, &self.key_,
                                     reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    }
    if (!success(status)) {
        (void)shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    self.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult P256ArtifactVerifier::shutdown() noexcept {
    Impl& self = impl();
    if (self.key_ != nullptr) (void)BCryptDestroyKey(self.key_);
    if (self.sha256_ != nullptr) (void)BCryptCloseAlgorithmProvider(self.sha256_, 0);
    if (self.ecdsa_ != nullptr) (void)BCryptCloseAlgorithmProvider(self.ecdsa_, 0);
    self.key_ = nullptr;
    self.sha256_ = nullptr;
    self.ecdsa_ = nullptr;
    self.initialized_ = false;
    return SACCADE_OK;
}

ArtifactVerifier P256ArtifactVerifier::descriptor() noexcept {
    return {this, Impl::verify};
}

} // namespace saccade::model

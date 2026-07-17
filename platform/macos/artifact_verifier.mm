#include "model/p256_verifier.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace saccade::model {
namespace {

bool key_valid(const P256PublicKey& key) noexcept {
    uint8_t combined = 0;
    for (uint8_t byte : key.xy)
        combined |= byte;
    return combined != 0;
}

} // namespace

struct P256ArtifactVerifier::Impl {
    static SaccadeResult verify(void* context, const ArtifactView& artifact) noexcept {
        auto* owner = static_cast<P256ArtifactVerifier*>(context);
        if (owner == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        const Impl& self = owner->impl();
        if (self.key_ == nullptr || artifact.signed_message.data == nullptr || artifact.signed_message.size == 0 ||
            artifact.signature.data == nullptr || artifact.signature.size != artifact_signature_bytes) {
            return SACCADE_ERROR_PERMISSION;
        }
        NSData* message = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(artifact.signed_message.data)
                                               length:artifact.signed_message.size
                                         freeWhenDone:NO];
        NSData* signature = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(artifact.signature.data)
                                                 length:artifact.signature.size
                                           freeWhenDone:NO];
        if (message == nil || signature == nil) return SACCADE_ERROR_BACKEND;
        CFErrorRef error = nullptr;
        const bool valid = SecKeyVerifySignature(self.key_, kSecKeyAlgorithmECDSASignatureMessageRFC4754SHA256,
                                                 (__bridge CFDataRef)message, (__bridge CFDataRef)signature, &error);
        if (error != nullptr) CFRelease(error);
        return valid ? SACCADE_OK : SACCADE_ERROR_PERMISSION;
    }

    SecKeyRef key_ = nullptr;
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
    if (self.key_ != nullptr) return SACCADE_ERROR_ALREADY_EXISTS;
    if (!key_valid(public_key)) return SACCADE_ERROR_INVALID_ARGUMENT;
    std::array<uint8_t, 65> external{};
    external[0] = 0x04;
    for (size_t index = 0; index < public_key.xy.size(); ++index)
        external[index + 1U] = public_key.xy[index];
    NSData* data = [NSData dataWithBytes:external.data() length:external.size()];
    NSDictionary* attributes = @{
        (__bridge id)kSecAttrKeyType : (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
        (__bridge id)kSecAttrKeyClass : (__bridge id)kSecAttrKeyClassPublic,
        (__bridge id)kSecAttrKeySizeInBits : @256
    };
    CFErrorRef error = nullptr;
    self.key_ = SecKeyCreateWithData((__bridge CFDataRef)data, (__bridge CFDictionaryRef)attributes, &error);
    if (error != nullptr) CFRelease(error);
    if (self.key_ == nullptr || !SecKeyIsAlgorithmSupported(self.key_, kSecKeyOperationTypeVerify,
                                                            kSecKeyAlgorithmECDSASignatureMessageRFC4754SHA256)) {
        (void)shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    return SACCADE_OK;
}

SaccadeResult P256ArtifactVerifier::shutdown() noexcept {
    Impl& self = impl();
    if (self.key_ != nullptr) CFRelease(self.key_);
    self.key_ = nullptr;
    return SACCADE_OK;
}

ArtifactVerifier P256ArtifactVerifier::descriptor() noexcept {
    return {this, Impl::verify};
}

} // namespace saccade::model

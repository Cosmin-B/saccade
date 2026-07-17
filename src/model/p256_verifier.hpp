#ifndef SACCADE_MODEL_P256_VERIFIER_HPP
#define SACCADE_MODEL_P256_VERIFIER_HPP

#include "model/artifact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::model {

struct P256PublicKey {
    std::array<uint8_t, 64> xy{};
};

class P256ArtifactVerifier final {
  public:
    static constexpr size_t storage_size = 512;

    P256ArtifactVerifier() noexcept;
    ~P256ArtifactVerifier();

    P256ArtifactVerifier(const P256ArtifactVerifier&) = delete;
    P256ArtifactVerifier& operator=(const P256ArtifactVerifier&) = delete;
    P256ArtifactVerifier(P256ArtifactVerifier&&) = delete;
    P256ArtifactVerifier& operator=(P256ArtifactVerifier&&) = delete;

    SaccadeResult initialize(const P256PublicKey&) noexcept;
    SaccadeResult shutdown() noexcept;
    [[nodiscard]] ArtifactVerifier descriptor() noexcept;

  private:
    struct Impl;
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::model

#endif

#ifndef SACCADE_MODEL_MAPPED_ARTIFACT_HPP
#define SACCADE_MODEL_MAPPED_ARTIFACT_HPP

#include "model/artifact.hpp"

#include <cstddef>

namespace saccade::model {

class MappedArtifact final {
  public:
    MappedArtifact() noexcept = default;
    ~MappedArtifact();

    MappedArtifact(const MappedArtifact&) = delete;
    MappedArtifact& operator=(const MappedArtifact&) = delete;
    MappedArtifact(MappedArtifact&&) = delete;
    MappedArtifact& operator=(MappedArtifact&&) = delete;

    SaccadeResult initialize(const char* utf8_path, ArtifactVerifier) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] SaccadeSpanU8 bytes() const noexcept { return {data_, size_}; }

    [[nodiscard]] const ArtifactView& view() const noexcept { return view_; }

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    void unmap() noexcept;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    ArtifactView view_{};
#if defined(_WIN32)
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#else
    int file_ = -1;
#endif
    bool initialized_ = false;
};

} // namespace saccade::model

#endif

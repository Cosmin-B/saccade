#ifndef SACCADE_BACKENDS_D3D12_OVERLAY_RENDERER_HPP
#define SACCADE_BACKENDS_D3D12_OVERLAY_RENDERER_HPP

#include "overlay/glyph_atlas.hpp"

#include <saccade/saccade_backend.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::d3d12 {

struct OverlaySubmission {
    uint64_t sequence = 0;
    uint64_t scene_epoch = 0;
    uint32_t slot_index = 0;
    uint32_t instance_count = 0;
};

struct OverlayRenderTarget {
    ID3D12Resource* texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE view{};
    uint64_t timestamp_ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OverlayInstanceSpan {
    SaccadeOverlayRect* rects = nullptr;
    SaccadeOverlayInstanceMeta* metadata = nullptr;
    size_t capacity = 0;
};

struct OverlayStats {
    uint32_t slot_count = 0;
    uint32_t target_capacity = 0;
    uint32_t instance_capacity = 0;
    uint32_t reserved = 0;
    uint64_t submissions = 0;
    uint64_t busy_submissions = 0;
    uint64_t static_dispatches = 0;
    uint64_t active_dispatches = 0;
    uint64_t packet_upload_bytes = 0;
    uint64_t rendered_frames = 0;
    uint64_t draw_calls = 0;
    uint64_t failures = 0;
};

class OverlayRenderer final {
  public:
    struct Impl;

    static constexpr size_t storage_size = 64 * 1024;

    OverlayRenderer() noexcept;
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;
    OverlayRenderer(OverlayRenderer&&) = delete;
    OverlayRenderer& operator=(OverlayRenderer&&) = delete;

    SaccadeResult initialize(ID3D12Device*, ID3D12CommandQueue*, const char* shader_directory) noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult submit(const SaccadeOverlayFrameDesc&, OverlaySubmission*) noexcept;
    SaccadeResult render(const SaccadeOverlayFrameDesc&, const OverlayRenderTarget&, OverlaySubmission*) noexcept;
    SaccadeResult poll(const OverlaySubmission&, bool*) const noexcept;
    SaccadeResult wait(const OverlaySubmission&, uint64_t timeout_ns) const noexcept;
    SaccadeResult copy_instances(const OverlaySubmission&, OverlayInstanceSpan, size_t*) noexcept;
    SaccadeResult memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] OverlayStats stats() const noexcept;

  private:
    SaccadeResult submit_internal(const SaccadeOverlayFrameDesc&, const OverlayRenderTarget*,
                                  OverlaySubmission*) noexcept;
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(OverlaySubmission) == 24);
static_assert(sizeof(OverlayRenderTarget) == 32);
static_assert(sizeof(OverlayStats) == 80);

} // namespace saccade::backend::d3d12

#endif

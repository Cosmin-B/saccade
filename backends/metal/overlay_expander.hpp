#ifndef SACCADE_BACKENDS_METAL_OVERLAY_EXPANDER_HPP
#define SACCADE_BACKENDS_METAL_OVERLAY_EXPANDER_HPP

#include "overlay/glyph_atlas.hpp"

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::backend::metal {

enum class PathPreference : uint32_t { automatic = 0, metal3 = 3, metal4 = 4 };

enum class Path : uint32_t { unavailable = 0, metal3 = 3, metal4 = 4 };

struct Submission {
    uint64_t sequence = 0;
    uint64_t scene_epoch = 0;
    uint32_t slot_index = 0;
    uint32_t instance_count = 0;
};

struct InstanceSpan {
    SaccadeOverlayRect* rects = nullptr;
    SaccadeOverlayInstanceMeta* metadata = nullptr;
    size_t capacity = 0;
};

enum : uint32_t { render_target_display_link = UINT32_C(0x00000001) };

struct RenderTarget {
    void* texture = nullptr;
    void* drawable = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    double target_presentation_time = 0.0;
};

struct Stats {
    Path path = Path::unavailable;
    uint32_t slot_count = 0;
    uint32_t target_capacity = 0;
    uint32_t instance_capacity = 0;
    uint32_t resident_render_targets = 0;
    uint64_t submissions = 0;
    uint64_t busy_submissions = 0;
    uint64_t static_dispatches = 0;
    uint64_t active_dispatches = 0;
    uint64_t packet_upload_bytes = 0;
    uint64_t command_allocator_bytes = 0;
    uint64_t rendered_frames = 0;
    uint64_t presented_frames = 0;
    uint64_t draw_calls = 0;
    uint64_t render_failures = 0;
};

class OverlayExpander final {
  public:
    static constexpr size_t storage_size = 8192;

    OverlayExpander() noexcept;
    ~OverlayExpander();

    OverlayExpander(const OverlayExpander&) = delete;
    OverlayExpander& operator=(const OverlayExpander&) = delete;
    OverlayExpander(OverlayExpander&&) = delete;
    OverlayExpander& operator=(OverlayExpander&&) = delete;

    SaccadeResult initialize(const char* metallib_path, PathPreference preference) noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult submit(const SaccadeOverlayFrameDesc&, Submission*) noexcept;
    SaccadeResult render(const SaccadeOverlayFrameDesc&, const RenderTarget&, Submission*) noexcept;
    SaccadeResult poll(const Submission&, bool* out_complete) const noexcept;
    SaccadeResult wait(const Submission&, uint64_t timeout_ns) const noexcept;
    SaccadeResult copy_instances(const Submission&, InstanceSpan, size_t* out_count) const noexcept;
    SaccadeResult memory_stats(SaccadeMemoryStats*) const noexcept;

    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] void* native_device() const noexcept;

  private:
    struct Impl;

    SaccadeResult submit_internal(const SaccadeOverlayFrameDesc&, const RenderTarget*, Submission*) noexcept;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::backend::metal

#endif

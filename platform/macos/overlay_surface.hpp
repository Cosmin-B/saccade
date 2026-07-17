#ifndef SACCADE_PLATFORM_MACOS_OVERLAY_SURFACE_HPP
#define SACCADE_PLATFORM_MACOS_OVERLAY_SURFACE_HPP

#include "backends/metal/overlay_expander.hpp"
#include "geometry/display_catalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

using LoadOverlayFrame = SaccadeResult (*)(void*, uint64_t, SaccadeOverlayFrameDesc*) noexcept;
using ObserveOverlayFrame = void (*)(void*, uint64_t, SaccadeResult, const backend::metal::Submission*) noexcept;

struct OverlaySurfaceCallbacks {
    void* context = nullptr;
    LoadOverlayFrame load_frame = nullptr;
    ObserveOverlayFrame observe_frame = nullptr;
};

enum : uint32_t {
    overlay_surface_initialized = UINT32_C(0x00000001),
    overlay_surface_visible = UINT32_C(0x00000002),
    overlay_surface_paused = UINT32_C(0x00000004),
    overlay_surface_click_through = UINT32_C(0x00000008),
    overlay_surface_nonactivating = UINT32_C(0x00000010),
    overlay_surface_all_spaces = UINT32_C(0x00000020),
    overlay_surface_color_managed = UINT32_C(0x00000040),
    overlay_surface_display_paced = UINT32_C(0x00000080)
};

struct OverlaySurfaceInfo {
    uint64_t display_id = 0;
    uint64_t window_number = 0;
    uint32_t drawable_width = 0;
    uint32_t drawable_height = 0;
    uint32_t preferred_fps = 0;
    uint32_t maximum_drawable_count = 0;
    int32_t window_level = 0;
    uint32_t flags = 0;
};

struct OverlaySurfaceStats {
    uint64_t display_ticks = 0;
    uint64_t rendered_frames = 0;
    uint64_t no_frame_ticks = 0;
    uint64_t busy_frames = 0;
    uint64_t deadline_misses = 0;
    uint64_t failures = 0;
    uint64_t last_scene_epoch = 0;
    uint64_t last_transform_epoch = 0;
    uint64_t last_callback_ns = 0;
    uint64_t maximum_callback_ns = 0;
};

struct OverlaySurfaceMemoryStats {
    SaccadeMemoryStats renderer{};
    uint64_t drawable_bytes_estimate = 0;
    uint64_t surface_host_bytes = 0;
    uint64_t total_known_and_estimated = 0;
    uint32_t drawable_width = 0;
    uint32_t drawable_height = 0;
    uint32_t drawable_count = 0;
    uint32_t reserved = 0;
};

static_assert(sizeof(OverlaySurfaceInfo) == 40);
static_assert(sizeof(OverlaySurfaceStats) == 80);
static_assert(sizeof(OverlaySurfaceMemoryStats) == 144);

class OverlaySurface final {
  public:
    static constexpr size_t storage_size = 16384;

    OverlaySurface() noexcept;
    ~OverlaySurface();

    OverlaySurface(const OverlaySurface&) = delete;
    OverlaySurface& operator=(const OverlaySurface&) = delete;
    OverlaySurface(OverlaySurface&&) = delete;
    OverlaySurface& operator=(OverlaySurface&&) = delete;

    SaccadeResult initialize(const geometry::DisplaySurface&, const char* metallib_path, backend::metal::PathPreference,
                             OverlaySurfaceCallbacks) noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult update_display(const geometry::DisplaySurface&) noexcept;
    SaccadeResult start() noexcept;
    SaccadeResult stop() noexcept;
    SaccadeResult request_present(uint32_t animation_ticks, bool animate_active_target) noexcept;
    SaccadeResult set_click_through(bool enabled) noexcept;
    SaccadeResult read_info(OverlaySurfaceInfo*) const noexcept;
    SaccadeResult read_stats(OverlaySurfaceStats*) const noexcept;
    SaccadeResult read_renderer_stats(backend::metal::Stats*) const noexcept;
    SaccadeResult read_memory_stats(OverlaySurfaceMemoryStats*) const noexcept;

  private:
    struct Impl;
    friend void overlay_surface_display_tick(void*, void*) noexcept;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

struct OverlaySurfaceSetStats {
    uint64_t synchronize_attempts = 0;
    uint64_t topology_changes = 0;
    uint64_t surfaces_added = 0;
    uint64_t surfaces_removed = 0;
    uint64_t surfaces_updated = 0;
    uint64_t failures = 0;
    uint64_t topology_epoch = 0;
    uint32_t active_surfaces = 0;
    uint32_t running = 0;
};

static_assert(sizeof(OverlaySurfaceSetStats) == 64);

class OverlaySurfaceSet final {
  public:
    static constexpr size_t metallib_path_capacity = 1024;
    static constexpr size_t storage_size = 416 * 1024;

    OverlaySurfaceSet() noexcept;
    ~OverlaySurfaceSet();

    OverlaySurfaceSet(const OverlaySurfaceSet&) = delete;
    OverlaySurfaceSet& operator=(const OverlaySurfaceSet&) = delete;
    OverlaySurfaceSet(OverlaySurfaceSet&&) = delete;
    OverlaySurfaceSet& operator=(OverlaySurfaceSet&&) = delete;

    SaccadeResult initialize(const char* metallib_path, backend::metal::PathPreference,
                             OverlaySurfaceCallbacks) noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult synchronize(const geometry::DisplaySnapshot&) noexcept;
    SaccadeResult start() noexcept;
    SaccadeResult stop() noexcept;
    SaccadeResult request_present(uint32_t animation_ticks, bool animate_active_target) noexcept;
    SaccadeResult set_click_through(bool enabled) noexcept;
    SaccadeResult read_stats(OverlaySurfaceSetStats*) const noexcept;
    SaccadeResult read_surface_info(uint64_t display_id, OverlaySurfaceInfo*) const noexcept;
    SaccadeResult read_surface_stats(uint64_t display_id, OverlaySurfaceStats*) const noexcept;
    SaccadeResult read_surface_memory_stats(uint64_t display_id, OverlaySurfaceMemoryStats*) const noexcept;
    SaccadeResult read_surface_renderer_stats(uint64_t display_id, backend::metal::Stats*) const noexcept;

  private:
    struct Impl;

    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
};

} // namespace saccade::platform::macos

#endif

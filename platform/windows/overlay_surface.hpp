#ifndef SACCADE_PLATFORM_WINDOWS_OVERLAY_SURFACE_HPP
#define SACCADE_PLATFORM_WINDOWS_OVERLAY_SURFACE_HPP

#include "backends/d3d12/overlay_renderer.hpp"
#include "geometry/display_catalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::windows {

using LoadOverlayFrame = SaccadeResult (*)(void*, uint64_t, SaccadeOverlayFrameDesc*) noexcept;
using ObserveOverlayFrame = void (*)(void*, uint64_t, SaccadeResult, const backend::d3d12::OverlaySubmission*) noexcept;

struct OverlaySurfaceCallbacks {
    void* context = nullptr;
    LoadOverlayFrame load_frame = nullptr;
    ObserveOverlayFrame observe_frame = nullptr;
};

enum : uint32_t {
    overlay_surface_initialized = UINT32_C(0x00000001),
    overlay_surface_visible = UINT32_C(0x00000002),
    overlay_surface_click_through = UINT32_C(0x00000004),
    overlay_surface_nonactivating = UINT32_C(0x00000008),
    overlay_surface_topmost = UINT32_C(0x00000010),
    overlay_surface_excluded_from_capture = UINT32_C(0x00000020),
    overlay_surface_color_managed = UINT32_C(0x00000040),
    overlay_surface_display_paced = UINT32_C(0x00000080)
};

enum class OverlaySurfaceNativeStage : uint32_t {
    none = 0,
    window = 1,
    capture_exclusion = 2,
    dxgi_device = 3,
    swapchain = 4,
    color_space = 5,
    frame_latency = 6,
    composition_device = 7,
    composition_target = 8,
    composition_visual = 9,
    render_views = 10,
    renderer = 11
};

struct OverlaySurfaceInfo {
    uint64_t display_id = 0;
    uint64_t window_handle = 0;
    uint64_t frame_latency_handle = 0;
    uint32_t drawable_width = 0;
    uint32_t drawable_height = 0;
    uint32_t buffer_count = 0;
    uint32_t flags = 0;
};

struct OverlaySurfaceStats {
    uint64_t presentation_attempts = 0;
    uint64_t rendered_frames = 0;
    uint64_t presented_frames = 0;
    uint64_t no_frame_ticks = 0;
    uint64_t busy_frames = 0;
    uint64_t failures = 0;
    uint64_t last_scene_epoch = 0;
    uint64_t last_transform_epoch = 0;
    uint64_t pacing_not_ready = 0;
    uint64_t pacing_failures = 0;
};

struct OverlaySurfaceMemoryStats {
    SaccadeMemoryStats renderer{};
    uint64_t swapchain_bytes_estimate = 0;
    uint64_t surface_host_bytes = 0;
    uint64_t total_known_and_estimated = 0;
};

class OverlaySurface final {
  public:
    static constexpr size_t storage_size = 96 * 1024;

    OverlaySurface() noexcept;
    ~OverlaySurface();

    OverlaySurface(const OverlaySurface&) = delete;
    OverlaySurface& operator=(const OverlaySurface&) = delete;
    OverlaySurface(OverlaySurface&&) = delete;
    OverlaySurface& operator=(OverlaySurface&&) = delete;

    SaccadeResult initialize(const geometry::DisplaySurface&, ID3D12Device*, ID3D12CommandQueue*,
                             const char* shader_directory, OverlaySurfaceCallbacks) noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult update_display(const geometry::DisplaySurface&) noexcept;
    SaccadeResult start() noexcept;
    SaccadeResult stop() noexcept;
    SaccadeResult present(uint64_t now_ns) noexcept;
    SaccadeResult set_click_through(bool enabled) noexcept;
    SaccadeResult read_info(OverlaySurfaceInfo*) const noexcept;
    SaccadeResult read_stats(OverlaySurfaceStats*) const noexcept;
    SaccadeResult read_renderer_stats(backend::d3d12::OverlayStats*) const noexcept;
    SaccadeResult read_memory_stats(OverlaySurfaceMemoryStats*) const noexcept;
    SaccadeResult read_last_native_error(int32_t*, OverlaySurfaceNativeStage*) const noexcept;

  private:
    struct Impl;
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    int32_t last_native_error_ = 0;
    OverlaySurfaceNativeStage last_native_stage_ = OverlaySurfaceNativeStage::none;
    bool initialized_ = false;
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

class OverlaySurfaceSet final {
  public:
    static constexpr size_t shader_directory_capacity = 1024;
    static constexpr size_t storage_size = 1728 * 1024;

    OverlaySurfaceSet() noexcept;
    ~OverlaySurfaceSet();

    OverlaySurfaceSet(const OverlaySurfaceSet&) = delete;
    OverlaySurfaceSet& operator=(const OverlaySurfaceSet&) = delete;
    OverlaySurfaceSet(OverlaySurfaceSet&&) = delete;
    OverlaySurfaceSet& operator=(OverlaySurfaceSet&&) = delete;

    SaccadeResult initialize(ID3D12Device*, ID3D12CommandQueue*, const char* shader_directory,
                             OverlaySurfaceCallbacks) noexcept;
    SaccadeResult shutdown() noexcept;
    SaccadeResult set_glyph_atlas(overlay::GlyphAtlasView) noexcept;
    SaccadeResult synchronize(const geometry::DisplaySnapshot&) noexcept;
    SaccadeResult start() noexcept;
    SaccadeResult stop() noexcept;
    SaccadeResult present(uint64_t display_id, uint64_t now_ns) noexcept;
    SaccadeResult set_click_through(bool enabled) noexcept;
    SaccadeResult read_stats(OverlaySurfaceSetStats*) const noexcept;
    SaccadeResult read_surface_info(uint64_t, OverlaySurfaceInfo*) const noexcept;
    SaccadeResult read_surface_stats(uint64_t, OverlaySurfaceStats*) const noexcept;
    SaccadeResult read_surface_memory_stats(uint64_t, OverlaySurfaceMemoryStats*) const noexcept;
    SaccadeResult read_surface_renderer_stats(uint64_t, backend::d3d12::OverlayStats*) const noexcept;

  private:
    struct Impl;
    [[nodiscard]] Impl& impl() noexcept;
    [[nodiscard]] const Impl& impl() const noexcept;

    alignas(64) std::array<std::byte, storage_size> storage_{};
    bool initialized_ = false;
};

static_assert(sizeof(OverlaySurfaceInfo) == 40);
static_assert(sizeof(OverlaySurfaceStats) == 80);
static_assert(sizeof(OverlaySurfaceMemoryStats) == 128);
static_assert(sizeof(OverlaySurfaceSetStats) == 64);

} // namespace saccade::platform::windows

#endif

#include "platform/macos/overlay_surface.hpp"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace saccade::platform::macos {
void overlay_surface_display_tick(void*, void*) noexcept;
}

@interface SaccadeOverlayPanel : NSPanel
@end

@implementation SaccadeOverlayPanel

- (BOOL)canBecomeKeyWindow {
    return NO;
}

- (BOOL)canBecomeMainWindow {
    return NO;
}

@end

@interface SaccadeOverlayDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, assign) void* owner;
@end

@implementation SaccadeOverlayDisplayLinkDelegate

- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    (void)link;
    saccade::platform::macos::overlay_surface_display_tick(self.owner, (__bridge void*)update);
}

@end

namespace saccade::platform::macos {
namespace {

bool main_thread() noexcept {
    return [NSThread isMainThread];
}

bool display_valid(const geometry::DisplaySurface& display) noexcept {
    return display.display_id != 0 && display.backing_width != 0 && display.backing_height != 0 &&
           display.maximum_fps != 0 && geometry::rect_valid(display.desktop_bounds);
}

CGDirectDisplayID screen_display_id(NSScreen* screen) noexcept {
    id value = screen.deviceDescription[@"NSScreenNumber"];
    return [value isKindOfClass:NSNumber.class]
               ? static_cast<CGDirectDisplayID>(static_cast<NSNumber*>(value).unsignedIntValue)
               : kCGNullDirectDisplay;
}

NSScreen* find_screen(uint64_t display_id) noexcept {
    if (display_id > UINT32_MAX) {
        return nil;
    }
    const CGDirectDisplayID expected = static_cast<CGDirectDisplayID>(display_id);
    for (NSScreen* screen in NSScreen.screens) {
        if (screen_display_id(screen) == expected) {
            return screen;
        }
    }
    return nil;
}

uint64_t elapsed_ns(std::chrono::steady_clock::time_point begin) noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
}

} // namespace

struct OverlaySurface::Impl {
    SaccadeOverlayPanel* panel_ = nil;
    NSView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    CAMetalDisplayLink* display_link_ = nil;
    SaccadeOverlayDisplayLinkDelegate* delegate_ = nil;
    geometry::DisplaySurface display_{};
    OverlaySurfaceCallbacks callbacks_{};
    OverlaySurfaceStats stats_{};
    backend::metal::OverlayExpander renderer_{};
    uint32_t animation_ticks_remaining_ = 0;
    bool animate_active_target_ = false;
    bool initialized_ = false;
    bool visible_ = false;

    ~Impl() {
        if (display_link_ != nil) {
            display_link_.paused = YES;
            [display_link_ invalidate];
        }
        if (delegate_ != nil) {
            delegate_.owner = nullptr;
        }
        if (panel_ != nil) {
            [panel_ orderOut:nil];
            [panel_ close];
        }
    }

    SaccadeResult apply_display(const geometry::DisplaySurface& display) noexcept {
        NSScreen* screen = find_screen(display.display_id);
        if (screen == nil) {
            return SACCADE_ERROR_NOT_FOUND;
        }
        display_ = display;
        [panel_ setFrame:screen.frame display:NO];
        view_.frame = panel_.contentView.bounds;
        layer_.frame = view_.bounds;
        layer_.contentsScale = screen.backingScaleFactor;
        layer_.drawableSize = CGSizeMake(display.backing_width, display.backing_height);
        CGColorSpaceRef color_space = CGDisplayCopyColorSpace(static_cast<CGDirectDisplayID>(display.display_id));
        if (color_space == nullptr) return SACCADE_ERROR_BACKEND;
        layer_.colorspace = color_space;
        CGColorSpaceRelease(color_space);
        const float maximum = static_cast<float>(display.maximum_fps);
        const float preferred = std::min(maximum, 120.0F);
        display_link_.preferredFrameRateRange = CAFrameRateRangeMake(std::min(30.0F, preferred), preferred, preferred);
        return SACCADE_OK;
    }
};

void overlay_surface_display_tick(void* owner, void* raw_update) noexcept {
    if (owner == nullptr || raw_update == nullptr) {
        return;
    }
    OverlaySurface* surface = static_cast<OverlaySurface*>(owner);
    OverlaySurface::Impl& state = surface->impl();
    if (!state.initialized_ || !state.visible_) {
        return;
    }

    @autoreleasepool {
        const auto begin = std::chrono::steady_clock::now();
        CAMetalDisplayLinkUpdate* update = (__bridge CAMetalDisplayLinkUpdate*)raw_update;
        ++state.stats_.display_ticks;

        SaccadeOverlayFrameDesc frame{};
        const SaccadeResult loaded =
            state.callbacks_.load_frame(state.callbacks_.context, state.display_.display_id, &frame);
        SaccadeResult result = loaded;
        backend::metal::Submission submission{};
        const backend::metal::Submission* observed_submission = nullptr;
        if (loaded == SACCADE_ERROR_NOT_FOUND) {
            ++state.stats_.no_frame_ticks;
        } else if (loaded != SACCADE_OK) {
            ++state.stats_.failures;
        } else {
            id<CAMetalDrawable> drawable = update.drawable;
            id<MTLTexture> texture = drawable.texture;
            if (texture.width > UINT32_MAX || texture.height > UINT32_MAX) {
                result = SACCADE_ERROR_CAPACITY;
            } else {
                const backend::metal::RenderTarget target{(__bridge void*)texture,
                                                          (__bridge void*)drawable,
                                                          static_cast<uint32_t>(texture.width),
                                                          static_cast<uint32_t>(texture.height),
                                                          backend::metal::render_target_display_link,
                                                          0,
                                                          update.targetPresentationTimestamp};
                result = state.renderer_.render(frame, target, &submission);
            }
            if (result == SACCADE_OK) {
                ++state.stats_.rendered_frames;
                state.stats_.last_scene_epoch = frame.scene_epoch;
                state.stats_.last_transform_epoch = frame.transform_epoch;
                observed_submission = &submission;
                if (state.animation_ticks_remaining_ != 0) --state.animation_ticks_remaining_;
                if (state.animation_ticks_remaining_ == 0 &&
                    (!state.animate_active_target_ || (frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) == 0)) {
                    state.display_link_.paused = YES;
                }
            } else if (result == SACCADE_ERROR_BUSY) {
                ++state.stats_.busy_frames;
            } else {
                ++state.stats_.failures;
            }
        }

        if (state.callbacks_.observe_frame != nullptr) {
            state.callbacks_.observe_frame(state.callbacks_.context, state.display_.display_id, result,
                                           observed_submission);
        }
        if (CACurrentMediaTime() > update.targetTimestamp) {
            ++state.stats_.deadline_misses;
        }
        const uint64_t duration = elapsed_ns(begin);
        state.stats_.last_callback_ns = duration;
        state.stats_.maximum_callback_ns = std::max(state.stats_.maximum_callback_ns, duration);
    }
}

OverlaySurface::OverlaySurface() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    new (storage_.data()) Impl{};
}

OverlaySurface::~OverlaySurface() {
    assert(main_thread() || !impl().initialized_);
    impl().~Impl();
}

OverlaySurface::Impl& OverlaySurface::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlaySurface::Impl& OverlaySurface::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlaySurface::initialize(const geometry::DisplaySurface& display, const char* metallib_path,
                                         backend::metal::PathPreference preference,
                                         OverlaySurfaceCallbacks callbacks) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (!display_valid(display) || callbacks.load_frame == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    NSScreen* screen = find_screen(display.display_id);
    if (screen == nil) {
        return SACCADE_ERROR_NOT_FOUND;
    }

    const SaccadeResult initialized = state.renderer_.initialize(metallib_path, preference);
    if (initialized != SACCADE_OK) {
        return initialized;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        constexpr NSWindowStyleMask style = NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;
        state.panel_ = [[SaccadeOverlayPanel alloc] initWithContentRect:screen.frame
                                                              styleMask:style
                                                                backing:NSBackingStoreBuffered
                                                                  defer:NO
                                                                 screen:screen];
        state.view_ = [[NSView alloc] initWithFrame:state.panel_.contentView.bounds];
        state.layer_ = [CAMetalLayer layer];
        state.delegate_ = [[SaccadeOverlayDisplayLinkDelegate alloc] init];
        if (state.panel_ == nil || state.view_ == nil || state.layer_ == nil || state.delegate_ == nil) {
            return SACCADE_ERROR_BACKEND;
        }

        state.panel_.opaque = NO;
        state.panel_.backgroundColor = NSColor.clearColor;
        state.panel_.hasShadow = NO;
        state.panel_.hidesOnDeactivate = NO;
        state.panel_.releasedWhenClosed = NO;
        state.panel_.ignoresMouseEvents = YES;
        state.panel_.acceptsMouseMovedEvents = NO;
        state.panel_.movable = NO;
        state.panel_.level = NSStatusWindowLevel;
        state.panel_.animationBehavior = NSWindowAnimationBehaviorNone;
        state.panel_.sharingType = NSWindowSharingNone;
        state.panel_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                          NSWindowCollectionBehaviorFullScreenAuxiliary |
                                          NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;

        state.layer_.device = (__bridge id<MTLDevice>)state.renderer_.native_device();
        state.layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        state.layer_.framebufferOnly = YES;
        state.layer_.opaque = NO;
        state.layer_.presentsWithTransaction = NO;
        state.layer_.maximumDrawableCount = 3;
        state.layer_.allowsNextDrawableTimeout = YES;
        state.layer_.displaySyncEnabled = YES;
        state.view_.wantsLayer = YES;
        state.view_.layer = state.layer_;
        state.panel_.contentView = state.view_;

        state.delegate_.owner = this;
        state.display_link_ = [[CAMetalDisplayLink alloc] initWithMetalLayer:state.layer_];
        if (state.display_link_ == nil) {
            return SACCADE_ERROR_BACKEND;
        }
        state.display_link_.delegate = state.delegate_;
        state.display_link_.preferredFrameLatency = 1.0F;
        state.display_link_.paused = YES;
        [state.display_link_ addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        state.callbacks_ = callbacks;
        const SaccadeResult applied = state.apply_display(display);
        if (applied != SACCADE_OK) {
            return applied;
        }
    }
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    if (!main_thread()) return SACCADE_ERROR_STATE;
    return impl().initialized_ ? impl().renderer_.set_glyph_atlas(atlas) : SACCADE_ERROR_STATE;
}

SaccadeResult OverlaySurface::update_display(const geometry::DisplaySurface& display) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_ || display.display_id != state.display_.display_id) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!display_valid(display)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const bool paused = state.display_link_.paused;
    state.display_link_.paused = YES;
    const SaccadeResult result = state.apply_display(display);
    state.display_link_.paused = paused;
    return result;
}

SaccadeResult OverlaySurface::start() noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (!state.visible_) {
        [state.panel_ orderFrontRegardless];
        state.visible_ = true;
        state.animation_ticks_remaining_ = std::max(state.animation_ticks_remaining_, 1U);
        state.display_link_.paused = NO;
    }
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::stop() noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    state.display_link_.paused = YES;
    state.visible_ = false;
    [state.panel_ orderOut:nil];
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::request_present(uint32_t animation_ticks, bool animate_active_target) noexcept {
    if (!main_thread() || animation_ticks == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_ERROR_STATE;
    state.animation_ticks_remaining_ = std::max(state.animation_ticks_remaining_, animation_ticks);
    state.animate_active_target_ = animate_active_target;
    if (state.visible_) state.display_link_.paused = NO;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::set_click_through(bool enabled) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    state.panel_.ignoresMouseEvents = enabled ? YES : NO;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_info(OverlaySurfaceInfo* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    const Impl& state = impl();
    if (!state.initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    OverlaySurfaceInfo result{};
    result.display_id = state.display_.display_id;
    result.window_number = static_cast<uint64_t>(state.panel_.windowNumber);
    result.drawable_width = static_cast<uint32_t>(state.layer_.drawableSize.width);
    result.drawable_height = static_cast<uint32_t>(state.layer_.drawableSize.height);
    result.preferred_fps = static_cast<uint32_t>(state.display_link_.preferredFrameRateRange.preferred);
    result.maximum_drawable_count = static_cast<uint32_t>(state.layer_.maximumDrawableCount);
    result.window_level = static_cast<int32_t>(state.panel_.level);
    result.flags =
        overlay_surface_initialized | (state.visible_ ? overlay_surface_visible : 0U) |
        (state.display_link_.paused ? overlay_surface_paused : 0U) |
        (state.panel_.ignoresMouseEvents ? overlay_surface_click_through : 0U) |
        ((state.panel_.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0 ? overlay_surface_nonactivating : 0U) |
        ((state.panel_.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0
             ? overlay_surface_all_spaces
             : 0U) |
        (state.layer_.colorspace != nullptr ? overlay_surface_color_managed : 0U) |
        (state.layer_.displaySyncEnabled ? overlay_surface_display_paced : 0U);
    *output = result;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_stats(OverlaySurfaceStats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_renderer_stats(backend::metal::Stats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().renderer_.stats();
    return SACCADE_OK;
}

SaccadeResult OverlaySurface::read_memory_stats(OverlaySurfaceMemoryStats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    const Impl& state = impl();
    if (!state.initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    OverlaySurfaceMemoryStats result{};
    result.renderer.struct_size = sizeof(result.renderer);
    result.renderer.api_version = SACCADE_API_VERSION;
    const SaccadeResult measured = state.renderer_.memory_stats(&result.renderer);
    if (measured != SACCADE_OK) {
        return measured;
    }
    result.drawable_width = static_cast<uint32_t>(state.layer_.drawableSize.width);
    result.drawable_height = static_cast<uint32_t>(state.layer_.drawableSize.height);
    result.drawable_count = static_cast<uint32_t>(state.layer_.maximumDrawableCount);
    result.drawable_bytes_estimate =
        static_cast<uint64_t>(result.drawable_width) * result.drawable_height * 4U * result.drawable_count;
    result.surface_host_bytes = sizeof(OverlaySurface);
    result.total_known_and_estimated = result.surface_host_bytes + result.renderer.device_imported +
                                       result.renderer.device_owned + result.renderer.framework_opaque +
                                       result.drawable_bytes_estimate;
    *output = result;
    return SACCADE_OK;
}

struct OverlaySurfaceSet::Impl {
    struct Slot {
        alignas(OverlaySurface) std::array<std::byte, sizeof(OverlaySurface)> storage_{};
        uint64_t display_id_ = 0;
        bool active_ = false;

        OverlaySurface& surface() noexcept { return *std::launder(reinterpret_cast<OverlaySurface*>(storage_.data())); }

        const OverlaySurface& surface() const noexcept {
            return *std::launder(reinterpret_cast<const OverlaySurface*>(storage_.data()));
        }

        void construct(uint64_t display_id) noexcept {
            new (storage_.data()) OverlaySurface{};
            display_id_ = display_id;
            active_ = true;
        }

        void destroy() noexcept {
            if (active_) {
                surface().~OverlaySurface();
                display_id_ = 0;
                active_ = false;
            }
        }
    };

    std::array<Slot, geometry::display_capacity> slots_{};
    overlay::GlyphAtlasStorage glyph_atlas_{};
    std::array<char, metallib_path_capacity> metallib_path_{};
    OverlaySurfaceCallbacks callbacks_{};
    backend::metal::PathPreference preference_ = backend::metal::PathPreference::automatic;
    OverlaySurfaceSetStats stats_{};
    bool initialized_ = false;
    bool running_ = false;
    bool click_through_ = true;
    bool has_glyph_atlas_ = false;

    ~Impl() {
        for (Slot& slot : slots_) {
            slot.destroy();
        }
    }

    Slot* find(uint64_t display_id) noexcept {
        for (Slot& slot : slots_) {
            if (slot.active_ && slot.display_id_ == display_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    const Slot* find(uint64_t display_id) const noexcept {
        for (const Slot& slot : slots_) {
            if (slot.active_ && slot.display_id_ == display_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* free_slot() noexcept {
        for (Slot& slot : slots_) {
            if (!slot.active_) {
                return &slot;
            }
        }
        return nullptr;
    }

    uint32_t active_count() const noexcept {
        uint32_t count = 0;
        for (const Slot& slot : slots_) {
            count += slot.active_ ? 1U : 0U;
        }
        return count;
    }
};

OverlaySurfaceSet::OverlaySurfaceSet() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    new (storage_.data()) Impl{};
}

OverlaySurfaceSet::~OverlaySurfaceSet() {
    assert(main_thread() || !impl().initialized_);
    impl().~Impl();
}

OverlaySurfaceSet::Impl& OverlaySurfaceSet::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlaySurfaceSet::Impl& OverlaySurfaceSet::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlaySurfaceSet::initialize(const char* metallib_path, backend::metal::PathPreference preference,
                                            OverlaySurfaceCallbacks callbacks) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    if (metallib_path == nullptr || callbacks.load_frame == nullptr ||
        (preference != backend::metal::PathPreference::automatic &&
         preference != backend::metal::PathPreference::metal3 &&
         preference != backend::metal::PathPreference::metal4)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    size_t length = 0;
    while (length < metallib_path_capacity && metallib_path[length] != '\0') {
        ++length;
    }
    if (length == 0 || length == metallib_path_capacity) {
        return SACCADE_ERROR_CAPACITY;
    }
    std::memcpy(state.metallib_path_.data(), metallib_path, length + 1U);
    state.preference_ = preference;
    state.callbacks_ = callbacks;
    state.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    if (!main_thread()) return SACCADE_ERROR_STATE;
    Impl& state = impl();
    if (!state.initialized_ || !overlay::glyph_atlas_valid(atlas)) return SACCADE_ERROR_INVALID_ARGUMENT;

    std::array<bool, geometry::display_capacity> updated{};
    for (size_t index = 0; index < state.slots_.size(); ++index) {
        Impl::Slot& slot = state.slots_[index];
        if (slot.active_) {
            const SaccadeResult result = slot.surface().set_glyph_atlas(atlas);
            if (result != SACCADE_OK) {
                if (state.has_glyph_atlas_) {
                    for (size_t rollback = 0; rollback < index; ++rollback) {
                        if (updated[rollback])
                            (void)state.slots_[rollback].surface().set_glyph_atlas(state.glyph_atlas_.view());
                    }
                }
                return result;
            }
            updated[index] = true;
        }
    }
    std::memcpy(state.glyph_atlas_.pixels.data(), atlas.pixels, overlay::glyph_atlas_bytes);
    std::memcpy(state.glyph_atlas_.symbols.data(), atlas.symbols,
                static_cast<size_t>(atlas.glyph_count) * sizeof(uint16_t));
    state.glyph_atlas_.glyph_count = atlas.glyph_count;
    state.has_glyph_atlas_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::synchronize(const geometry::DisplaySnapshot& snapshot) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_ || snapshot.epoch == 0 || snapshot.count == 0 ||
        snapshot.count > geometry::display_capacity) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    ++state.stats_.synchronize_attempts;
    if (snapshot.epoch < state.stats_.topology_epoch) {
        ++state.stats_.failures;
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (snapshot.epoch == state.stats_.topology_epoch) {
        return SACCADE_OK;
    }

    for (uint32_t index = 0; index < snapshot.count; ++index) {
        if (!display_valid(snapshot.displays[index])) {
            ++state.stats_.failures;
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (find_screen(snapshot.displays[index].display_id) == nil) {
            ++state.stats_.failures;
            return SACCADE_ERROR_NOT_FOUND;
        }
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (snapshot.displays[prior].display_id == snapshot.displays[index].display_id) {
                ++state.stats_.failures;
                return SACCADE_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    uint64_t removed_count = 0;
    for (Impl::Slot& slot : state.slots_) {
        if (!slot.active_) {
            continue;
        }
        bool retained = false;
        for (uint32_t index = 0; index < snapshot.count; ++index) {
            retained |= snapshot.displays[index].display_id == slot.display_id_;
        }
        if (!retained) {
            slot.destroy();
            ++removed_count;
        }
    }

    std::array<bool, geometry::display_capacity> added{};
    uint64_t added_count = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        Impl::Slot* slot = state.find(display.display_id);
        if (slot != nullptr) {
            continue;
        }
        slot = state.free_slot();
        if (slot == nullptr) {
            ++state.stats_.failures;
            return SACCADE_ERROR_CAPACITY;
        }
        slot->construct(display.display_id);
        const size_t slot_index = static_cast<size_t>(slot - state.slots_.data());
        added[slot_index] = true;
        SaccadeResult result =
            slot->surface().initialize(display, state.metallib_path_.data(), state.preference_, state.callbacks_);
        if (result == SACCADE_OK && state.has_glyph_atlas_) {
            result = slot->surface().set_glyph_atlas(state.glyph_atlas_.view());
        }
        if (result == SACCADE_OK && !state.click_through_) {
            result = slot->surface().set_click_through(false);
        }
        if (result == SACCADE_OK && state.running_) {
            result = slot->surface().start();
        }
        if (result != SACCADE_OK) {
            for (size_t rollback = 0; rollback < added.size(); ++rollback) {
                if (added[rollback]) {
                    state.slots_[rollback].destroy();
                }
            }
            state.stats_.surfaces_removed += removed_count;
            state.stats_.active_surfaces = state.active_count();
            ++state.stats_.failures;
            return result;
        }
        ++added_count;
    }

    uint64_t updated_count = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        Impl::Slot* slot = state.find(display.display_id);
        const size_t slot_index = static_cast<size_t>(slot - state.slots_.data());
        if (!added[slot_index]) {
            const SaccadeResult result = slot->surface().update_display(display);
            if (result != SACCADE_OK) {
                for (size_t rollback = 0; rollback < added.size(); ++rollback) {
                    if (added[rollback]) {
                        state.slots_[rollback].destroy();
                    }
                }
                state.stats_.surfaces_removed += removed_count;
                state.stats_.active_surfaces = state.active_count();
                ++state.stats_.failures;
                return result;
            }
            ++updated_count;
        }
    }
    state.stats_.surfaces_added += added_count;
    state.stats_.surfaces_removed += removed_count;
    state.stats_.surfaces_updated += updated_count;
    state.stats_.topology_epoch = snapshot.epoch;
    state.stats_.active_surfaces = state.active_count();
    state.stats_.running = state.running_ ? 1U : 0U;
    ++state.stats_.topology_changes;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::start() noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_ || state.stats_.active_surfaces == 0) {
        return SACCADE_ERROR_STATE;
    }
    if (state.running_) {
        return SACCADE_OK;
    }
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_) {
            const SaccadeResult result = slot.surface().start();
            if (result != SACCADE_OK) {
                for (Impl::Slot& rollback : state.slots_) {
                    if (rollback.active_) {
                        rollback.surface().stop();
                    }
                }
                ++state.stats_.failures;
                return result;
            }
        }
    }
    state.running_ = true;
    state.stats_.running = 1;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::stop() noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    SaccadeResult result = SACCADE_OK;
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_ && slot.surface().stop() != SACCADE_OK) {
            result = SACCADE_ERROR_BACKEND;
        }
    }
    state.running_ = false;
    state.stats_.running = 0;
    state.stats_.failures += result == SACCADE_OK ? 0U : 1U;
    return result;
}

SaccadeResult OverlaySurfaceSet::request_present(uint32_t animation_ticks, bool animate_active_target) noexcept {
    if (!main_thread() || animation_ticks == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_ERROR_STATE;
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_) {
            const SaccadeResult result = slot.surface().request_present(animation_ticks, animate_active_target);
            if (result != SACCADE_OK) {
                ++state.stats_.failures;
                return result;
            }
        }
    }
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::set_click_through(bool enabled) noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    Impl& state = impl();
    if (!state.initialized_) {
        return SACCADE_ERROR_STATE;
    }
    for (Impl::Slot& slot : state.slots_) {
        if (slot.active_) {
            const SaccadeResult result = slot.surface().set_click_through(enabled);
            if (result != SACCADE_OK) {
                ++state.stats_.failures;
                return result;
            }
        }
    }
    state.click_through_ = enabled;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::read_stats(OverlaySurfaceSetStats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult OverlaySurfaceSet::read_surface_info(uint64_t display_id, OverlaySurfaceInfo* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || display_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface().read_info(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_stats(uint64_t display_id, OverlaySurfaceStats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || display_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface().read_stats(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_memory_stats(uint64_t display_id,
                                                           OverlaySurfaceMemoryStats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || display_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface().read_memory_stats(output);
}

SaccadeResult OverlaySurfaceSet::read_surface_renderer_stats(uint64_t display_id,
                                                             backend::metal::Stats* output) const noexcept {
    if (!main_thread()) {
        return SACCADE_ERROR_STATE;
    }
    if (!impl().initialized_ || display_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl::Slot* slot = impl().find(display_id);
    return slot == nullptr ? SACCADE_ERROR_NOT_FOUND : slot->surface().read_renderer_stats(output);
}

} // namespace saccade::platform::macos

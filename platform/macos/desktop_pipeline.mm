#include "platform/macos/desktop_pipeline.hpp"

#include "input/execution_preflight.hpp"
#include "model/coreml_contract.hpp"
#include "platform/macos/glyph_atlas.hpp"
#include "platform/macos/keyboard.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CGSession.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <time.h>
#include <unistd.h>

namespace saccade::platform::macos {
namespace {

constexpr uint64_t activation_timeout_ns = UINT64_C(2'000'000'000);
constexpr uint64_t agent_cancel_wait_ns = UINT64_C(5'000'000'000);
constexpr uint64_t permission_check_period_ns = UINT64_C(250'000'000);
constexpr uint64_t keyboard_layout_check_period_ns = UINT64_C(250'000'000);
constexpr uint64_t active_scope_check_period_ns = scheduler::scene_period_30hz_ns;
constexpr uint64_t preprocess_lead_ns = UINT64_C(2'000'000);
constexpr uint64_t maximum_capture_age_ns = scheduler::scene_period_30hz_ns / 3U;
constexpr uint64_t desktop_source_id = UINT64_C(0x5341434341444501);
constexpr uint64_t grid_source_id = UINT64_C(0x5341434341444502);
constexpr uint64_t window_source_id = UINT64_C(0x5341434341444503);

void preserve_first_error(SaccadeResult candidate, SaccadeResult* first) noexcept {
    if (*first == SACCADE_OK && candidate != SACCADE_OK)
        *first = candidate;
}

void add_memory(const SaccadeMemoryStats& source, SaccadeMemoryStats* total) noexcept {
    total->host_committed += source.host_committed;
    total->host_reserved += source.host_reserved;
    total->device_imported += source.device_imported;
    total->device_owned += source.device_owned;
    total->framework_opaque += source.framework_opaque;
    total->copied_bytes += source.copied_bytes;
    total->high_water_bytes += source.high_water_bytes;
}

bool interactive_session() noexcept {
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (session == nullptr)
        return false;
    const auto on_console = static_cast<CFBooleanRef>(CFDictionaryGetValue(session, kCGSessionOnConsoleKey));
    const auto login_done = static_cast<CFBooleanRef>(CFDictionaryGetValue(session, kCGSessionLoginDoneKey));
    const bool active = on_console == kCFBooleanTrue && login_done == kCFBooleanTrue;
    CFRelease(session);
    return active;
}

bool target_window_available(uint64_t window_id) noexcept {
    if (window_id == 0)
        return true;
    if (window_id > UINT32_MAX)
        return false;
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow | kCGWindowListExcludeDesktopElements,
                                                    static_cast<CGWindowID>(window_id));
    const bool available = windows != nullptr && CFArrayGetCount(windows) != 0;
    if (windows != nullptr)
        CFRelease(windows);
    return available;
}

bool dictionary_i64(CFDictionaryRef dictionary, CFStringRef key, int64_t* output) noexcept {
    if (dictionary == nullptr || output == nullptr)
        return false;
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(value, kCFNumberSInt64Type, output);
}

SaccadeResult public_window_identity(uint64_t window_id, ExplicitWindowIdentity* output) noexcept {
    if (output == nullptr || window_id == 0 || window_id > UINT32_MAX)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    const CGWindowID native_id = static_cast<CGWindowID>(window_id);
    CFArrayRef windows =
        CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow | kCGWindowListExcludeDesktopElements, native_id);
    if (windows == nullptr || CFArrayGetCount(windows) != 1) {
        if (windows != nullptr)
            CFRelease(windows);
        return SACCADE_ERROR_NOT_FOUND;
    }
    const auto description = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
    int64_t process_id = 0;
    int64_t layer = 0;
    int64_t sharing = 0;
    const auto bounds_dictionary = static_cast<CFDictionaryRef>(CFDictionaryGetValue(description, kCGWindowBounds));
    const auto onscreen = static_cast<CFBooleanRef>(CFDictionaryGetValue(description, kCGWindowIsOnscreen));
    CGRect bounds{};
    const bool valid = dictionary_i64(description, kCGWindowOwnerPID, &process_id) && process_id > 0 && process_id <= INT32_MAX &&
                       dictionary_i64(description, kCGWindowLayer, &layer) && layer == 0 &&
                       dictionary_i64(description, kCGWindowSharingState, &sharing) && sharing != kCGWindowSharingNone &&
                       onscreen == kCFBooleanTrue && bounds_dictionary != nullptr &&
                       CGRectMakeWithDictionaryRepresentation(bounds_dictionary, &bounds) && std::isfinite(bounds.origin.x) &&
                       std::isfinite(bounds.origin.y) && std::isfinite(bounds.size.width) && std::isfinite(bounds.size.height) &&
                       bounds.size.width > 0 && bounds.size.height > 0 && bounds.origin.x >= INT32_MIN && bounds.origin.x <= INT32_MAX &&
                       bounds.origin.y >= INT32_MIN && bounds.origin.y <= INT32_MAX && bounds.size.width <= INT32_MAX &&
                       bounds.size.height <= INT32_MAX;
    if (valid) {
        output->process_id = static_cast<uint64_t>(process_id);
        output->window_id = window_id;
        output->capture_source_id = screen_capture_window_source_id(window_id);
        output->bounds = {static_cast<int32_t>(std::llround(bounds.origin.x)), static_cast<int32_t>(std::llround(bounds.origin.y)),
                          static_cast<int32_t>(std::llround(bounds.size.width)), static_cast<int32_t>(std::llround(bounds.size.height))};
        output->flags = explicit_window_visible | explicit_window_current_space;
    }
    CFRelease(windows);
    return valid ? SACCADE_OK : SACCADE_ERROR_NOT_FOUND;
}

bool same_explicit_identity(const ExplicitWindowIdentity& left, const ExplicitWindowIdentity& right) noexcept {
    return left.process_id == right.process_id && left.window_id == right.window_id &&
           left.capture_source_id == right.capture_source_id && left.bounds.x == right.bounds.x && left.bounds.y == right.bounds.y &&
           left.bounds.width == right.bounds.width && left.bounds.height == right.bounds.height && left.flags == right.flags &&
           left.reserved == right.reserved;
}

SaccadeResult read_explicit_native_frame(void* context, SaccadeCaptureStreamHandle stream, SaccadeFrameHandle frame,
                                        NativeCapturedFrame* output) noexcept {
    if (context == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    return static_cast<ScreenCaptureProvider*>(context)->read_native_frame(stream, frame, output);
}

constexpr uint32_t maximum_active_window_scan = 256;

uint64_t frontmost_process_id() noexcept {
    NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
    return app == nil ? 0 : static_cast<uint64_t>(app.processIdentifier);
}

bool focused_window_bounds(uint64_t process_id, SaccadeRectI32* output) noexcept {
    if (process_id == 0 || process_id > INT32_MAX || output == nullptr)
        return false;

    AXUIElementRef application = AXUIElementCreateApplication(static_cast<pid_t>(process_id));
    CFTypeRef focused = nullptr;
    CFTypeRef position = nullptr;
    CFTypeRef size = nullptr;
    const bool found =
        application != nullptr && AXUIElementCopyAttributeValue(application, kAXFocusedWindowAttribute, &focused) == kAXErrorSuccess &&
        focused != nullptr && CFGetTypeID(focused) == AXUIElementGetTypeID() &&
        AXUIElementCopyAttributeValue(static_cast<AXUIElementRef>(focused), kAXPositionAttribute, &position) == kAXErrorSuccess &&
        AXUIElementCopyAttributeValue(static_cast<AXUIElementRef>(focused), kAXSizeAttribute, &size) == kAXErrorSuccess;

    CGPoint origin{};
    CGSize extent{};
    const bool valid =
        found && position != nullptr && size != nullptr && CFGetTypeID(position) == AXValueGetTypeID() &&
        CFGetTypeID(size) == AXValueGetTypeID() && AXValueGetType(static_cast<AXValueRef>(position)) == kAXValueTypeCGPoint &&
        AXValueGetType(static_cast<AXValueRef>(size)) == kAXValueTypeCGSize &&
        AXValueGetValue(static_cast<AXValueRef>(position), kAXValueTypeCGPoint, &origin) &&
        AXValueGetValue(static_cast<AXValueRef>(size), kAXValueTypeCGSize, &extent) && std::isfinite(origin.x) && std::isfinite(origin.y) &&
        std::isfinite(extent.width) && std::isfinite(extent.height) && extent.width > 0 && extent.height > 0 && origin.x >= INT32_MIN &&
        origin.x <= INT32_MAX && origin.y >= INT32_MIN && origin.y <= INT32_MAX && extent.width <= INT32_MAX && extent.height <= INT32_MAX;

    if (valid) {
        *output = {static_cast<int32_t>(std::llround(origin.x)), static_cast<int32_t>(std::llround(origin.y)),
                   static_cast<int32_t>(std::llround(extent.width)), static_cast<int32_t>(std::llround(extent.height))};
    }
    if (size != nullptr)
        CFRelease(size);
    if (position != nullptr)
        CFRelease(position);
    if (focused != nullptr)
        CFRelease(focused);
    if (application != nullptr)
        CFRelease(application);
    return valid;
}

bool same_window_bounds(const SaccadeRectI32& left, const SaccadeRectI32& right) noexcept {
    constexpr int64_t tolerance = 4;
    return std::abs(static_cast<int64_t>(left.x) - right.x) <= tolerance && std::abs(static_cast<int64_t>(left.y) - right.y) <= tolerance &&
           std::abs(static_cast<int64_t>(left.width) - right.width) <= tolerance &&
           std::abs(static_cast<int64_t>(left.height) - right.height) <= tolerance;
}

SaccadeResult active_window_for_process(SaccadeAccessibilityProviderDesc provider, uint64_t process_id,
                                        SaccadeWindowInfo* output) noexcept {
    if (process_id == 0 || output == nullptr || provider.context == nullptr || provider.ops.enumerate_windows == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;

    SaccadeRectI32 focused_bounds{};
    const bool focused_available = focused_window_bounds(process_id, &focused_bounds);
    SaccadeWindowInfo fallback{};
    uint64_t fallback_area = 0;

    for (uint32_t index = 0; index < maximum_active_window_scan; ++index) {
        SaccadeWindowInfo candidate{};
        candidate.struct_size = sizeof(candidate);
        candidate.api_version = SACCADE_API_VERSION;
        const SaccadeResult result = provider.ops.enumerate_windows(provider.context, index, &candidate);
        if (result != SACCADE_OK) {
            if (fallback_area == 0)
                return result;
            *output = fallback;
            return SACCADE_OK;
        }
        if (candidate.process_id != process_id)
            continue;
        if (focused_available && same_window_bounds(candidate.desktop_bounds, focused_bounds)) {
            *output = candidate;
            return SACCADE_OK;
        }

        const uint64_t width = static_cast<uint32_t>(candidate.desktop_bounds.width);
        const uint64_t height = static_cast<uint32_t>(candidate.desktop_bounds.height);
        const uint64_t area = width * height;
        if (area > fallback_area) {
            fallback = candidate;
            fallback_area = area;
        }
    }
    if (fallback_area == 0)
        return SACCADE_ERROR_CAPACITY;
    *output = fallback;
    return SACCADE_OK;
}

bool secure_frontmost_application(NSRunningApplication* app) noexcept {
    if (app == nil)
        return true;
    NSString* identifier = app.bundleIdentifier;
    return [identifier isEqualToString:@"com.apple.loginwindow"] || [identifier hasPrefix:@"com.apple.ScreenSaver"];
}

bool basic_input_environment_available() noexcept {
    NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
    return interactive_session() && !secure_frontmost_application(app) && !IsSecureEventInputEnabled();
}

bool source_for_command(application::Command command, application::TargetSource* source) noexcept {
    switch (command) {
    case application::Command::source_pixel:
        *source = application::TargetSource::pixel;
        return true;
    case application::Command::source_semantic:
        *source = application::TargetSource::semantic;
        return true;
    case application::Command::source_grid:
        *source = application::TargetSource::grid;
        return true;
    case application::Command::source_fused:
        *source = application::TargetSource::fused;
        return true;
    default:
        return false;
    }
}

bool scope_for_command(application::Command command, application::TargetScope* scope) noexcept {
    switch (command) {
    case application::Command::scope_desktop:
        *scope = application::TargetScope::desktop;
        return true;
    case application::Command::scope_active_window:
        *scope = application::TargetScope::active_window;
        return true;
    case application::Command::scope_monitor:
        *scope = application::TargetScope::monitor;
        return true;
    default:
        return false;
    }
}

bool window_navigation_command(application::Command command) noexcept {
    return command >= application::Command::window_cycle_forward && command <= application::Command::window_activate_down;
}

bool mode_command(application::Command command) noexcept {
    return command >= application::Command::mode_single && command <= application::Command::mode_path;
}

bool intersects(const geometry::RectQ8& left, const geometry::RectQ8& right) noexcept {
    return static_cast<int64_t>(left.x) + left.width > right.x && static_cast<int64_t>(right.x) + right.width > left.x &&
           static_cast<int64_t>(left.y) + left.height > right.y && static_cast<int64_t>(right.y) + right.height > left.y;
}

uint64_t display_id_for_window(const geometry::DisplaySnapshot& displays, const SaccadeRectI32& window) noexcept {
    const int64_t center_x_q8 = static_cast<int64_t>(window.x) * 256 + static_cast<int64_t>(window.width) * 128;
    const int64_t center_y_q8 = static_cast<int64_t>(window.y) * 256 + static_cast<int64_t>(window.height) * 128;

    for (uint32_t index = 0; index < displays.count; ++index) {
        const geometry::RectQ8& bounds = displays.displays[index].desktop_bounds;
        if (center_x_q8 >= bounds.x && center_y_q8 >= bounds.y && center_x_q8 < static_cast<int64_t>(bounds.x) + bounds.width &&
            center_y_q8 < static_cast<int64_t>(bounds.y) + bounds.height)
            return displays.displays[index].display_id;
    }

    return 0;
}

void advance_capture_deadline(uint64_t now_ns, uint64_t start_time_ns, uint64_t* deadline_ns) noexcept {
    constexpr uint64_t phase_offset = scheduler::scene_period_30hz_ns - preprocess_lead_ns;
    const uint64_t phase = start_time_ns + phase_offset;
    *deadline_ns =
        now_ns < phase ? phase : phase + ((now_ns - phase) / scheduler::scene_period_30hz_ns + 1U) * scheduler::scene_period_30hz_ns;
}

application::SceneSource scene_source(application::TargetSource source) noexcept {
    switch (source) {
    case application::TargetSource::pixel:
        return application::SceneSource::pixel;
    case application::TargetSource::semantic:
        return application::SceneSource::semantic;
    case application::TargetSource::grid:
        return application::SceneSource::grid;
    case application::TargetSource::fused:
        return application::SceneSource::fused;
    }
    return application::SceneSource::pixel;
}

application::TargetFilterConfig target_filter(const application::DetectorSettings& detector) noexcept {
    application::TargetFilterConfig filter{};
    filter.confidence_q16 = detector.confidence_q16;
    filter.text_confidence_q16 = detector.text_sensitivity_q16;
    filter.minimum_width_q8 = detector.minimum_width_q8;
    filter.minimum_height_q8 = detector.minimum_height_q8;
    if (detector.merge_policy == application::MergePolicy::text_first)
        filter.order = application::TargetOrderPolicy::text_first;
    else if (detector.merge_policy == application::MergePolicy::controls_first)
        filter.order = application::TargetOrderPolicy::controls_first;
    return filter;
}

scene::FusionConfig fusion_config(const application::DetectorSettings& detector, uint32_t maximum_targets) noexcept {
    scene::FusionConfig config{};
    config.maximum_targets = maximum_targets;
    config.iou_threshold_q16 = detector.duplicate_iou_q16;
    config.merge_duplicates = detector.merge_policy != application::MergePolicy::disabled;
    return config;
}

application::LabelPlacement label_placement(const application::SettingsDocument& settings) noexcept {
    const application::HintPlacement placement =
        settings.appearance.placement == application::HintPlacement::automatic ? settings.hints.placement : settings.appearance.placement;
    return static_cast<application::LabelPlacement>(placement);
}

bool q8(CGFloat value, int32_t* output) noexcept {
    const double scaled = static_cast<double>(value) * 256.0;
    if (!std::isfinite(scaled) || scaled < INT32_MIN || scaled > INT32_MAX)
        return false;
    *output = static_cast<int32_t>(std::llround(scaled));
    return true;
}

geometry::PointQ8 pointer_position() noexcept {
    CGEventRef event = CGEventCreate(nullptr);
    if (event == nullptr)
        return {};
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    geometry::PointQ8 output{};
    (void)q8(point.x, &output.x);
    (void)q8(point.y, &output.y);
    return output;
}

bool desktop_geometry(const geometry::DisplaySnapshot& snapshot, Desktop* output) noexcept {
    if (snapshot.count == 0 || output == nullptr)
        return false;
    int64_t left = INT64_MAX;
    int64_t top = INT64_MAX;
    int64_t right = INT64_MIN;
    int64_t bottom = INT64_MIN;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::RectQ8& bounds = snapshot.displays[index].desktop_bounds;
        if (!geometry::rect_valid(bounds) || bounds.x % 256 != 0 || bounds.y % 256 != 0 || bounds.width % 256 != 0 ||
            bounds.height % 256 != 0)
            return false;

        left = std::min(left, static_cast<int64_t>(bounds.x / 256));
        top = std::min(top, static_cast<int64_t>(bounds.y / 256));
        right = std::max(right, (static_cast<int64_t>(bounds.x) + bounds.width) / 256);
        bottom = std::max(bottom, (static_cast<int64_t>(bounds.y) + bounds.height) / 256);
    }
    if (left < INT32_MIN || top < INT32_MIN || right <= left || bottom <= top || right - left > UINT32_MAX || bottom - top > UINT32_MAX)
        return false;

    *output = {static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<uint32_t>(right - left),
               static_cast<uint32_t>(bottom - top), snapshot.epoch};
    return true;
}

geometry::PointQ8 desktop_center(const Desktop& desktop) noexcept {
    return {static_cast<int32_t>(static_cast<int64_t>(desktop.x) * 256 + static_cast<int64_t>(desktop.width) * 128),
            static_cast<int32_t>(static_cast<int64_t>(desktop.y) * 256 + static_cast<int64_t>(desktop.height) * 128)};
}

constexpr CoreMlComputePolicy compute_policy(application::ComputePolicy policy) noexcept {
    switch (policy) {
    case application::ComputePolicy::cpu_only:
        return CoreMlComputePolicy::cpu_only;
    case application::ComputePolicy::cpu_and_gpu:
        return CoreMlComputePolicy::cpu_and_gpu;
    case application::ComputePolicy::cpu_and_accelerator:
        return CoreMlComputePolicy::cpu_and_neural_engine;
    case application::ComputePolicy::automatic:
        return CoreMlComputePolicy::cpu_and_neural_engine;
    case application::ComputePolicy::named_device:
        return CoreMlComputePolicy::all;
    }
    return CoreMlComputePolicy::cpu_and_neural_engine;
}

static_assert(compute_policy(application::ComputePolicy::automatic) == CoreMlComputePolicy::cpu_and_neural_engine);

uint32_t required_compute_capability(application::ComputePolicy policy) noexcept {
    switch (policy) {
    case application::ComputePolicy::cpu_only:
        return SACCADE_PROVIDER_CAPABILITY_CPU;
    case application::ComputePolicy::cpu_and_gpu:
        return SACCADE_PROVIDER_CAPABILITY_GPU;
    case application::ComputePolicy::cpu_and_accelerator:
        return SACCADE_PROVIDER_CAPABILITY_ACCELERATOR;
    case application::ComputePolicy::automatic:
    case application::ComputePolicy::named_device:
        return 0;
    }
    return 0;
}

bool system_dark_theme() noexcept {
    NSAppearance* appearance = NSApp.effectiveAppearance;
    if (appearance == nil)
        appearance = NSAppearance.currentDrawingAppearance;
    const NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
}

} // namespace

DesktopPipeline::DesktopPipeline() noexcept = default;

DesktopPipeline::~DesktopPipeline() {
    (void)shutdown();
}

SaccadeResult DesktopPipeline::release_explicit_capture_frame(void* context, const SceneCaptureFrame& frame) noexcept {
    return context == nullptr ? SACCADE_ERROR_INVALID_ARGUMENT
                              : static_cast<DesktopPipeline*>(context)->explicit_capture_.release(frame);
}

SaccadeResult DesktopPipeline::set_text(SaccadeSpanU8 text) noexcept {
    return initialized_ ? runtime_.set_text(text) : SACCADE_ERROR_STATE;
}

bool DesktopPipeline::active() const noexcept {
    if (pending_command_ || topology_sync_pending_ || runtime_.active() || input_.synthetic_input_active())
        return true;
    return bridge_.initialized_ && bridge_.bridge_.busy();
}

bool DesktopPipeline::surface_qualified(bool refresh) noexcept {
    if (refresh)
        return surface_qualifier_.refresh();
    if (!basic_input_environment_available())
        return false;
    const SurfaceQualifierSnapshot& cached = surface_qualifier_.cached();
    if (cached.disposition == SurfaceDisposition::unknown)
        return surface_qualifier_.refresh();
    return cached.disposition == SurfaceDisposition::qualified;
}

SaccadeResult DesktopPipeline::fail(SaccadeResult result, DesktopPipelineStage stage) noexcept {
    if (result != SACCADE_OK) {
        last_result_ = result;
        last_stage_ = stage;
        ++stats_.failures;
    }
    return result;
}

SaccadeResult DesktopPipeline::initialize(const DesktopPipelineConfig& config) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.artifact_path == nullptr || config.model_root == nullptr || config.metallib_path == nullptr || config.settings == nullptr ||
        config.verifier.verify == nullptr || config.start_time_ns == 0 || application::validate_settings(*config.settings) != SACCADE_OK)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result = debugger_.initialized() ? debugger_.clear() : debugger_.initialize(&debugger_storage_);
    if (result != SACCADE_OK)
        return result;
    config_ = config;
    settings_ = *config.settings;
    config_.settings = &settings_;
    result = resolve_hint_language(settings_.hints, &resolved_hints_, &keyboard_layout_token_);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::keys);
    result = artifact_.initialize(config.artifact_path, config.verifier);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::artifact);
    result = provider_.initialize({config.model_root, compute_policy(config.settings->compute.policy), false, {}, config.verifier});
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::provider);
    provider_initialized_ = true;
    const SaccadeInferenceProviderDesc provider = provider_.descriptor();
    application::InferenceRuntimeConfig inference_config{};
    inference_config.provider = provider;
    inference_config.artifact = artifact_.bytes();
    inference_config.model_stable_id = artifact_.view().stable_id;
    inference_config.provider_stable_id = provider.info.stable_id;
    inference_config.device_stable_id = config.settings->compute.device_stable_id;
    inference_config.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT | SACCADE_PROVIDER_CAPABILITY_ASYNC |
                                                required_compute_capability(config.settings->compute.policy);
    inference_config.preferred_capability_bits =
        SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_ACCELERATOR | SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    inference_config.required_format_bits = SACCADE_FORMAT_BGRA8;
    inference_config.required_precision_bits = artifact_.view().precision_bits;
    inference_config.required_import_bits = SACCADE_IMPORT_IOSURFACE;
    result = inference_.initialize(inference_config);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::inference);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
        return fail(SACCADE_ERROR_NOT_FOUND, DesktopPipelineStage::capture_provider);
    metal_device_ = (__bridge void*)device;
    result = capture_provider_.initialize(metal_device_);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::capture_provider);
    result = display_collector_.refresh(&displays_);
    if (result != SACCADE_OK || displays_.snapshot().count == 0)
        return fail(result == SACCADE_OK ? SACCADE_ERROR_NOT_FOUND : result, DesktopPipelineStage::topology);
    result = captures_.initialize(&capture_provider_, 0, 0);
    if (result == SACCADE_OK)
        captures_initialized_ = true;
    if (result == SACCADE_OK)
        result = captures_.synchronize(displays_.snapshot());
    if (result == SACCADE_ERROR_PERMISSION)
        result = SACCADE_OK;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::capture_set);
    result = explicit_capture_.initialize(capture_provider_.descriptor(), &capture_provider_, read_explicit_native_frame, 0, 0);
    if (result == SACCADE_OK)
        explicit_capture_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::capture_set);
    result = initialize_bridge();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::bridge);
    result = accessibility_.initialize();
    if (result == SACCADE_OK)
        accessibility_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::accessibility);
    Desktop desktop{};
    if (!desktop_geometry(displays_.snapshot(), &desktop))
        return fail(SACCADE_ERROR_CAPACITY, DesktopPipelineStage::input);
    const geometry::PointQ8 pointer = pointer_position();
    result = input_.initialize(desktop, {this, post_with_cg_event, activate_window_public, preflight_input}, permission_epoch_, pointer.x,
                               pointer.y);
    if (result == SACCADE_OK)
        input_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    capture_permission_available_ = CGPreflightScreenCaptureAccess();
    accessibility_permission_available_ = accessibility_.permission_granted();
    input_permission_available_ = input_permission_granted();
    surface_qualifier_.invalidate();
    input_available_ = input_permission_available_ && surface_qualified(true);
    next_permission_check_ns_ = config.start_time_ns;
    const geometry::PointQ8 center = desktop_center(desktop);
    application::DesktopRuntimeConfig runtime_config{};
    runtime_config.neural.runtime = inference_.runtime();
    runtime_config.neural.session = inference_.session();
    runtime_config.neural.model_epoch = artifact_.view().stable_id;
    runtime_config.neural.session_epoch = config.start_time_ns;
    runtime_config.neural.desktop_source_id = desktop_source_id;
    runtime_config.neural.maximum_output_bytes = inference_.info().max_output_bytes;
    runtime_config.neural.maximum_targets = artifact_.view().max_targets;
    runtime_config.neural.start_time_ns = config.start_time_ns;
    runtime_config.accessibility = accessibility_.descriptor();
    runtime_config.fusion = fusion_config(config.settings->detector, artifact_.view().max_targets);
    runtime_config.scene_filter = target_filter(config.settings->detector);
    runtime_config.source = scene_source(config.settings->source);
    runtime_config.executor = {this, execute_plan};
    runtime_config.interaction =
        application::make_interaction_profile(settings_, resolved_hints_, pointer.x, pointer.y, center.x, center.y, config.start_time_ns);
    runtime_config.environment = {this, read_environment};
    runtime_config.sink = {this, forward_command, input_lease_active, neutralize_input};
    result = runtime_.initialize(runtime_config, &runtime_storage_);
    if (result == SACCADE_OK)
        runtime_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    constexpr SaccadeAgentCapabilityBits agent_capabilities = SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER |
                                                              SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_WINDOW;
    agent::ServiceConfig agent_config{
        this, acquire_agent_scene, execute_plan, read_agent_physical_state, abort_agent_input, cycle_agent_window, agent_capabilities};
    agent_config.execute_background_press = execute_agent_background_press;
    agent_config.prepare_window_activation = prepare_agent_window_activation;
    result = agent_.initialize(agent_config);
    if (result == SACCADE_OK)
        agent_initialized_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    scope_ = config.settings->scope;
    result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    result = keys_.initialize(&runtime_, {this, route_session_command});
    if (result == SACCADE_OK)
        keys_initialized_ = true;
    if (result == SACCADE_OK)
        result = keys_.replace(settings_.bindings.data(), settings_.binding_count);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::keys);
    overlay_style_ = application::resolve_overlay_style(config.settings->appearance, config.settings->flags, system_dark_theme());
    application::SettingsDocument glyph_settings = settings_;
    glyph_settings.hints = resolved_hints_;
    result = rasterize_glyph_atlas(glyph_settings, &glyph_atlases_[0]);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::overlay);
    current_glyph_atlas_ = 0;
    result = overlays_.initialize(config.metallib_path, backend::metal::PathPreference::automatic, {this, load_overlay, observe_overlay});
    if (result == SACCADE_OK)
        overlays_initialized_ = true;
    if (result == SACCADE_OK)
        result = overlays_.set_glyph_atlas(glyph_atlases_[current_glyph_atlas_].view());
    if (result == SACCADE_OK)
        result = overlays_.synchronize(displays_.snapshot());
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::overlay);
    source_ = config.settings->source;
    for (uint32_t index = 0; index < displays_.snapshot().count; ++index)
        overlay_frames_[index].display_id_ = displays_.snapshot().displays[index].display_id;
    next_capture_ns_ = config.start_time_ns;
    next_explicit_session_epoch_ = config.start_time_ns;
    next_active_scope_check_ns_ = config.start_time_ns;
    next_keyboard_layout_check_ns_ = config.start_time_ns + keyboard_layout_check_period_ns;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::apply_settings(const application::SettingsDocument& settings, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0 || application::validate_settings(settings) != SACCADE_OK)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (active())
        return SACCADE_ERROR_BUSY;
    if (settings.compute.policy != settings_.compute.policy || settings.compute.device_stable_id != settings_.compute.device_stable_id) {
        const application::SettingsDocument previous = settings_;
        DesktopPipelineConfig restart = config_;
        restart.start_time_ns = now_ns;
        SaccadeResult result = shutdown();
        if (result != SACCADE_OK)
            return result;
        restart.settings = &settings;
        result = initialize(restart);
        if (result == SACCADE_OK)
            return SACCADE_OK;
        const SaccadeResult requested_result = result;
        (void)shutdown();
        restart.settings = &previous;
        (void)initialize(restart);
        return requested_result;
    }

    const application::SettingsDocument previous = settings_;
    const application::TargetSource previous_source = source_;
    const application::TargetScope previous_scope = scope_;
    const application::HintSettings previous_hints = resolved_hints_;
    const uint64_t previous_layout_token = keyboard_layout_token_;
    const SaccadeOverlayStyle previous_style = overlay_style_;
    const uint32_t previous_glyph_atlas = current_glyph_atlas_;
    const uint32_t pending_glyph_atlas = previous_glyph_atlas == 0 ? 1U : 0U;
    application::HintSettings pending_hints{};
    uint64_t pending_layout_token = 0;
    SaccadeResult result = resolve_hint_language(settings.hints, &pending_hints, &pending_layout_token);
    if (result != SACCADE_OK)
        return result;
    application::SettingsDocument glyph_settings = settings;
    glyph_settings.hints = pending_hints;
    result = rasterize_glyph_atlas(glyph_settings, &glyph_atlases_[pending_glyph_atlas]);
    if (result != SACCADE_OK)
        return result;
    Desktop desktop{};
    if (!desktop_geometry(displays_.snapshot(), &desktop))
        return SACCADE_ERROR_STATE;
    const geometry::PointQ8 pointer = pointer_position();
    const geometry::PointQ8 center = desktop_center(desktop);
    settings_ = settings;
    resolved_hints_ = pending_hints;
    keyboard_layout_token_ = pending_layout_token;
    source_ = settings.source;
    scope_ = settings.scope;
    overlay_style_ = application::resolve_overlay_style(settings.appearance, settings.flags, system_dark_theme());
    result = runtime_.set_interaction_profile(
        application::make_interaction_profile(settings_, resolved_hints_, pointer.x, pointer.y, center.x, center.y, now_ns));
    if (result == SACCADE_OK)
        result = runtime_.set_target_filter(target_filter(settings.detector));
    if (result == SACCADE_OK)
        result = runtime_.set_fusion(fusion_config(settings.detector, artifact_.view().max_targets));
    if (result == SACCADE_OK)
        result = runtime_.set_source(scene_source(source_));
    if (result == SACCADE_OK)
        result = apply_scope();
    if (result == SACCADE_OK)
        result = keys_.replace(settings.bindings.data(), settings.binding_count);
    if (result == SACCADE_OK)
        result = overlays_.set_glyph_atlas(glyph_atlases_[pending_glyph_atlas].view());
    if (result == SACCADE_OK) {
        current_glyph_atlas_ = pending_glyph_atlas;
        next_keyboard_layout_check_ns_ = now_ns + keyboard_layout_check_period_ns;
        overlay_dirty_ = true;
        return SACCADE_OK;
    }

    settings_ = previous;
    resolved_hints_ = previous_hints;
    keyboard_layout_token_ = previous_layout_token;
    source_ = previous_source;
    scope_ = previous_scope;
    overlay_style_ = previous_style;
    current_glyph_atlas_ = previous_glyph_atlas;
    (void)runtime_.set_interaction_profile(
        application::make_interaction_profile(previous, previous_hints, pointer.x, pointer.y, center.x, center.y, now_ns));
    (void)runtime_.set_target_filter(target_filter(previous.detector));
    (void)runtime_.set_fusion(fusion_config(previous.detector, artifact_.view().max_targets));
    (void)runtime_.set_source(scene_source(previous_source));
    (void)apply_scope();
    (void)keys_.replace(previous.bindings.data(), previous.binding_count);
    return fail(result, DesktopPipelineStage::runtime);
}

SaccadeResult DesktopPipeline::refresh_hint_language(uint64_t now_ns) noexcept {
    if (now_ns < next_keyboard_layout_check_ns_)
        return SACCADE_OK;
    next_keyboard_layout_check_ns_ =
        now_ns > UINT64_MAX - keyboard_layout_check_period_ns ? UINT64_MAX : now_ns + keyboard_layout_check_period_ns;
    if (std::strcmp(settings_.hints.language.data(), "und") != 0)
        return SACCADE_OK;

    const uint64_t current = active_keyboard_layout_token();
    if (current == 0)
        return SACCADE_ERROR_BACKEND;
    if (current == keyboard_layout_token_ || runtime_.active())
        return SACCADE_OK;
    return apply_settings(settings_, now_ns);
}

SaccadeResult DesktopPipeline::initialize_bridge() noexcept {
    if (bridge_.initialized_)
        return SACCADE_OK;
    model::coreml::Contract contract{};
    SaccadeResult result = model::coreml::parse_contract(artifact_.view(), &contract);
    if (result != SACCADE_OK)
        return result;

    CoreMlImageBridgeConfig config{};
    config.runtime = inference_.runtime();
    config.metal_device = metal_device_;
    config.metallib_path = config_.metallib_path;
    config.path = backend::metal::PathPreference::automatic;
    config.input_width = artifact_.view().input_width;
    config.input_height = artifact_.view().input_height;
    config.letterbox_rgb = contract.letterbox_rgb;
    result = bridge_.bridge_.initialize(config);
    if (result != SACCADE_OK)
        return result;
    bridge_.initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::start_capture() noexcept {
    if (capture_running_)
        return SACCADE_OK;
    if (topology_sync_pending_)
        return SACCADE_ERROR_BUSY;
    if (!capture_permission_available_)
        return SACCADE_ERROR_PERMISSION;
    const SaccadeResult result = captures_.set_running(true);
    if (result != SACCADE_OK)
        return fail(result);
    capture_running_ = true;
    ++stats_.capture_starts;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::stop_capture(bool* stopped) noexcept {
    *stopped = false;
    if (!capture_running_ || config_.continuous_observation || pending_command_ || runtime_.active() || input_.synthetic_input_active())
        return SACCADE_OK;
    if (bridge_.initialized_ && bridge_.bridge_.busy())
        return SACCADE_OK;
    const SaccadeResult result = captures_.set_running(false);
    if (result != SACCADE_OK)
        return fail(result);
    capture_running_ = false;
    *stopped = true;
    ++stats_.capture_stops;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::stop_capture_immediately() noexcept {
    SaccadeResult result = SACCADE_OK;
    if (bridge_.initialized_)
        preserve_first_error(bridge_.bridge_.discard(), &result);
    if (capture_running_) {
        const SaccadeResult stopped = captures_.set_running(false);
        if (stopped == SACCADE_OK) {
            capture_running_ = false;
            ++stats_.capture_stops;
        }
        preserve_first_error(stopped, &result);
    }
    return result;
}

SaccadeResult DesktopPipeline::start_overlay() noexcept {
    if (overlay_running_)
        return overlay_dirty_ ? publish_overlays() : SACCADE_OK;
    SaccadeResult result = overlay_dirty_ ? publish_overlays() : SACCADE_OK;
    if (result != SACCADE_OK)
        return result;
    result = overlays_.start();
    if (result != SACCADE_OK)
        return fail(result);
    overlay_running_ = true;
    ++stats_.overlay_starts;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::stop_overlay() noexcept {
    if (!overlay_running_)
        return SACCADE_OK;
    const SaccadeResult result = overlays_.stop();
    if (result != SACCADE_OK)
        return fail(result);
    overlay_running_ = false;
    ++stats_.overlay_stops;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::request(application::Command command, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (topology_sync_pending_)
        return SACCADE_ERROR_BUSY;
    const SaccadeResult refreshed = refresh_permissions(now_ns);
    if (refreshed != SACCADE_OK)
        return refreshed;
    if (!input_permission_granted() || !surface_qualified(true)) {
        const SaccadeResult result = apply_input_availability(false, now_ns);
        return result == SACCADE_OK ? SACCADE_ERROR_PERMISSION : result;
    }
    if (!input_available_) {
        const SaccadeResult result = apply_input_availability(true, now_ns);
        if (result != SACCADE_OK)
            return result;
    }
    application::TargetSource requested_source{};
    if (source_for_command(command, &requested_source))
        return change_source(requested_source, now_ns);
    if (command == application::Command::scope_toggle) {
        const application::TargetScope next =
            scope_ == application::TargetScope::active_window ? application::TargetScope::desktop : application::TargetScope::active_window;
        return change_scope(next, now_ns);
    }
    application::TargetScope requested_scope{};
    if (scope_for_command(command, &requested_scope))
        return change_scope(requested_scope, now_ns);
    if (mode_command(command) && runtime_.active()) {
        application::InteractionCommandResult output{};
        SaccadeResult result = runtime_.dispatch(command, now_ns, &output);
        if (result != SACCADE_OK)
            return fail(result);
        (void)stop_overlay();
        return restart_action(now_ns);
    }
    if (command == application::Command::free_pointer) {
        if (pending_command_ || runtime_.active())
            return SACCADE_ERROR_BUSY;
        SaccadeResult result = apply_scope();
        if (result != SACCADE_OK)
            return fail(result);
        if (source_ != application::TargetSource::grid) {
            result = runtime_.set_source(application::SceneSource::grid);
            if (result != SACCADE_OK)
                return fail(result);
            source_ = application::TargetSource::grid;
        }
        return begin_grid_action(command, now_ns);
    }
    if (window_navigation_command(command))
        return navigate_window(command, now_ns);
    if (command == application::Command::window_activate) {
        if (pending_command_ || runtime_.active())
            return SACCADE_ERROR_BUSY;
        return begin_window_action(now_ns);
    }
    if (!application::command_targets_scene(command)) {
        application::InteractionCommandResult output{};
        return runtime_.dispatch(command, now_ns, &output);
    }
    if (pending_command_ || runtime_.active())
        return SACCADE_ERROR_BUSY;
    SaccadeResult source_result = runtime_.set_source(scene_source(source_));
    if (source_result != SACCADE_OK)
        return fail(source_result);
    SaccadeResult result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result);
    if (source_ == application::TargetSource::grid)
        return begin_grid_action(command, now_ns);
    result = start_capture();
    if (result != SACCADE_OK)
        return result;
    pending_ = command;
    pending_command_ = true;
    pending_deadline_ns_ = now_ns + activation_timeout_ns;
    if (pending_deadline_ns_ <= now_ns)
        return fail(SACCADE_ERROR_CAPACITY);
    next_capture_ns_ = now_ns;
    ++stats_.activations;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::begin_window_action(uint64_t now_ns) noexcept {
    SaccadeResult result = window_navigator_.collect(accessibility_.descriptor(), static_cast<uint64_t>(getpid()), &windows_);
    if (result != SACCADE_OK)
        return fail(result);
    result = runtime_.set_source(application::SceneSource::windows);
    if (result != SACCADE_OK)
        return fail(result);
    scene::WindowSceneConfig config{};
    config.frame_id = next_window_frame_id_++;
    if (next_window_frame_id_ == 0)
        ++next_window_frame_id_;
    config.model_epoch = artifact_.view().stable_id;
    config.session_epoch = config_.start_time_ns;
    config.transform_epoch = displays_.snapshot().epoch;
    config.topology_epoch = displays_.snapshot().epoch;
    config.source_id = window_source_id;
    application::SceneCoordinatorAdvance scene{};
    result = runtime_.publish_windows(config, windows_.windows.data(), windows_.count, &scene);
    if (result != SACCADE_OK)
        return fail(result);
    application::InteractionCommandResult action{};
    result = runtime_.dispatch(application::Command::window_activate, now_ns, &action);
    if (result != SACCADE_OK)
        return fail(result);
    window_scene_active_ = action.action_started;
    return action.action_started ? start_overlay() : SACCADE_OK;
}

SaccadeResult DesktopPipeline::navigate_window(application::Command command, uint64_t now_ns) noexcept {
    const bool restart = now_ns != 0 && runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    SaccadeResult result = window_navigator_.collect(accessibility_.descriptor(), static_cast<uint64_t>(getpid()), &windows_);
    if (result != SACCADE_OK)
        return fail(result);
    const uint64_t frontmost_process = frontmost_process_id();
    uint64_t current = windows_.windows[0].stable_id;
    for (uint32_t index = 0; index < windows_.count; ++index) {
        if (windows_.windows[index].process_id != frontmost_process)
            continue;
        current = windows_.windows[index].stable_id;
        break;
    }
    uint64_t selected = 0;
    if (command == application::Command::window_cycle_forward)
        result = window_navigator_.cycle(windows_, current, true, &selected);
    else if (command == application::Command::window_cycle_backward)
        result = window_navigator_.cycle(windows_, current, false, &selected);
    else if (command == application::Command::window_activate_behind)
        result = window_navigator_.behind(windows_, current, &selected);
    else {
        const application::WindowDirection direction =
            command == application::Command::window_activate_left    ? application::WindowDirection::left
            : command == application::Command::window_activate_right ? application::WindowDirection::right
            : command == application::Command::window_activate_up    ? application::WindowDirection::up
                                                                     : application::WindowDirection::down;
        result = window_navigator_.directional(windows_, current, direction, &selected);
    }
    if (result != SACCADE_OK)
        return fail(result);
    result = activate_window_public(nullptr, selected);
    if (result != SACCADE_OK)
        return fail(result);
    result = runtime_.cancel_semantic();
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::accessibility);
    if (scope_ == application::TargetScope::active_window) {
        result = apply_scope();
        if (result != SACCADE_OK)
            return fail(result);
    }
    if (!restart)
        return SACCADE_OK;
    (void)stop_overlay();
    return restart_action(now_ns);
}

SaccadeResult DesktopPipeline::resolve_scope(geometry::RectQ8* output, uint64_t* display_id, uint64_t* window_id) noexcept {
    *display_id = 0;
    *window_id = 0;
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    if (scope_ == application::TargetScope::active_window) {
        const SaccadeAccessibilityProviderDesc provider = accessibility_.descriptor();
        SaccadeWindowInfo window{};
        window.struct_size = sizeof(window);
        window.api_version = SACCADE_API_VERSION;
        const SaccadeResult result = active_window_for_process(provider, frontmost_process_id(), &window);
        if (result != SACCADE_OK || !q8(window.desktop_bounds.x, &output->x) || !q8(window.desktop_bounds.y, &output->y) ||
            !q8(window.desktop_bounds.width, &output->width) || !q8(window.desktop_bounds.height, &output->height))
            return result != SACCADE_OK ? result : SACCADE_ERROR_CAPACITY;
        *display_id = display_id_for_window(snapshot, window.desktop_bounds);
        *window_id = window.stable_id;
        return SACCADE_OK;
    }
    if (scope_ == application::TargetScope::monitor) {
        const geometry::DisplaySurface* display = nullptr;
        if (config_.settings->monitor_stable_id != 0) {
            display = displays_.find(config_.settings->monitor_stable_id);
        } else {
            const geometry::PointQ8 pointer = pointer_position();
            for (uint32_t index = 0; index < snapshot.count; ++index) {
                const geometry::RectQ8& bounds = snapshot.displays[index].desktop_bounds;
                if (pointer.x >= bounds.x && pointer.y >= bounds.y &&
                    static_cast<int64_t>(pointer.x) < static_cast<int64_t>(bounds.x) + bounds.width &&
                    static_cast<int64_t>(pointer.y) < static_cast<int64_t>(bounds.y) + bounds.height) {
                    display = &snapshot.displays[index];
                    break;
                }
            }
        }
        if (display == nullptr)
            return SACCADE_ERROR_NOT_FOUND;
        *output = display->desktop_bounds;
        *display_id = display->display_id;
        return SACCADE_OK;
    }
    Desktop desktop{};
    if (!desktop_geometry(snapshot, &desktop) || desktop.width > INT32_MAX || desktop.height > INT32_MAX || !q8(desktop.x, &output->x) ||
        !q8(desktop.y, &output->y) || !q8(static_cast<int32_t>(desktop.width), &output->width) ||
        !q8(static_cast<int32_t>(desktop.height), &output->height))
        return SACCADE_ERROR_CAPACITY;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::apply_scope() noexcept {
    uint64_t display_id = 0;
    uint64_t window_id = 0;
    SaccadeResult result = resolve_scope(&scope_rect_, &display_id, &window_id);
    if (result != SACCADE_OK)
        return result;
    scope_filter_enabled_ = scope_ != application::TargetScope::desktop;
    result = runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
    if (result != SACCADE_OK)
        return result;
    active_window_id_ = window_id;
    active_display_id_ = display_id;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::refresh_active_scope(uint64_t now_ns) noexcept {
    next_active_scope_check_ns_ = now_ns > UINT64_MAX - active_scope_check_period_ns ? UINT64_MAX : now_ns + active_scope_check_period_ns;
    if (scope_ != application::TargetScope::active_window)
        return SACCADE_OK;

    geometry::RectQ8 next_scope{};
    uint64_t next_display_id = 0;
    uint64_t next_window_id = 0;
    const SaccadeResult resolved = resolve_scope(&next_scope, &next_display_id, &next_window_id);
    if (resolved != SACCADE_OK)
        return resolved;

    const bool unchanged = next_window_id == active_window_id_ && next_display_id == active_display_id_ && next_scope.x == scope_rect_.x &&
                           next_scope.y == scope_rect_.y && next_scope.width == scope_rect_.width &&
                           next_scope.height == scope_rect_.height;
    if (unchanged)
        return SACCADE_OK;

    if (bridge_.initialized_) {
        const SaccadeResult discarded = bridge_.bridge_.discard();
        if (discarded != SACCADE_OK)
            return discarded;
    }
    const SaccadeResult scoped = runtime_.set_scope(&next_scope);
    if (scoped != SACCADE_OK)
        return scoped;

    scope_rect_ = next_scope;
    active_window_id_ = next_window_id;
    active_display_id_ = next_display_id;
    next_capture_ns_ = now_ns;
    overlay_dirty_ = true;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::publish_grid(application::SceneCoordinatorAdvance* output) noexcept {
    geometry::RectQ8 scope{};
    uint64_t display_id = 0;
    uint64_t window_id = 0;
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    SaccadeResult result = resolve_scope(&scope, &display_id, &window_id);
    if (result != SACCADE_OK)
        return result;
    scene::GridSceneConfig grid{};
    grid.scope = scope;
    grid.frame_id = next_grid_frame_id_++;
    if (next_grid_frame_id_ == 0)
        ++next_grid_frame_id_;
    grid.model_epoch = artifact_.view().stable_id;
    grid.session_epoch = config_.start_time_ns;
    grid.transform_epoch = snapshot.epoch;
    grid.topology_epoch = snapshot.epoch;
    grid.source_id = grid_source_id;
    grid.display_id = display_id;
    grid.rows = config_.settings->grid.rows;
    grid.columns = config_.settings->grid.columns;
    grid.margin_x_q8 = config_.settings->grid.margin_x_q8;
    grid.margin_y_q8 = config_.settings->grid.margin_y_q8;
    return runtime_.publish_grid(grid, output);
}

SaccadeResult DesktopPipeline::begin_grid_action(application::Command command, uint64_t now_ns) noexcept {
    application::SceneCoordinatorAdvance scene{};
    SaccadeResult result = publish_grid(&scene);
    if (result != SACCADE_OK)
        return fail(result);
    application::InteractionCommandResult action{};
    result = runtime_.dispatch(command, now_ns, &action);
    if (result != SACCADE_OK)
        return fail(result);
    return action.action_started ? start_overlay() : SACCADE_OK;
}

SaccadeResult DesktopPipeline::restart_action(uint64_t now_ns) noexcept {
    SaccadeResult result = runtime_.set_source(scene_source(source_));
    if (result != SACCADE_OK)
        return fail(result);
    window_scene_active_ = false;
    pending_ = application::Command::repeat_action;
    pending_command_ = true;
    pending_deadline_ns_ = now_ns + activation_timeout_ns;
    if (pending_deadline_ns_ <= now_ns) {
        pending_command_ = false;
        return fail(SACCADE_ERROR_CAPACITY);
    }
    if (source_ == application::TargetSource::grid) {
        pending_command_ = false;
        result = begin_grid_action(application::Command::repeat_action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    if (result != SACCADE_OK) {
        pending_command_ = false;
        return result;
    }
    next_capture_ns_ = now_ns;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::change_source(application::TargetSource source, uint64_t now_ns) noexcept {
    if (source_ == source)
        return SACCADE_OK;
    const bool restart = runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    SaccadeResult result = runtime_.set_source(scene_source(source));
    if (result != SACCADE_OK)
        return fail(result);
    source_ = source;
    (void)stop_overlay();
    if (restart)
        return restart_action(now_ns);
    if (!pending_command_)
        return SACCADE_OK;
    if (source_ == application::TargetSource::grid) {
        const application::Command action = pending_;
        pending_command_ = false;
        result = begin_grid_action(action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    next_capture_ns_ = now_ns;
    return result;
}

SaccadeResult DesktopPipeline::change_scope(application::TargetScope scope, uint64_t now_ns) noexcept {
    if (scope_ == scope)
        return SACCADE_OK;
    const bool restart = runtime_.active();
    if (restart) {
        application::InteractionCommandResult cancelled{};
        const SaccadeResult result = runtime_.dispatch(application::Command::cancel, now_ns, &cancelled);
        if (result != SACCADE_OK)
            return fail(result);
    }
    scope_ = scope;
    SaccadeResult result = apply_scope();
    if (result != SACCADE_OK)
        return fail(result);
    (void)stop_overlay();
    if (restart)
        return restart_action(now_ns);
    if (!pending_command_)
        return SACCADE_OK;
    if (source_ == application::TargetSource::grid) {
        const application::Command action = pending_;
        pending_command_ = false;
        result = begin_grid_action(action, now_ns);
        bool stopped = false;
        (void)stop_capture(&stopped);
        return result;
    }
    result = start_capture();
    next_capture_ns_ = now_ns;
    return result;
}

SaccadeResult DesktopPipeline::begin_frames(uint64_t now_ns, uint32_t* started) noexcept {
    *started = 0;
    ++stats_.capture_attempts;
    SaccadeResult injected = debugger_.consume_fault(application::DebugFaultPoint::capture);
    if (injected != SACCADE_OK)
        return fail(injected, DesktopPipelineStage::capture_set);
    injected = debugger_.consume_fault(application::DebugFaultPoint::inference);
    if (injected != SACCADE_OK)
        return fail(injected, DesktopPipelineStage::inference);
    if (bridge_.bridge_.busy()) {
        advance_capture_deadline(now_ns, config_.start_time_ns, &next_capture_ns_);
        return SACCADE_OK;
    }

    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    std::array<uint32_t, geometry::display_capacity> indices{};
    uint32_t display_count = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[index];
        if (!intersects(display.desktop_bounds, scope_rect_) ||
            ((display.flags & geometry::display_surface_mirrored) != 0 && (display.flags & geometry::display_surface_main) == 0)) {
            continue;
        }
        uint32_t insertion = display_count;
        while (insertion != 0 && snapshot.displays[indices[insertion - 1U]].display_id > display.display_id) {
            indices[insertion] = indices[insertion - 1U];
            --insertion;
        }
        indices[insertion] = index;
        ++display_count;
    }
    if (display_count == 0)
        return fail(SACCADE_ERROR_NOT_FOUND, DesktopPipelineStage::capture_set);

    std::array<SceneCaptureFrame, geometry::display_capacity> captures{};
    std::array<geometry::DisplaySurface, geometry::display_capacity> displays{};
    const bool full_refresh = !bridge_.bridge_.atlas_matches(scope_rect_, snapshot.epoch);
    uint32_t acquired_count = 0;
    for (uint32_t index = 0; index < display_count; ++index) {
        const geometry::DisplaySurface& display = snapshot.displays[indices[index]];
        SceneCaptureFrame capture{};
        const SaccadeResult acquired = captures_.acquire(display.display_id, &capture);
        if (acquired == SACCADE_OK) {
            captures[acquired_count] = capture;
            displays[acquired_count] = display;
            ++acquired_count;
            continue;
        }
        if (acquired == SACCADE_ERROR_BUSY && !full_refresh)
            continue;
        for (uint32_t release_index = 0; release_index < acquired_count; ++release_index)
            (void)captures_.release(captures[release_index]);
        if (acquired == SACCADE_ERROR_BUSY)
            return SACCADE_OK;
        return fail(acquired, DesktopPipelineStage::capture_set);
    }
    if (acquired_count == 0) {
        if (!full_refresh) {
            const SaccadeResult replayed = bridge_.bridge_.begin_cached();
            if (replayed != SACCADE_OK)
                return fail(replayed, DesktopPipelineStage::bridge);
        }
        advance_capture_deadline(now_ns, config_.start_time_ns, &next_capture_ns_);
        return SACCADE_OK;
    }

    uint64_t oldest_capture_ns = UINT64_MAX;
    for (uint32_t index = 0; index < acquired_count; ++index) {
        const uint64_t timestamp_ns = captures[index].frame.timestamp_ns;
        if (timestamp_ns != 0)
            oldest_capture_ns = std::min(oldest_capture_ns, timestamp_ns);
    }
    if (!full_refresh && oldest_capture_ns != UINT64_MAX && now_ns > oldest_capture_ns &&
        now_ns - oldest_capture_ns > maximum_capture_age_ns) {
        for (uint32_t index = 0; index < acquired_count; ++index)
            (void)captures_.release(captures[index]);
        ++stats_.stale_capture_retries;
        return SACCADE_OK;
    }

    const SaccadeResult result =
        bridge_.bridge_.begin_scope(&captures_, captures.data(), displays.data(), acquired_count, scope_rect_, desktop_source_id);
    if (result != SACCADE_OK) {
        for (uint32_t index = 0; index < acquired_count; ++index) {
            (void)captures_.release(captures[index]);
        }
        return fail(result, DesktopPipelineStage::bridge);
    }
    bridge_.scene_transform_epoch_ = snapshot.epoch;
    bridge_.source_count_ = 1;
    *started = acquired_count;
    stats_.frames_started += acquired_count;
    advance_capture_deadline(now_ns, config_.start_time_ns, &next_capture_ns_);
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::poll_frames(uint32_t* offered) noexcept {
    *offered = 0;
    if (!bridge_.initialized_ || !bridge_.bridge_.preprocessing())
        return SACCADE_OK;
    scheduler::NeuralFrame neural{};
    bool ready = false;
    SaccadeResult result = bridge_.bridge_.poll(&neural, &ready);
    if (result != SACCADE_OK)
        return fail(result);
    if (!ready)
        return SACCADE_OK;
    scheduler::DesktopNeuralFrame frame{};
    static_cast<scheduler::NeuralFrame&>(frame) = neural;
    frame.scene_transform_epoch = bridge_.scene_transform_epoch_;
    frame.source_count = bridge_.source_count_;
    result = runtime_.offer(frame);
    if (result != SACCADE_OK) {
        (void)saccade_frame_release(inference_.runtime(), frame.frame);
        frame.retire(frame.retire_context, frame.frame);
        return fail(result);
    }
    *offered = 1;
    ++stats_.frames_offered;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::request_semantic(uint64_t frame_id, uint64_t transform_epoch, uint64_t topology_epoch) noexcept {
    if (!accessibility_permission_available_)
        return SACCADE_ERROR_PERMISSION;
    if (runtime_.semantic_running())
        return SACCADE_OK;
    const SaccadeAccessibilityProviderDesc provider = accessibility_.descriptor();
    SaccadeWindowInfo window{};
    window.struct_size = sizeof(window);
    window.api_version = SACCADE_API_VERSION;
    SaccadeResult result = active_window_for_process(provider, frontmost_process_id(), &window);
    if (result != SACCADE_OK)
        return result;
    SaccadeAccessibilityQueryDesc query{};
    query.struct_size = sizeof(query);
    query.api_version = SACCADE_API_VERSION;
    query.window_id = window.stable_id;
    query.scope = window.desktop_bounds;
    query.target_capacity = artifact_.view().max_targets;
    query.session_epoch = config_.start_time_ns;
    query.transform_epoch = transform_epoch;
    query.topology_epoch = topology_epoch;
    query.frame_id = frame_id;
    result = runtime_.request_semantic(query);
    if (result == SACCADE_OK) {
        active_window_id_ = window.stable_id;
        active_display_id_ = display_id_for_window(displays_.snapshot(), window.desktop_bounds);
        ++stats_.semantic_requests;
    }
    return result;
}

SaccadeResult DesktopPipeline::publish_overlays() noexcept {
    if (!runtime_.active())
        return SACCADE_ERROR_NOT_FOUND;
    const SaccadeResult injected = debugger_.consume_fault(application::DebugFaultPoint::overlay);
    if (injected != SACCADE_OK)
        return fail(injected, DesktopPipelineStage::overlay);
    const geometry::DisplaySnapshot& snapshot = displays_.snapshot();
    const uint32_t current = published_overlay_arena_.load(std::memory_order_acquire);
    const uint32_t next = current == 0 ? 1U : 0U;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        if (overlay_frames_[index].reading_[next].load(std::memory_order_acquire)) {
            ++stats_.overlay_busy;
            return SACCADE_OK;
        }
    }
    size_t offset = 0;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        OverlayFrameSlot& slot = overlay_frames_[index];
        SaccadeResult result = geometry::make_desktop_to_surface_transform(snapshot.displays[index], snapshot.epoch, &overlay_transform_);
        if (result != SACCADE_OK)
            return result;
        application::OverlayComposeConfig config{};
        config.display_id = snapshot.displays[index].display_id;
        config.transform_epoch = snapshot.epoch;
        config.desktop_to_surface = &overlay_transform_;
        config.styles = &overlay_style_;
        const overlay::GlyphAtlasStorage& glyph_atlas = glyph_atlases_[current_glyph_atlas_];
        config.glyph_symbols = glyph_atlas.symbols.data();
        config.style_count = 1;
        config.glyph_symbol_count = glyph_atlas.glyph_count;
        config.placement = label_placement(settings_);
        config.role_styles.fill(0);
        application::OverlayComposeResult composed{};
        const size_t remaining = overlay_arenas_[next].size() - offset;
        result = runtime_.compose_overlay(config, &overlay_workspace_, {overlay_arenas_[next].data() + offset, remaining}, &composed);
        if (result != SACCADE_OK)
            return result;
        const auto* header = reinterpret_cast<const SaccadeOverlayPacketHeader*>(overlay_arenas_[next].data() + offset);
        slot.offsets_[next] = offset;
        slot.byte_sizes_[next] = composed.byte_size;
        slot.scene_epochs_[next] = header->scene_epoch;
        slot.transform_epochs_[next] = header->transform_epoch;
        slot.active_target_indices_[next] = composed.active_target_index;
        offset += composed.byte_size;
        ++stats_.overlay_publications;
    }
    published_overlay_arena_.store(next, std::memory_order_release);
    const bool animated = (overlay_style_.flags & SACCADE_OVERLAY_STYLE_ANIMATED) != 0;
    const SaccadeResult requested = overlays_.request_present(animated ? 16U : 1U, animated);
    if (requested != SACCADE_OK)
        return requested;
    overlay_dirty_ = false;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::advance(uint64_t now_ns, DesktopPipelineAdvance* output) noexcept {
    if (!initialized_ || output == nullptr || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    ++stats_.advances;
    const SaccadeResult drained_agent_action = drain_cancelled_agent_actions();
    if (drained_agent_action != SACCADE_OK && drained_agent_action != SACCADE_ERROR_BUSY)
        return fail(drained_agent_action, DesktopPipelineStage::accessibility);
    if (topology_sync_pending_) {
        const SaccadeResult synchronized = synchronize_pending_topology(now_ns);
        if (synchronized == SACCADE_ERROR_BUSY)
            return SACCADE_OK;
        if (synchronized != SACCADE_OK)
            return fail(synchronized, DesktopPipelineStage::topology);
    }
    const SaccadeResult language = refresh_hint_language(now_ns);
    if (language != SACCADE_OK)
        return fail(language, DesktopPipelineStage::keys);
    if (now_ns >= next_permission_check_ns_) {
        const SaccadeResult refreshed = refresh_permissions(now_ns);
        if (refreshed != SACCADE_OK)
            return refreshed;
    }
    if (now_ns >= next_active_scope_check_ns_) {
        const SaccadeResult refreshed = refresh_active_scope(now_ns);
        if (refreshed != SACCADE_OK && refreshed != SACCADE_ERROR_NOT_FOUND && refreshed != SACCADE_ERROR_CAPACITY) {
            return fail(refreshed, DesktopPipelineStage::accessibility);
        }
    }
    if (!input_available_ && !config_.continuous_observation)
        return SACCADE_ERROR_PERMISSION;
    if (config_.continuous_observation && !explicit_session_.active() && !capture_running_ && capture_permission_available_ &&
        basic_input_environment_available()) {
        const SaccadeResult started = start_capture();
        if (started != SACCADE_OK)
            return started;
        next_capture_ns_ = now_ns;
    }
    if (input_.synthetic_input_active() && (!basic_input_environment_available() || !input_permission_granted())) {
        const SaccadeResult lost = apply_input_availability(false, now_ns);
        return lost == SACCADE_OK ? SACCADE_ERROR_PERMISSION : lost;
    }
    if (input_.synthetic_input_active()) {
        InputExecutionResult timed{};
        const SaccadeResult advanced = input_.advance(now_ns, &timed);
        if (advanced != SACCADE_OK && advanced != SACCADE_ERROR_NOT_FOUND)
            return fail(advanced, DesktopPipelineStage::input);
    }
    if (pending_command_ && now_ns >= pending_deadline_ns_) {
        pending_command_ = false;
        bool stopped = false;
        (void)stop_capture(&stopped);
        output->became_idle = stopped;
        return fail(SACCADE_ERROR_TIMEOUT);
    }
    const bool semantic_source = source_ == application::TargetSource::semantic || source_ == application::TargetSource::fused;
    if (!explicit_session_.active() && capture_running_ && now_ns >= next_capture_ns_ &&
        !(semantic_source && runtime_.semantic_running())) {
        SaccadeResult result = begin_frames(now_ns, &output->frames_started);
        if (result != SACCADE_OK)
            return result;
    }
    SaccadeResult result = poll_frames(&output->frames_offered);
    if (result != SACCADE_OK)
        return result;
    result = debugger_.consume_fault(application::DebugFaultPoint::scene);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    result = runtime_.advance(now_ns, &output->runtime);
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION)
        return fail(result);
    if (runtime_.active() && (output->runtime.neural.scene_published || output->runtime.scene.scene_published)) {
        overlay_dirty_ = true;
    }
    if (!explicit_runtime_mode_ && semantic_source && output->runtime.neural.scene_published) {
        result = request_semantic(output->runtime.neural.frame_id, output->runtime.neural.transform_epoch,
                                  output->runtime.neural.topology_epoch);
        if (result != SACCADE_OK)
            return fail(result);
    }
    const bool scene_ready = output->runtime.scene.scene_published && (!semantic_source || output->runtime.scene.semantic_collected);
    if (pending_command_ && scene_ready) {
        application::InteractionCommandResult command{};
        result = runtime_.dispatch(pending_, now_ns, &command);
        if (result != SACCADE_OK)
            return fail(result);
        pending_command_ = false;
        output->command_started = command.action_started;
        if (command.action_started) {
            overlay_dirty_ = true;
            result = start_overlay();
            if (result != SACCADE_OK)
                return result;
        }
    } else if (overlay_running_ && runtime_.active() && overlay_dirty_) {
        result = publish_overlays();
        if (result != SACCADE_OK)
            return fail(result);
    }
    if (!runtime_.active()) {
        if (window_scene_active_) {
            (void)runtime_.set_source(scene_source(source_));
            window_scene_active_ = false;
        }
        const SaccadeResult hidden = stop_overlay();
        if (hidden != SACCADE_OK)
            return hidden;
    }
    bool stopped = false;
    const SaccadeResult idle = stop_capture(&stopped);
    output->became_idle = stopped;
    return idle != SACCADE_OK ? idle : result;
}

bool DesktopPipeline::route_key(const application::KeyEvent& event) noexcept {
    if (!initialized_)
        return false;
    application::SessionKeyRoute route{};
    const bool handled = keys_.route(event, &route) == SACCADE_OK && route.handled;
    if (handled && runtime_.active())
        overlay_dirty_ = true;
    if (handled && route.session_ended) {
        (void)stop_overlay();
        if (window_scene_active_) {
            (void)runtime_.set_source(scene_source(source_));
            window_scene_active_ = false;
        }
        bool stopped = false;
        (void)stop_capture(&stopped);
    }
    return handled;
}

SaccadeResult DesktopPipeline::neutralize_synthetic_input() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    SaccadeResult result = cancel_agent_actions(ExplicitWindowRetirementReason::disconnected);
    const geometry::PointQ8 pointer = pointer_position();
    preserve_first_error(input_.physical_override(pointer.x, pointer.y), &result);
    if (result != SACCADE_OK)
        return fail(result);
    ++stats_.physical_inputs;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::observe_physical_input(uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result = neutralize_synthetic_input();
    if (result != SACCADE_OK)
        return result;
    result = runtime_.observe_physical_input(now_ns);
    if (result != SACCADE_OK)
        return fail(result);
    pending_command_ = false;
    if (window_scene_active_) {
        (void)runtime_.set_source(scene_source(source_));
        window_scene_active_ = false;
    }
    (void)stop_overlay();
    bool stopped = false;
    result = stop_capture(&stopped);
    return result;
}

SaccadeResult DesktopPipeline::apply_input_availability(bool available, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (input_available_ == available)
        return SACCADE_OK;
    ++permission_epoch_;
    if (permission_epoch_ == 0)
        ++permission_epoch_;
    SaccadeResult result = available ? SACCADE_OK : cancel_agent_actions(ExplicitWindowRetirementReason::permission_lost);
    preserve_first_error(input_.permission_lost(permission_epoch_), &result);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    input_available_ = available;
    ++stats_.permission_changes;
    if (available)
        return SACCADE_OK;
    result = runtime_.observe_physical_input(now_ns);
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::runtime);
    pending_command_ = false;
    if (window_scene_active_) {
        (void)runtime_.set_source(scene_source(source_));
        window_scene_active_ = false;
    }
    result = stop_overlay();
    bool stopped = false;
    if (result == SACCADE_OK)
        result = stop_capture(&stopped);
    return result;
}

SaccadeResult DesktopPipeline::set_input_available(bool available, uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!available)
        surface_qualifier_.invalidate();
    const bool qualified = available && input_permission_granted() && surface_qualified(true);
    return apply_input_availability(qualified, now_ns);
}

SaccadeResult DesktopPipeline::refresh_permissions(uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;

    next_permission_check_ns_ = now_ns > UINT64_MAX - permission_check_period_ns ? UINT64_MAX : now_ns + permission_check_period_ns;
    bool capture = CGPreflightScreenCaptureAccess();
    const bool accessibility = accessibility_.permission_granted();
    const bool input = input_permission_granted();
    const bool basic_environment = basic_input_environment_available();
    if (!basic_environment)
        surface_qualifier_.invalidate();
    bool qualified_surface = input_available_;
    if (!input || !basic_environment) {
        qualified_surface = false;
    } else if (!input_available_ || active()) {
        qualified_surface = surface_qualified(true);
    }
    const bool input_available = input && basic_environment && qualified_surface;
    if (capture && !capture_permission_available_) {
        const SaccadeResult synchronized = captures_.synchronize(displays_.snapshot());
        if (synchronized == SACCADE_ERROR_PERMISSION)
            capture = false;
        else if (synchronized != SACCADE_OK)
            return fail(synchronized, DesktopPipelineStage::capture_set);
    }
    if (capture == capture_permission_available_ && accessibility == accessibility_permission_available_ &&
        input == input_permission_available_ && input_available == input_available_) {
        return SACCADE_OK;
    }

    const bool lost = (capture_permission_available_ && !capture) || (accessibility_permission_available_ && !accessibility) ||
                      (input_permission_available_ && !input) || (input_available_ && !input_available);
    capture_permission_available_ = capture;
    accessibility_permission_available_ = accessibility;
    input_permission_available_ = input;
    ++permission_epoch_;
    if (permission_epoch_ == 0)
        ++permission_epoch_;
    ++stats_.permission_changes;

    if (!input_available)
        input_available_ = false;
    SaccadeResult result = input_.permission_lost(permission_epoch_);
    if (result == SACCADE_OK && input_available)
        input_available_ = true;
    if (result != SACCADE_OK)
        return fail(result, DesktopPipelineStage::input);
    if (!lost)
        return SACCADE_OK;

    preserve_first_error(cancel_agent_actions(ExplicitWindowRetirementReason::permission_lost), &result);
    if ((!capture || !accessibility) && explicit_session_.active() && agent_press_ticket_ == 0)
        preserve_first_error(retire_explicit_agent_scene(ExplicitWindowRetirementReason::permission_lost), &result);
    preserve_first_error(runtime_.observe_physical_input(now_ns), &result);
    pending_command_ = false;
    if (window_scene_active_) {
        preserve_first_error(runtime_.set_source(scene_source(source_)), &result);
        window_scene_active_ = false;
    }
    preserve_first_error(stop_overlay(), &result);
    preserve_first_error(stop_capture_immediately(), &result);
    return result == SACCADE_OK ? SACCADE_OK : fail(result);
}

SaccadeResult DesktopPipeline::process_agent(SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                                             SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (!agent_initialized_)
        return SACCADE_ERROR_STATE;
    const SaccadeResult drained = drain_cancelled_agent_actions();
    if (drained != SACCADE_OK)
        return drained;
    return agent_.process(request, client_capabilities, now_ns, output, output_size);
}

SaccadeResult DesktopPipeline::update_input_desktop() noexcept {
    Desktop desktop{};
    if (!desktop_geometry(displays_.snapshot(), &desktop))
        return SACCADE_ERROR_CAPACITY;
    SaccadeResult result = input_.release_all();
    if (result == SACCADE_OK)
        result = input_.update_desktop(desktop);
    return result;
}

SaccadeResult DesktopPipeline::synchronize_pending_topology(uint64_t now_ns) noexcept {
    if (!topology_sync_pending_)
        return SACCADE_OK;

    if (bridge_.initialized_ && bridge_.bridge_.output_in_use()) {
        if (now_ns == 0)
            return SACCADE_ERROR_BUSY;
        application::DesktopRuntimeAdvance discarded{};
        const SaccadeResult advanced = runtime_.advance(now_ns, &discarded);
        if (advanced != SACCADE_OK)
            return advanced;
        if (bridge_.bridge_.output_in_use())
            return SACCADE_ERROR_BUSY;
    }
    if (bridge_.initialized_ && bridge_.bridge_.preprocessing()) {
        scheduler::NeuralFrame frame{};
        bool ready = false;
        const SaccadeResult polled = bridge_.bridge_.poll(&frame, &ready);
        if (polled != SACCADE_OK)
            return polled;
        if (!ready)
            return SACCADE_ERROR_BUSY;
        const SaccadeResult released = saccade_frame_release(inference_.runtime(), frame.frame);
        if (released != SACCADE_OK)
            return released;
        frame.retire(frame.retire_context, frame.frame);
    }
    if (bridge_.initialized_ && bridge_.bridge_.busy())
        return SACCADE_ERROR_BUSY;

    if (capture_running_) {
        const SaccadeResult stopped = captures_.set_running(false);
        if (stopped != SACCADE_OK)
            return stopped;
        capture_running_ = false;
        ++stats_.capture_stops;
    }

    SaccadeResult result = capture_permission_available_ ? captures_.synchronize(displays_.snapshot()) : SACCADE_OK;
    if (result == SACCADE_ERROR_PERMISSION) {
        capture_permission_available_ = false;
        result = SACCADE_OK;
    }
    if (result != SACCADE_OK)
        return result;
    result = overlays_.synchronize(displays_.snapshot());
    if (result != SACCADE_OK)
        return result;

    for (OverlayFrameSlot& slot : overlay_frames_) {
        slot.offsets_.fill(0);
        slot.byte_sizes_.fill(0);
        slot.scene_epochs_.fill(0);
        slot.transform_epochs_.fill(0);
        slot.active_target_indices_.fill(SACCADE_OVERLAY_ACTIVE_TARGET_NONE);
        slot.reading_[0].store(false, std::memory_order_relaxed);
        slot.reading_[1].store(false, std::memory_order_relaxed);
        slot.display_id_ = 0;
        slot.reading_index_ = UINT32_MAX;
    }
    published_overlay_arena_.store(UINT32_MAX, std::memory_order_release);
    for (uint32_t index = 0; index < displays_.snapshot().count; ++index) {
        overlay_frames_[index].display_id_ = displays_.snapshot().displays[index].display_id;
    }
    overlay_dirty_ = true;
    topology_sync_pending_ = false;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::refresh_topology() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    ++stats_.topology_refreshes;
    const uint64_t previous = displays_.snapshot().epoch;
    SaccadeResult result = display_collector_.refresh(&displays_);
    if (result != SACCADE_OK)
        return fail(result);
    if (displays_.snapshot().epoch != previous) {
        ++stats_.topology_changes;
        topology_sync_pending_ = true;
        pending_command_ = false;
        result = cancel_agent_actions(ExplicitWindowRetirementReason::identity_changed);
        if (runtime_.active()) {
            application::InteractionCommandResult command{};
            (void)runtime_.dispatch(application::Command::cancel, 1, &command);
        }
        preserve_first_error(runtime_.cancel_semantic(), &result);
        if (result == SACCADE_OK)
            result = stop_overlay();
        published_overlay_arena_.store(UINT32_MAX, std::memory_order_release);
        if (result == SACCADE_OK)
            result = update_input_desktop();
        if (result != SACCADE_OK)
            return fail(result, DesktopPipelineStage::topology);
    }
    const SaccadeResult synchronized = synchronize_pending_topology(0);
    if (synchronized == SACCADE_ERROR_BUSY)
        return SACCADE_OK;
    return synchronized == SACCADE_OK ? SACCADE_OK : fail(synchronized, DesktopPipelineStage::topology);
}

SaccadeResult DesktopPipeline::execute_plan(void* context, SaccadeSpanU8 plan, uint32_t permissions, uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    /* Quick early check only. The real validation runs per command in
       input::validate_execution_preflight below. Keep this a subset of it so
       the two cannot disagree. */
    if (!pipeline->input_available_ || !input_permission_granted() || !pipeline->surface_qualified(true)) {
        (void)pipeline->apply_input_availability(false, now_ns);
        return SACCADE_ERROR_PERMISSION;
    }
    const SaccadeResult injected = pipeline->debugger_.consume_fault(application::DebugFaultPoint::input);
    if (injected != SACCADE_OK)
        return pipeline->fail(injected, DesktopPipelineStage::input);
    InputExecutionResult output{};
    const SaccadeResult result = pipeline->input_.execute(plan, permissions, now_ns, &output);
    if (result == SACCADE_OK)
        ++pipeline->stats_.input_plans;
    return result;
}

SaccadeResult DesktopPipeline::preflight_input(void* context, const input::PlanView& plan, uint32_t command_index,
                                               uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    scene::PacketView scene{};
    SaccadeResult result = pipeline->runtime_.acquire_scene(&scene);
    if (result != SACCADE_OK)
        return result;
    const SaccadePhysicalInputState physical = pipeline->input_.physical_state().state();
    NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
    input::ExecutionPreflightState state{};
    state.scene = scene;
    state.focus_id = frontmost == nil ? 0 : static_cast<uint64_t>(frontmost.processIdentifier);
    state.topology_epoch = pipeline->displays_.snapshot().epoch;
    state.permission_epoch = pipeline->permission_epoch_;
    state.buttons = physical.buttons;
    state.input_available = pipeline->input_available_ && input_permission_granted();
    state.surface_secure = !pipeline->surface_qualified(false);
    state.target_window_available = target_window_available(plan.header->window_id);
    state.validate_active_window = command_index < plan.header->command_count && plan.header->window_id != 0 &&
                                   plan.commands[command_index].kind != SACCADE_INPUT_COMMAND_WINDOW_ACTIVATE;
    if (state.validate_active_window) {
        SaccadeWindowInfo window{};
        result = active_window_for_process(pipeline->accessibility_.descriptor(), state.focus_id, &window);
        if (result != SACCADE_OK)
            return result;
        state.window_id = window.stable_id;
    }
    if (command_index < plan.header->command_count && (plan.commands[command_index].flags & SACCADE_INPUT_COMMAND_ABSOLUTE) != 0) {
        state.target_point_secure =
            qualify_action_point(plan.commands[command_index].x_q8, plan.commands[command_index].y_q8) != ActionPointDisposition::qualified;
    }
    state.validate_initial_buttons = command_index == 0;
    return input::validate_execution_preflight(plan, state, now_ns);
}

SaccadeResult DesktopPipeline::read_environment(void* context, application::InteractionState* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    output->permission_epoch = pipeline->permission_epoch_;
    NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (!pipeline->input_available_ || !pipeline->surface_qualified(false))
        return SACCADE_ERROR_PERMISSION;
    output->focus_id = app == nil ? 0 : static_cast<uint64_t>(app.processIdentifier);
    if (output->focus_id == 0)
        return SACCADE_ERROR_PERMISSION;
    SaccadeWindowInfo window{};
    if (active_window_for_process(pipeline->accessibility_.descriptor(), output->focus_id, &window) != SACCADE_OK ||
        !q8(window.desktop_bounds.x, &output->window_bounds.x) || !q8(window.desktop_bounds.y, &output->window_bounds.y) ||
        !q8(window.desktop_bounds.width, &output->window_bounds.width) || !q8(window.desktop_bounds.height, &output->window_bounds.height))
        return SACCADE_ERROR_NOT_FOUND;
    output->window_id = window.stable_id;
    output->display_id = display_id_for_window(pipeline->displays_.snapshot(), window.desktop_bounds);
    if (input_permission_granted()) {
        output->permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT;
    }
    if (pipeline->accessibility_.permission_granted())
        output->permissions |= SACCADE_INPUT_PERMISSION_WINDOW;
    const geometry::PointQ8 pointer = pointer_position();
    output->pointer_x_q8 = pointer.x;
    output->pointer_y_q8 = pointer.y;
    output->expected_buttons = pipeline->input_.physical_state().state().buttons;
    return output->permissions != 0 ? SACCADE_OK : SACCADE_ERROR_PERMISSION;
}

SaccadeResult DesktopPipeline::cancel_agent_actions(ExplicitWindowRetirementReason reason) noexcept {
    agent_.cancel_pending_requests();
    SaccadeResult result = SACCADE_OK;
    const bool activation_was_waiting = agent_activation_.waiting();
    if (agent_press_ticket_ != 0) {
        const SaccadeResult cancelled = accessibility_.cancel_press(agent_press_ticket_);
        if (cancelled != SACCADE_OK && cancelled != SACCADE_ERROR_STATE && cancelled != SACCADE_ERROR_STALE_HANDLE)
            result = cancelled;
        agent_press_abandoned_ = true;
    }
    if (activation_was_waiting || agent_press_ticket_ != 0 || explicit_session_.active()) {
        agent_action_retirement_pending_ = true;
        agent_action_retirement_reason_ = reason;
    }
    agent_activation_.cancel();
    return result;
}

SaccadeResult DesktopPipeline::drain_cancelled_agent_actions() noexcept {
    if (agent_press_abandoned_ && agent_press_ticket_ != 0) {
        AccessibilityPressStatus status{};
        const SaccadeResult polled = accessibility_.poll_press(agent_press_ticket_, &status);
        if (polled == SACCADE_ERROR_STALE_HANDLE) {
            agent_press_ticket_ = 0;
        } else if (polled != SACCADE_OK) {
            return polled;
        } else if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING) {
            return SACCADE_ERROR_BUSY;
        } else {
            agent_press_ticket_ = 0;
        }
        agent_press_request_id_ = 0;
        agent_press_request_ = {};
        agent_press_abandoned_ = false;
    }
    if (!agent_action_retirement_pending_)
        return SACCADE_OK;
    const SaccadeResult retired = retire_explicit_agent_scene(agent_action_retirement_reason_);
    if (retired != SACCADE_OK)
        return retired;
    agent_action_retirement_pending_ = false;
    agent_action_retirement_reason_ = ExplicitWindowRetirementReason::none;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::retire_explicit_agent_scene(ExplicitWindowRetirementReason reason) noexcept {
    if (!explicit_session_.active() && explicit_accessibility_ticket_ == 0 && !explicit_capture_.active() &&
        !explicit_capture_.retiring())
        return SACCADE_OK;
    const SaccadeAccessibilityProviderDesc provider = accessibility_.descriptor();
    if (explicit_accessibility_ticket_ != 0) {
        (void)provider.ops.cancel(provider.context, explicit_accessibility_ticket_);
        SaccadeAccessibilityStatus status{};
        status.struct_size = sizeof(status);
        status.api_version = SACCADE_API_VERSION;
        const SaccadeResult polled = provider.ops.poll(provider.context, explicit_accessibility_ticket_, &status);
        if (polled == SACCADE_OK && (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING))
            return SACCADE_ERROR_BUSY;
        explicit_accessibility_ticket_ = 0;
    }
    if (explicit_accessibility_key_.session_epoch != 0) {
        const SaccadeResult retired = accessibility_.retire_action_session(explicit_accessibility_key_.session_epoch);
        if (retired == SACCADE_ERROR_BUSY)
            return retired;
    }
    if (explicit_capture_.active() || explicit_capture_.retiring()) {
        const SaccadeResult retired = explicit_capture_.active() ? explicit_capture_.retire(reason)
                                                                 : explicit_capture_.drain_retirement();
        if (retired != SACCADE_OK)
            return retired;
    }
    explicit_session_.retire(reason);
    explicit_identity_ = {};
    explicit_action_token_ = {};
    explicit_accessibility_key_ = {};
    explicit_scene_size_ = 0;
    explicit_semantic_size_ = 0;
    explicit_visual_size_ = 0;
    explicit_source_mode_ = 0;
    explicit_capture_validated_ = false;
    explicit_scene_ready_ = false;
    explicit_semantic_ready_ = false;
    explicit_visual_submitted_ = false;
    explicit_visual_ready_ = false;
    if (explicit_runtime_mode_) {
        SaccadeResult restored = runtime_.set_source(scene_source(source_));
        if (restored == SACCADE_OK)
            restored = runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
        if (restored != SACCADE_OK)
            return restored;
        explicit_runtime_mode_ = false;
    }
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::acquire_explicit_agent_scene(const SaccadeAgentScope& scope,
                                                            const SaccadeAgentFreshness& freshness, scene::PacketView* scene,
                                                            application::InteractionState* output) noexcept {
    if (!capture_permission_available_)
        return SACCADE_ERROR_PERMISSION;
    const SaccadeAgentSourceMode source_mode = scope.source_mode == 0 ? SACCADE_AGENT_SOURCE_FUSED : scope.source_mode;
    if (source_mode != SACCADE_AGENT_SOURCE_PIXEL && source_mode != SACCADE_AGENT_SOURCE_SEMANTIC &&
        source_mode != SACCADE_AGENT_SOURCE_FUSED)
        return SACCADE_ERROR_UNSUPPORTED;
    if (source_mode != SACCADE_AGENT_SOURCE_PIXEL && !accessibility_.permission_granted())
        return SACCADE_ERROR_PERMISSION;

    ExplicitWindowIdentity identity{};
    SaccadeResult result = public_window_identity(scope.stable_id, &identity);
    if (result != SACCADE_OK)
        return result;
    const bool after_generation = freshness.policy == SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    if (freshness.policy != SACCADE_AGENT_FRESHNESS_LATEST_VALID && !after_generation)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (explicit_session_.active() &&
        (!same_explicit_identity(explicit_identity_, identity) || explicit_source_mode_ != source_mode)) {
        result = retire_explicit_agent_scene(ExplicitWindowRetirementReason::replaced);
        if (result != SACCADE_OK)
            return result;
    }
    if (explicit_session_.active() && after_generation) {
        if (freshness.after_generation > explicit_session_.session_epoch() ||
            (!explicit_scene_ready_ && freshness.after_generation == explicit_session_.session_epoch())) {
            return SACCADE_ERROR_BUSY;
        }
        if (freshness.after_generation == explicit_session_.session_epoch()) {
            result = retire_explicit_agent_scene(ExplicitWindowRetirementReason::replaced);
            if (result != SACCADE_OK)
                return result;
        }
    }

    const SaccadeAccessibilityProviderDesc provider = accessibility_.descriptor();
    if (!explicit_session_.active()) {
        uint64_t session_epoch = next_explicit_session_epoch_;
        if (session_epoch == 0)
            return SACCADE_ERROR_CAPACITY;
        if (after_generation && session_epoch <= freshness.after_generation)
            return SACCADE_ERROR_BUSY;

        const uint64_t frame_id = next_explicit_frame_id_;
        if (frame_id == 0)
            return SACCADE_ERROR_CAPACITY;

        next_explicit_session_epoch_ = session_epoch == UINT64_MAX ? 0 : session_epoch + 1;
        next_explicit_frame_id_ = frame_id == UINT64_MAX ? 0 : frame_id + 1;
        result = explicit_session_.select(identity, session_epoch);
        if (result != SACCADE_OK)
            return result;
        result = explicit_capture_.select(identity, session_epoch);
        if (result != SACCADE_OK) {
            explicit_session_.retire(ExplicitWindowRetirementReason::disconnected);
            return result;
        }
        SaccadeAccessibilityQueryDesc query{};
        query.struct_size = sizeof(query);
        query.api_version = SACCADE_API_VERSION;
        query.window_id = identity.window_id;
        query.scope = identity.bounds;
        query.target_capacity = artifact_.view().max_targets;
        query.session_epoch = session_epoch;
        query.transform_epoch = displays_.snapshot().epoch;
        query.topology_epoch = displays_.snapshot().epoch;
        query.frame_id = frame_id;
        if (source_mode != SACCADE_AGENT_SOURCE_PIXEL) {
            result = provider.ops.request(provider.context, &query, &explicit_accessibility_ticket_);
            if (result != SACCADE_OK) {
                (void)explicit_capture_.retire(ExplicitWindowRetirementReason::disconnected);
                explicit_session_.retire(ExplicitWindowRetirementReason::disconnected);
                return result;
            }
        }
        explicit_identity_ = identity;
        explicit_source_mode_ = source_mode;
        explicit_accessibility_key_ = {session_epoch, frame_id, query.transform_epoch, query.topology_epoch, identity.process_id,
                                       identity.window_id};
        explicit_capture_validated_ = false;
        explicit_scene_ready_ = false;
        explicit_scene_size_ = 0;
        explicit_semantic_size_ = 0;
        explicit_visual_size_ = 0;
        explicit_semantic_ready_ = source_mode == SACCADE_AGENT_SOURCE_PIXEL;
        explicit_visual_submitted_ = false;
        explicit_visual_ready_ = source_mode == SACCADE_AGENT_SOURCE_SEMANTIC;
        return SACCADE_ERROR_BUSY;
    }

    result = explicit_capture_.synchronize(identity);
    if (result != SACCADE_OK) {
        const SaccadeResult identity_result = result;
        const SaccadeResult retired = retire_explicit_agent_scene(
            result == SACCADE_ERROR_NOT_FOUND ? ExplicitWindowRetirementReason::disappeared
                                              : ExplicitWindowRetirementReason::identity_changed);
        return retired == SACCADE_OK ? identity_result : retired;
    }
    const bool visual_required = source_mode == SACCADE_AGENT_SOURCE_PIXEL || source_mode == SACCADE_AGENT_SOURCE_FUSED;
    if (!explicit_capture_validated_) {
        SceneCaptureFrame frame{};
        result = explicit_capture_.acquire(&frame);
        if (result == SACCADE_OK && visual_required) {
            if (runtime_.active() || bridge_.bridge_.busy()) {
                (void)explicit_capture_.release(frame);
                return SACCADE_ERROR_BUSY;
            }
            geometry::RectQ8 window_bounds{};
            if (!q8(identity.bounds.x, &window_bounds.x) || !q8(identity.bounds.y, &window_bounds.y) ||
                !q8(identity.bounds.width, &window_bounds.width) || !q8(identity.bounds.height, &window_bounds.height)) {
                (void)explicit_capture_.release(frame);
                return SACCADE_ERROR_CAPACITY;
            }
            scene::PacketView previous{};
            explicit_runtime_previous_scene_epoch_ =
                runtime_.acquire_scene(&previous) == SACCADE_OK ? previous.header->scene_epoch : 0;
            result = runtime_.set_source(application::SceneSource::pixel);
            if (result == SACCADE_OK)
                result = runtime_.set_scope(&window_bounds);
            if (result != SACCADE_OK) {
                (void)explicit_capture_.release(frame);
                const SaccadeResult configure_result = result;
                SaccadeResult restored = runtime_.set_source(scene_source(source_));
                if (restored == SACCADE_OK)
                    restored = runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
                return restored == SACCADE_OK ? configure_result : restored;
            }
            explicit_runtime_mode_ = true;
            frame.release_context = this;
            frame.release = release_explicit_capture_frame;
            geometry::DisplaySurface surface{};
            surface.display_id = identity.window_id;
            surface.desktop_bounds = window_bounds;
            surface.work_bounds = window_bounds;
            surface.backing_width = frame.native.width;
            surface.backing_height = frame.native.height;
            surface.maximum_fps = 60;
            result = bridge_.bridge_.begin_scope(nullptr, &frame, &surface, 1, window_bounds, identity.window_id);
            if (result != SACCADE_OK) {
                (void)explicit_capture_.release(frame);
                const SaccadeResult begin_result = result;
                SaccadeResult restored = runtime_.set_source(scene_source(source_));
                if (restored == SACCADE_OK)
                    restored = runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
                if (restored == SACCADE_OK)
                    explicit_runtime_mode_ = false;
                return restored == SACCADE_OK ? begin_result : restored;
            }
            bridge_.scene_transform_epoch_ = explicit_accessibility_key_.transform_epoch;
            bridge_.source_count_ = 1;
            explicit_visual_submitted_ = true;
            explicit_capture_validated_ = true;
        } else if (result == SACCADE_OK) {
            result = explicit_capture_.release(frame);
            if (result == SACCADE_OK)
                explicit_capture_validated_ = true;
        }
        if (result != SACCADE_OK && result != SACCADE_ERROR_BUSY)
            return result;
    }

    if (explicit_visual_submitted_ && !explicit_visual_ready_) {
        scene::PacketView visual{};
        result = runtime_.acquire_scene(&visual);
        if (result == SACCADE_OK && visual.header->scene_epoch > explicit_runtime_previous_scene_epoch_ &&
            visual.header->topology_epoch == explicit_session_.session_epoch()) {
            if (visual.byte_size > explicit_visual_bytes_.size())
                return SACCADE_ERROR_CAPACITY;
            std::memcpy(explicit_visual_bytes_.data(), visual.header, visual.byte_size);
            explicit_visual_size_ = visual.byte_size;
            explicit_visual_ready_ = true;
            result = runtime_.set_source(scene_source(source_));
            if (result == SACCADE_OK)
                result = runtime_.set_scope(scope_filter_enabled_ ? &scope_rect_ : nullptr);
            if (result != SACCADE_OK)
                return result;
            explicit_runtime_mode_ = false;
        } else if (result != SACCADE_OK && result != SACCADE_ERROR_NOT_FOUND) {
            return result;
        }
    }

    if (!explicit_semantic_ready_) {
        SaccadeAccessibilityStatus status{};
        status.struct_size = sizeof(status);
        status.api_version = SACCADE_API_VERSION;
        result = provider.ops.poll(provider.context, explicit_accessibility_ticket_, &status);
        if (result != SACCADE_OK) {
            explicit_accessibility_ticket_ = 0;
            (void)retire_explicit_agent_scene(ExplicitWindowRetirementReason::disconnected);
            return result;
        }
        if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING)
            return SACCADE_ERROR_BUSY;
        if (status.state != SACCADE_TICKET_COMPLETE || status.result != SACCADE_OK || status.snapshot == 0) {
            const SaccadeResult semantic_result = status.result == SACCADE_OK ? SACCADE_ERROR_BACKEND : status.result;
            explicit_accessibility_ticket_ = 0;
            (void)retire_explicit_agent_scene(ExplicitWindowRetirementReason::disconnected);
            return semantic_result;
        }
        size_t required = 0;
        result = provider.ops.collect(provider.context, status.snapshot,
                                      {explicit_semantic_bytes_.data(), explicit_semantic_bytes_.size()}, &required);
        if (result == SACCADE_OK)
            result = accessibility_.promote_action_generation(explicit_accessibility_key_);
        const SaccadeResult released = provider.ops.release(provider.context, status.snapshot);
        explicit_accessibility_ticket_ = 0;
        if (result == SACCADE_OK)
            result = released;
        if (result != SACCADE_OK) {
            (void)retire_explicit_agent_scene(ExplicitWindowRetirementReason::disconnected);
            return result;
        }
        scene::PacketView collected{};
        result = scene::validate_packet({explicit_semantic_bytes_.data(), required}, &collected);
        if (result != SACCADE_OK)
            return result;
        explicit_semantic_size_ = required;
        explicit_semantic_ready_ = true;
    }

    if (!explicit_scene_ready_ && explicit_semantic_ready_ && explicit_visual_ready_) {
        const uint64_t display_id = display_id_for_window(displays_.snapshot(), identity.bounds);
        const uint64_t scene_epoch = explicit_session_.session_epoch();
        const auto stamp = [&](std::array<uint8_t, scene::target_packet_max_bytes>& bytes, size_t size) noexcept -> SaccadeResult {
            if (size == 0)
                return SACCADE_ERROR_INVALID_ARGUMENT;
            scene::PacketView packet{};
            SaccadeResult stamped = scene::validate_packet({bytes.data(), size}, &packet);
            if (stamped != SACCADE_OK)
                return stamped;
            auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(bytes.data());
            auto* targets = reinterpret_cast<SaccadeTargetRecord*>(bytes.data() + header->targets_offset);
            header->scene_epoch = scene_epoch;
            header->frame_id = explicit_accessibility_key_.frame_id;
            header->model_epoch = artifact_.view().stable_id;
            header->session_epoch = explicit_accessibility_key_.session_epoch;
            header->transform_epoch = explicit_accessibility_key_.transform_epoch;
            header->topology_epoch = explicit_accessibility_key_.topology_epoch;
            header->source_id = identity.window_id;
            for (uint32_t index = 0; index < header->target_count; ++index) {
                if (targets[index].window_id != 0 && targets[index].window_id != identity.window_id)
                    return SACCADE_ERROR_STALE_HANDLE;
                targets[index].window_id = identity.window_id;
                targets[index].display_id = display_id;
            }
            return SACCADE_OK;
        };
        if (source_mode != SACCADE_AGENT_SOURCE_PIXEL) {
            result = stamp(explicit_semantic_bytes_, explicit_semantic_size_);
            if (result != SACCADE_OK)
                return result;
        }
        if (visual_required) {
            result = stamp(explicit_visual_bytes_, explicit_visual_size_);
            if (result != SACCADE_OK)
                return result;
        }
        if (source_mode == SACCADE_AGENT_SOURCE_FUSED) {
            std::array<scene::PacketView, 2> packets{};
            result = scene::validate_packet({explicit_semantic_bytes_.data(), explicit_semantic_size_}, &packets[0]);
            if (result == SACCADE_OK)
                result = scene::validate_packet({explicit_visual_bytes_.data(), explicit_visual_size_}, &packets[1]);
            if (result != SACCADE_OK)
                return result;
            scene::FusionEpochs epochs{};
            epochs.scene_epoch = scene_epoch;
            epochs.frame_id = explicit_accessibility_key_.frame_id;
            epochs.capture_time_ns = packets[1].header->capture_time_ns;
            epochs.model_epoch = artifact_.view().stable_id;
            epochs.session_epoch = explicit_accessibility_key_.session_epoch;
            epochs.transform_epoch = explicit_accessibility_key_.transform_epoch;
            epochs.topology_epoch = explicit_accessibility_key_.topology_epoch;
            epochs.source_id = identity.window_id;
            scene::FusionStats fusion_stats{};
            result = scene::fuse(packets.data(), static_cast<uint32_t>(packets.size()),
                                 fusion_config(settings_.detector, artifact_.view().max_targets), epochs,
                                 &explicit_fusion_workspace_, {explicit_scene_bytes_.data(), explicit_scene_bytes_.size()},
                                 &explicit_scene_size_, &fusion_stats);
            if (result != SACCADE_OK)
                return result;
        } else {
            const auto& source = source_mode == SACCADE_AGENT_SOURCE_PIXEL ? explicit_visual_bytes_ : explicit_semantic_bytes_;
            const size_t source_size = source_mode == SACCADE_AGENT_SOURCE_PIXEL ? explicit_visual_size_ : explicit_semantic_size_;
            std::memcpy(explicit_scene_bytes_.data(), source.data(), source_size);
            explicit_scene_size_ = source_size;
        }
        const ExplicitWindowSceneGeneration generation{
            scene_epoch,
            explicit_accessibility_key_.frame_id,
            explicit_accessibility_key_.transform_epoch,
            explicit_accessibility_key_.topology_epoch,
            permission_epoch_,
        };
        result = explicit_session_.publish(identity, generation, &explicit_action_token_);
        if (result != SACCADE_OK)
            return result;
        explicit_scene_ready_ = true;
    }
    if (!explicit_capture_validated_ || !explicit_scene_ready_)
        return SACCADE_ERROR_BUSY;

    result = public_window_identity(scope.stable_id, &identity);
    if (result != SACCADE_OK || explicit_session_.validate(identity, explicit_action_token_) != SACCADE_OK)
        return SACCADE_ERROR_STALE_HANDLE;
    result = scene::validate_packet({explicit_scene_bytes_.data(), explicit_scene_size_}, scene);
    if (result != SACCADE_OK)
        return result;
    *output = {};
    output->scene_epoch = scene->header->scene_epoch;
    output->transform_epoch = scene->header->transform_epoch;
    output->topology_epoch = scene->header->topology_epoch;
    output->permission_epoch = permission_epoch_;
    output->process_id = identity.process_id;
    output->foreground_process_id = frontmost_process_id();
    output->focus_id = output->foreground_process_id;
    output->window_id = identity.window_id;
    output->display_id = display_id_for_window(displays_.snapshot(), identity.bounds);
    output->scene_flags = interaction::interaction_scene_explicit_window;
    output->permissions = accessibility_.permission_granted() ? SACCADE_INPUT_PERMISSION_WINDOW : 0;
    if (input_available_ && input_permission_granted())
        output->permissions |= SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT;
    if (!q8(identity.bounds.x, &output->window_bounds.x) || !q8(identity.bounds.y, &output->window_bounds.y) ||
        !q8(identity.bounds.width, &output->window_bounds.width) || !q8(identity.bounds.height, &output->window_bounds.height))
        return SACCADE_ERROR_CAPACITY;
    const geometry::PointQ8 pointer = pointer_position();
    output->pointer_x_q8 = pointer.x;
    output->pointer_y_q8 = pointer.y;
    output->expected_buttons = input_available_ ? input_.physical_state().state().buttons : 0;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::acquire_agent_scene(void* context, const SaccadeAgentScope& scope,
                                                   const SaccadeAgentFreshness& freshness, scene::PacketView* scene,
                                                   application::InteractionState* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || scene == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (scope.kind == SACCADE_AGENT_SCOPE_WINDOW)
        return pipeline->acquire_explicit_agent_scene(scope, freshness, scene, output);
    if (!pipeline->capture_permission_available_ || !basic_input_environment_available())
        return SACCADE_ERROR_PERMISSION;

    SaccadeResult result = pipeline->runtime_.acquire_scene(scene);
    if (result != SACCADE_OK)
        return result;
    if (freshness.policy == SACCADE_AGENT_FRESHNESS_AFTER_GENERATION &&
        scene->header->scene_epoch <= freshness.after_generation)
        return SACCADE_ERROR_BUSY;

    *output = {};
    output->permission_epoch = pipeline->permission_epoch_;
    NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (!basic_input_environment_available())
        return SACCADE_ERROR_PERMISSION;
    output->focus_id = app == nil ? 0 : static_cast<uint64_t>(app.processIdentifier);
    if (output->focus_id == 0)
        return SACCADE_ERROR_PERMISSION;

    SaccadeWindowInfo window{};
    if (active_window_for_process(pipeline->accessibility_.descriptor(), output->focus_id, &window) != SACCADE_OK ||
        !q8(window.desktop_bounds.x, &output->window_bounds.x) || !q8(window.desktop_bounds.y, &output->window_bounds.y) ||
        !q8(window.desktop_bounds.width, &output->window_bounds.width) ||
        !q8(window.desktop_bounds.height, &output->window_bounds.height)) {
        return SACCADE_ERROR_NOT_FOUND;
    }

    output->window_id = window.stable_id;
    output->process_id = output->focus_id;
    output->foreground_process_id = output->focus_id;
    output->display_id = display_id_for_window(pipeline->displays_.snapshot(), window.desktop_bounds);
    if (pipeline->input_available_ && input_permission_granted()) {
        output->permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT;
    }
    if (pipeline->input_available_ && pipeline->accessibility_.permission_granted()) {
        output->permissions |= SACCADE_INPUT_PERMISSION_WINDOW;
    }
    const geometry::PointQ8 pointer = pointer_position();
    output->pointer_x_q8 = pointer.x;
    output->pointer_y_q8 = pointer.y;
    output->expected_buttons = pipeline->input_.physical_state().state().buttons;
    output->scene_epoch = scene->header->scene_epoch;
    output->transform_epoch = scene->header->transform_epoch;
    output->topology_epoch = scene->header->topology_epoch;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::read_agent_physical_state(void* context, SaccadeAgentPhysicalState* output) noexcept {
    if (context == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    const SaccadePhysicalInputState state = pipeline->input_.physical_state().state();
    *output = {};
    output->pointer = {state.pointer_x_q8, state.pointer_y_q8};
    output->buttons = state.buttons;
    output->modifiers = state.modifiers;
    output->active_lease_id = state.active_lease_id;
    output->permission_epoch = state.permission_epoch;
    output->physical_sequence = state.physical_sequence;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::execute_agent_background_press(void* context, uint64_t request_id,
                                                              const SaccadeAgentGeneration& generation, uint64_t session_epoch,
                                                              const SaccadeAgentTarget& target, bool dry_run,
                                                              agent::BackgroundActionExecution* execution) noexcept {
    if (context == nullptr || execution == nullptr || request_id == 0 || dry_run || session_epoch == 0 || target.target_id == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    *execution = {};
    if (pipeline->agent_press_abandoned_)
        return SACCADE_ERROR_BUSY;
    const AccessibilityGenerationKey key{session_epoch, generation.frame_id, generation.transform_epoch, generation.topology_epoch,
                                         generation.process_id, generation.window_id};
    const AccessibilityPressRequest requested{key, target.target_id,
                                              {target.bounds.x_q8, target.bounds.y_q8, target.bounds.width_q8,
                                               target.bounds.height_q8}};
    const auto same_key = [](const AccessibilityGenerationKey& left, const AccessibilityGenerationKey& right) noexcept {
        return left.session_epoch == right.session_epoch && left.frame_id == right.frame_id &&
               left.transform_epoch == right.transform_epoch && left.topology_epoch == right.topology_epoch &&
               left.process_id == right.process_id && left.window_id == right.window_id;
    };
    const auto same_request = [&same_key](const AccessibilityPressRequest& left,
                                          const AccessibilityPressRequest& right) noexcept {
        return same_key(left.generation, right.generation) && left.target_id == right.target_id &&
               left.bounds_q8.x == right.bounds_q8.x && left.bounds_q8.y == right.bounds_q8.y &&
               left.bounds_q8.width == right.bounds_q8.width && left.bounds_q8.height == right.bounds_q8.height;
    };
    if (pipeline->agent_press_ticket_ != 0) {
        const bool same_pending = pipeline->agent_press_request_id_ == request_id &&
                                  same_request(pipeline->agent_press_request_, requested);
        AccessibilityPressStatus status{};
        const SaccadeResult polled = pipeline->accessibility_.poll_press(pipeline->agent_press_ticket_, &status);
        if (polled != SACCADE_OK)
            return polled;
        if (status.state == SACCADE_TICKET_QUEUED || status.state == SACCADE_TICKET_RUNNING)
            return SACCADE_ERROR_BUSY;
        pipeline->agent_press_ticket_ = 0;
        pipeline->agent_press_request_id_ = 0;
        pipeline->agent_press_request_ = {};
        if (!same_pending) {
            if (status.attempt_count != 0)
                (void)pipeline->retire_explicit_agent_scene(ExplicitWindowRetirementReason::disconnected);
            return SACCADE_ERROR_STALE_HANDLE;
        }
        execution->result = status.result;
        execution->platform_error = status.native_error;
        execution->result_flags = SACCADE_AGENT_ACTION_RESULT_BACKGROUND_ACCESSIBILITY;
        if (status.attempt_count != 0) {
            const SaccadeResult retired = pipeline->retire_explicit_agent_scene(ExplicitWindowRetirementReason::replaced);
            if (retired != SACCADE_OK) {
                execution->result = SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED;
                execution->platform_error = retired;
            }
        }
        return SACCADE_OK;
    }
    SaccadeTicketHandle ticket = 0;
    const SaccadeResult requested_result = pipeline->accessibility_.request_press(requested, &ticket);
    if (requested_result != SACCADE_OK)
        return requested_result;
    pipeline->agent_press_request_id_ = request_id;
    pipeline->agent_press_ticket_ = ticket;
    pipeline->agent_press_request_ = requested;
    return SACCADE_ERROR_BUSY;
}

SaccadeResult DesktopPipeline::prepare_agent_window_activation(void* context, uint64_t request_id, uint64_t process_id,
                                                               uint64_t window_id, uint64_t source_generation, uint64_t deadline_ns,
                                                               bool dry_run, agent::BackgroundActionExecution* execution) noexcept {
    if (context == nullptr || execution == nullptr || request_id == 0 || process_id == 0 || window_id == 0 ||
        source_generation == 0 || deadline_ns == 0 || dry_run) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    *execution = {};
    const ExplicitWindowActivationKey key{request_id, process_id, window_id, source_generation, deadline_ns};
    const ExplicitWindowActivationAdmission admission = pipeline->agent_activation_.admit(key);
    if (admission == ExplicitWindowActivationAdmission::invalid)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (admission == ExplicitWindowActivationAdmission::busy)
        return SACCADE_ERROR_BUSY;
    if (admission == ExplicitWindowActivationAdmission::cancelled) {
        execution->result = SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED;
        execution->platform_error = SACCADE_ERROR_CANCELLED;
        return SACCADE_OK;
    }
    const auto fail_after_activation_request = [pipeline, execution](SaccadeResult failure) noexcept -> SaccadeResult {
        pipeline->agent_activation_.complete();
        execution->result = SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED;
        execution->platform_error = failure;
        return SACCADE_OK;
    };
    timespec monotonic{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &monotonic) != 0) {
        if (admission == ExplicitWindowActivationAdmission::resume)
            return fail_after_activation_request(SACCADE_ERROR_BACKEND);
        pipeline->agent_activation_.complete();
        return SACCADE_ERROR_BACKEND;
    }
    const uint64_t now_ns = static_cast<uint64_t>(monotonic.tv_sec) * UINT64_C(1'000'000'000) +
                            static_cast<uint64_t>(monotonic.tv_nsec);
    if (now_ns >= deadline_ns) {
        if (admission == ExplicitWindowActivationAdmission::resume)
            return fail_after_activation_request(SACCADE_ERROR_TIMEOUT);
        pipeline->agent_activation_.complete();
        return SACCADE_ERROR_TIMEOUT;
    }
    ExplicitWindowIdentity identity{};
    SaccadeResult result = public_window_identity(window_id, &identity);
    if (result != SACCADE_OK || identity.process_id != process_id) {
        const SaccadeResult failure = result == SACCADE_OK ? SACCADE_ERROR_STALE_HANDLE : result;
        if (admission == ExplicitWindowActivationAdmission::resume)
            return fail_after_activation_request(failure);
        pipeline->agent_activation_.complete();
        return failure;
    }
    if (admission == ExplicitWindowActivationAdmission::start) {
        scene::PacketView previous{};
        const uint64_t previous_scene_epoch =
            pipeline->runtime_.acquire_scene(&previous) == SACCADE_OK ? previous.header->scene_epoch : 0;
        result = pipeline->agent_activation_.set_previous_scene_epoch(previous_scene_epoch);
        if (result != SACCADE_OK) {
            pipeline->agent_activation_.complete();
            return result;
        }
        result = activate_window_public(nullptr, window_id);
        if (result != SACCADE_OK) {
            pipeline->agent_activation_.complete();
            return result;
        }
        return SACCADE_ERROR_BUSY;
    }
    if (frontmost_process_id() != process_id)
        return SACCADE_ERROR_BUSY;
    SaccadeWindowInfo active{};
    result = active_window_for_process(pipeline->accessibility_.descriptor(), process_id, &active);
    if (result != SACCADE_OK || active.stable_id != window_id)
        return SACCADE_ERROR_BUSY;
    scene::PacketView current{};
    result = pipeline->runtime_.acquire_scene(&current);
    if (result != SACCADE_OK || current.header->scene_epoch <= pipeline->agent_activation_.previous_scene_epoch())
        return SACCADE_ERROR_BUSY;
    result = pipeline->retire_explicit_agent_scene(ExplicitWindowRetirementReason::replaced);
    if (result != SACCADE_OK) {
        execution->result = SACCADE_AGENT_ERROR_BACKEND;
        execution->platform_error = result;
        execution->result_flags = SACCADE_AGENT_ACTION_RESULT_WINDOW_ACTIVATED;
        pipeline->agent_activation_.complete();
        return SACCADE_OK;
    }
    pipeline->agent_activation_.complete();
    execution->result = SACCADE_AGENT_OK;
    execution->result_flags = SACCADE_AGENT_ACTION_RESULT_WINDOW_ACTIVATED;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::abort_agent_input(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.release_all();
}

SaccadeResult DesktopPipeline::cycle_agent_window(void* context, bool backward) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (!pipeline->input_available_ || !pipeline->surface_qualified(true))
        return SACCADE_ERROR_PERMISSION;
    return pipeline->navigate_window(backward ? application::Command::window_cycle_backward : application::Command::window_cycle_forward,
                                     0);
}

SaccadeResult DesktopPipeline::forward_command(void* context, application::Command command, uint64_t now_ns) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    return pipeline->config_.forward_shell == nullptr ? SACCADE_ERROR_UNSUPPORTED
                                                      : pipeline->config_.forward_shell(pipeline->config_.shell_context, command, now_ns);
}

SaccadeResult DesktopPipeline::route_session_command(void* context, application::Command command, uint64_t now_ns) noexcept {
    return static_cast<DesktopPipeline*>(context)->request(command, now_ns);
}

bool DesktopPipeline::input_lease_active(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.synthetic_input_active();
}

SaccadeResult DesktopPipeline::neutralize_input(void* context) noexcept {
    return static_cast<DesktopPipeline*>(context)->input_.release_all();
}

SaccadeResult DesktopPipeline::load_overlay(void* context, uint64_t display_id, SaccadeOverlayFrameDesc* output) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    OverlayFrameSlot* slot = nullptr;
    for (OverlayFrameSlot& candidate : pipeline->overlay_frames_)
        if (candidate.display_id_ == display_id) {
            slot = &candidate;
            break;
        }
    if (slot == nullptr)
        return SACCADE_ERROR_NOT_FOUND;
    const uint32_t index = pipeline->published_overlay_arena_.load(std::memory_order_acquire);
    if (index > 1)
        return SACCADE_ERROR_NOT_FOUND;
    slot->reading_[index].store(true, std::memory_order_release);
    if (pipeline->published_overlay_arena_.load(std::memory_order_acquire) != index) {
        slot->reading_[index].store(false, std::memory_order_release);
        return SACCADE_ERROR_BUSY;
    }
    slot->reading_index_ = index;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = SACCADE_API_VERSION;
    output->scene_epoch = slot->scene_epochs_[index];
    output->transform_epoch = slot->transform_epochs_[index];
    output->packet = {pipeline->overlay_arenas_[index].data() + slot->offsets_[index], slot->byte_sizes_[index]};
    const uint32_t active_target_index = slot->active_target_indices_[index];
    if (active_target_index != SACCADE_OVERLAY_ACTIVE_TARGET_NONE) {
        output->flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
        output->active_target_index = active_target_index;
    }
    return SACCADE_OK;
}

void DesktopPipeline::observe_overlay(void* context, uint64_t display_id, SaccadeResult result,
                                      const backend::metal::Submission*) noexcept {
    auto* pipeline = static_cast<DesktopPipeline*>(context);
    if (pipeline == nullptr)
        return;
    for (OverlayFrameSlot& slot : pipeline->overlay_frames_) {
        if (slot.display_id_ != display_id)
            continue;
        if (slot.reading_index_ <= 1) {
            slot.reading_[slot.reading_index_].store(false, std::memory_order_release);
            slot.reading_index_ = UINT32_MAX;
        }
        break;
    }
    if (result != SACCADE_OK && result != SACCADE_ERROR_BUSY && result != SACCADE_ERROR_NOT_FOUND)
        (void)pipeline->fail(result);
}

SaccadeResult DesktopPipeline::read_diagnostics(DesktopPipelineDiagnostics* output) const noexcept {
    if (!initialized_ || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    output->pipeline = stats_;
    output->runtime = runtime_.diagnostics();
    SaccadeResult result = captures_.read_stats(&output->capture);
    if (result == SACCADE_OK)
        result = overlays_.read_stats(&output->overlay_set);
    output->inference_memory.struct_size = sizeof(output->inference_memory);
    output->inference_memory.api_version = SACCADE_API_VERSION;
    if (result == SACCADE_OK)
        result = saccade_inference_memory_stats(inference_.runtime(), inference_.session(), &output->inference_memory);
    output->preprocess_memory.struct_size = sizeof(output->preprocess_memory);
    output->preprocess_memory.api_version = SACCADE_API_VERSION;
    if (result == SACCADE_OK) {
        SaccadeMemoryStats memory{};
        memory.struct_size = sizeof(memory);
        memory.api_version = SACCADE_API_VERSION;
        result = bridge_.bridge_.read_memory_stats(&memory);
        if (result == SACCADE_OK)
            add_memory(memory, &output->preprocess_memory);
    }
    if (result == SACCADE_OK)
        result = captures_.read_memory_stats(&output->capture_memory);
    if (result != SACCADE_OK)
        return result;
    output->model = inference_.info();
    output->debugger_frames_transforms = debugger_.frames_transforms();
    output->debugger_scene_fusion = debugger_.scene_fusion();
    const geometry::DisplaySnapshot& displays = displays_.snapshot();
    for (uint32_t index = 0; index < displays.count; ++index) {
        DesktopDisplayDiagnostics& display = output->displays[index];
        display.display = displays.displays[index];
        result = overlays_.read_surface_stats(display.display.display_id, &display.overlay);
        if (result == SACCADE_OK)
            result = overlays_.read_surface_memory_stats(display.display.display_id, &display.memory);
        if (result == SACCADE_OK)
            result = overlays_.read_surface_renderer_stats(display.display.display_id, &display.gpu);
        if (result != SACCADE_OK)
            return result;
        output->overlay.ticks += display.overlay.display_ticks;
        output->overlay.rendered += display.overlay.rendered_frames;
        output->overlay.presented += display.overlay.rendered_frames;
        output->overlay.no_frame += display.overlay.no_frame_ticks;
        output->overlay.busy += display.overlay.busy_frames;
        output->overlay.deadline_misses += display.overlay.deadline_misses;
        output->overlay.failures += display.overlay.failures;
        output->overlay.known_memory_bytes += display.memory.total_known_and_estimated;
    }
    output->topology_epoch = displays.epoch;
    const SurfaceQualifierSnapshot& surface = surface_qualifier_.cached();
    output->surface_epoch = surface.epoch;
    output->surface_reason_bits = surface.reason_bits;
    output->surface = surface.disposition;
    output->display_count = displays.count;
    if (capture_permission_available_)
        output->permissions |= diagnostic_capture_permission;
    if (accessibility_permission_available_)
        output->permissions |= diagnostic_accessibility_permission;
    if (input_available_ && basic_input_environment_available() && input_permission_available_ &&
        surface.disposition == SurfaceDisposition::qualified)
        output->permissions |= diagnostic_input_permission;
    output->source = source_;
    output->scope = scope_;
    output->compute = settings_.compute.policy;
    return SACCADE_OK;
}

SaccadeResult DesktopPipeline::debug_capture_scene() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;

    const geometry::DisplaySnapshot& displays = displays_.snapshot();
    std::array<application::DebuggerTransformRecord, geometry::display_capacity> transforms{};
    for (uint32_t index = 0; index < displays.count; ++index) {
        geometry::CoordinateTransform transform;
        const SaccadeResult result = geometry::make_desktop_to_surface_transform(displays.displays[index], displays.epoch, &transform);
        if (result != SACCADE_OK)
            return result;
        transforms[index] = {displays.displays[index].display_id, displays.displays[index].display_id, transform.descriptor()};
    }
    const application::DebuggerCaptureContext context{0, transforms.data(), nullptr, displays.count, 0};
    return runtime_.capture_debugger_scene(&debugger_, context);
}

SaccadeResult DesktopPipeline::debug_dry_run(uint64_t now_ns, application::DebuggerPlanView* output) noexcept {
    return initialized_ ? debugger_.dry_run_first_click(now_ns, output) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_replay(application::DebuggerPlanView* output) noexcept {
    return initialized_ ? debugger_.replay(output) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_arm_fault(application::DebugFaultPoint point, uint32_t count, SaccadeResult result) noexcept {
    return initialized_ ? debugger_.arm_fault(point, count, result) : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::debug_clear() noexcept {
    return initialized_ ? debugger_.clear() : SACCADE_ERROR_STATE;
}

SaccadeResult DesktopPipeline::shutdown() noexcept {
    SaccadeResult result = SACCADE_OK;
    preserve_first_error(cancel_agent_actions(ExplicitWindowRetirementReason::shutdown), &result);
    if (agent_press_ticket_ != 0) {
        AccessibilityPressStatus status{};
        const SaccadeResult waited = accessibility_.wait_press(agent_press_ticket_, agent_cancel_wait_ns, &status);
        preserve_first_error(waited, &result);
        if (waited == SACCADE_OK) {
            agent_press_ticket_ = 0;
            agent_press_request_id_ = 0;
            agent_press_request_ = {};
            agent_press_abandoned_ = false;
        }
    }
    if (overlay_running_)
        preserve_first_error(stop_overlay(), &result);

    if (keys_initialized_) {
        const SaccadeResult stopped = keys_.shutdown();
        if (stopped == SACCADE_OK)
            keys_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (agent_initialized_) {
        const SaccadeResult stopped = agent_.shutdown();
        if (stopped == SACCADE_OK)
            agent_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (input_initialized_) {
        const SaccadeResult stopped = input_.shutdown();
        if (stopped == SACCADE_OK)
            input_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (runtime_initialized_) {
        const SaccadeResult stopped = runtime_.shutdown();
        if (stopped == SACCADE_OK)
            runtime_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (bridge_.initialized_) {
        const SaccadeResult discarded = bridge_.bridge_.discard();
        const SaccadeResult stopped = bridge_.bridge_.shutdown();
        if (stopped == SACCADE_OK)
            bridge_.initialized_ = false;
        preserve_first_error(discarded, &result);
        preserve_first_error(stopped, &result);
    }

    if (explicit_capture_initialized_) {
        preserve_first_error(retire_explicit_agent_scene(ExplicitWindowRetirementReason::shutdown), &result);
        const SaccadeResult stopped = explicit_capture_.shutdown();
        if (stopped == SACCADE_OK)
            explicit_capture_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (accessibility_initialized_) {
        const SaccadeResult stopped = accessibility_.shutdown();
        if (stopped == SACCADE_OK)
            accessibility_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    if (captures_initialized_) {
        const SaccadeResult stopped = captures_.shutdown();
        if (stopped == SACCADE_OK)
            captures_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    preserve_first_error(inference_.shutdown(), &result);

    if (provider_initialized_) {
        const SaccadeResult stopped = provider_.shutdown();
        if (stopped == SACCADE_OK)
            provider_initialized_ = false;
        preserve_first_error(stopped, &result);
    }

    preserve_first_error(artifact_.shutdown(), &result);
    if (result != SACCADE_OK)
        return fail(result);
    metal_device_ = nullptr;
    pending_command_ = false;
    topology_sync_pending_ = false;
    capture_running_ = false;
    overlay_running_ = false;
    input_available_ = true;
    capture_permission_available_ = false;
    accessibility_permission_available_ = false;
    input_permission_available_ = false;
    agent_press_request_id_ = 0;
    agent_press_ticket_ = 0;
    agent_press_request_ = {};
    agent_activation_.complete();
    agent_press_abandoned_ = false;
    agent_action_retirement_pending_ = false;
    agent_action_retirement_reason_ = ExplicitWindowRetirementReason::none;
    permission_epoch_ = 1;
    initialized_ = false;
    config_ = {};
    settings_ = {};
    resolved_hints_ = {};
    next_keyboard_layout_check_ns_ = 0;
    keyboard_layout_token_ = 0;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos

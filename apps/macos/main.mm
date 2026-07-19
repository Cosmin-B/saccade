#include "application/binding_editor.hpp"
#include "application/debugger_layout.hpp"
#include "application/desktop_host.hpp"
#include "application/recovery_schedule.hpp"
#include "application/settings_controller.hpp"
#include "apps/model_trust.hpp"
#include "model/p256_verifier.hpp"
#include "platform/macos/agent_socket.hpp"
#include "platform/macos/desktop_pipeline.hpp"
#include "platform/macos/global_hotkeys.hpp"
#include "platform/macos/input_monitor.hpp"
#include "platform/macos/keyboard.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <mach-o/dyld.h>
#import <os/log.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <spawn.h>

extern char** environ;

using saccade::application::Command;
using saccade::application::CommandEvent;
using saccade::application::DesktopHost;
using saccade::platform::macos::AgentSocket;
using saccade::platform::macos::DesktopPipeline;
using saccade::platform::macos::GlobalHotkeys;
using saccade::platform::macos::InputMonitor;

constexpr uint64_t permission_retry_period_ns = UINT64_C(1'000'000'000);
constexpr uint64_t agent_socket_retry_period_ns = UINT64_C(50'000'000);

uint64_t timestamp_ns() noexcept {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0) return 1;
    return static_cast<uint64_t>(time.tv_sec) * UINT64_C(1'000'000'000) + static_cast<uint64_t>(time.tv_nsec);
}

NSRect debugger_rect(const saccade::application::DebuggerLayoutRect& rect, CGFloat content_height) noexcept {
    return NSMakeRect(static_cast<CGFloat>(rect.x), content_height - static_cast<CGFloat>(rect.y + rect.height),
                      static_cast<CGFloat>(rect.width), static_cast<CGFloat>(rect.height));
}

NSString* settings_path() {
    NSArray<NSURL*>* roots = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                                    inDomains:NSUserDomainMask];
    NSURL* directory = [roots.firstObject URLByAppendingPathComponent:@"Saccade" isDirectory:YES];
    if (directory == nil) return nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:directory
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil])
        return nil;
    return [[directory URLByAppendingPathComponent:@"settings.bin"] path];
}

bool load_settings(saccade::application::SettingsDocument* output) noexcept {
    NSString* path = settings_path();
    NSData* data = path == nil ? nil : [NSData dataWithContentsOfFile:path];
    if (data == nil || data.length == 0 || data.length > saccade::application::settings_encoded_capacity) return false;
    return saccade::application::decode_settings({static_cast<const uint8_t*>(data.bytes), data.length}, output) ==
           SACCADE_OK;
}

SaccadeResult save_settings(const saccade::application::SettingsDocument& settings) noexcept {
    std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
    size_t size = 0;
    const SaccadeResult encoded = saccade::application::encode_settings(settings, {bytes.data(), bytes.size()}, &size);
    if (encoded != SACCADE_OK) return encoded;
    NSString* path = settings_path();
    if (path == nil) return SACCADE_ERROR_BACKEND;
    NSData* data = [NSData dataWithBytes:bytes.data() length:size];
    return [data writeToFile:path options:NSDataWritingAtomic error:nil] ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

bool platform_permissions_ready() noexcept {
    return CGPreflightScreenCaptureAccess() && CGPreflightListenEventAccess() && CGPreflightPostEventAccess() &&
           AXIsProcessTrusted();
}

void request_platform_permissions() noexcept {
    if (!CGPreflightScreenCaptureAccess()) (void)CGRequestScreenCaptureAccess();
    if (!CGPreflightListenEventAccess()) (void)CGRequestListenEventAccess();
    if (!CGPreflightPostEventAccess()) (void)CGRequestPostEventAccess();
    if (!AXIsProcessTrusted()) {
        NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
        (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    }
}

@class SaccadeAppDelegate;

class ApplicationState final {
  public:
    SaccadeResult initialize(SaccadeAppDelegate*) noexcept;
    void shutdown() noexcept;
    void update_menu() noexcept;
    void tick(uint64_t) noexcept;
    SaccadeResult synchronize_input_monitor(uint64_t) noexcept;
    SaccadeResult initialize_agent_socket(uint64_t) noexcept;
    SaccadeResult initialize_pipeline(uint64_t) noexcept;
    SaccadeResult shutdown_pipeline() noexcept;
    void begin_pipeline_recovery(SaccadeResult, uint64_t) noexcept;

    DesktopHost host_{};
    GlobalHotkeys hotkeys_{};
    InputMonitor input_monitor_{};
    saccade::application::SettingsController settings_{};
    saccade::model::P256ArtifactVerifier verifier_{};
    DesktopPipeline pipeline_{};
    AgentSocket agent_socket_{};
    saccade::platform::macos::AgentSocketStorage agent_socket_storage_{};
    std::array<char, 4096> artifact_path_{};
    std::array<char, 4096> model_root_{};
    std::array<char, 4096> metallib_path_{};
    uint64_t next_input_monitor_retry_ns_ = 0;
    uint64_t next_agent_socket_retry_ns_ = 0;
    saccade::application::RecoverySchedule pipeline_recovery_{};
    __weak SaccadeAppDelegate* delegate_ = nil;
    SaccadeResult fault_ = SACCADE_OK;
    bool host_initialized_ = false;
    bool hotkeys_initialized_ = false;
    bool input_monitor_initialized_ = false;
    bool settings_initialized_ = false;
    bool verifier_initialized_ = false;
    bool pipeline_initialized_ = false;
    bool pipeline_cleanup_required_ = false;
    bool agent_socket_initialized_ = false;
    bool permission_attention_ = false;
    bool scene_incomplete_ = false;
    bool restart_requested_ = false;
};

static ApplicationState application;

@interface SaccadeAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) NSMenuItem* suspendItem;
@property(nonatomic, strong) NSMenuItem* faultItem;
@property(nonatomic, strong) NSTimer* runtimeTimer;
@property(nonatomic, strong) NSPanel* diagnosticsPanel;
@property(nonatomic, strong) NSScrollView* diagnosticsScroll;
@property(nonatomic, strong) NSTextView* diagnosticsText;
@property(nonatomic, strong) NSSegmentedControl* diagnosticsView;
@property(nonatomic, strong) NSArray<NSButton*>* diagnosticsActions;
@property(nonatomic, strong) NSPopUpButton* debugFault;
@property(nonatomic, strong) NSButton* debugArmFault;
@property(nonatomic, strong) NSString* debuggerOperation;
@property(nonatomic) uint32_t diagnosticsTicks;
- (void)rebuildMenu;
- (void)openSettings:(id)sender;
- (void)openDiagnostics:(id)sender;
- (void)layoutDiagnostics;
- (void)refreshDiagnostics;
- (void)changeDiagnosticsView:(id)sender;
- (void)captureDebugScene:(id)sender;
- (void)dryRunDebugPlan:(id)sender;
- (void)replayDebugPlan:(id)sender;
- (void)clearDebugCapture:(id)sender;
- (void)armDebugFault:(id)sender;
- (void)requestPermissions:(id)sender;
- (void)toggleSuspended:(id)sender;
- (void)restart:(id)sender;
- (void)quit:(id)sender;
- (void)tickRuntime:(NSTimer*)timer;
- (void)refreshTopology:(NSNotification*)notification;
- (void)inputUnavailable:(NSNotification*)notification;
- (void)inputAvailable:(NSNotification*)notification;
@end

@interface SaccadeKeyboardSelection : NSObject
@property(nonatomic, weak) NSPopUpButton* popup;
@property(nonatomic, weak) NSButton* selected;
@property(nonatomic, strong) NSArray<NSButton*>* buttons;
- (void)chooseKey:(NSButton*)sender;
- (void)selectKeyAtIndex:(NSInteger)index;
@end

@implementation SaccadeKeyboardSelection

- (void)chooseKey:(NSButton*)sender {
    [self selectKeyAtIndex:sender.tag];
}

- (void)selectKeyAtIndex:(NSInteger)index {
    NSButton* selected = nil;
    for (NSButton* button in self.buttons) {
        if (button.tag == index) {
            selected = button;
            break;
        }
    }
    if (selected == nil) return;

    self.selected.state = NSControlStateValueOff;
    selected.state = NSControlStateValueOn;
    self.selected = selected;
    [self.popup selectItemAtIndex:index];
}

@end

static NSInteger binding_key_index(uint32_t usage) noexcept;

@interface SaccadeBindingSelection : NSObject {
  @public
    const saccade::application::SettingsDocument* settings_;
}
@property(nonatomic, weak) NSPopUpButton* command;
@property(nonatomic, weak) NSButton* control;
@property(nonatomic, weak) NSButton* alt;
@property(nonatomic, weak) NSButton* shift;
@property(nonatomic, weak) NSButton* meta;
@property(nonatomic, weak) NSButton* sessionOnly;
@property(nonatomic, weak) SaccadeKeyboardSelection* keyboardSelection;
- (void)loadCommand:(id)sender;
@end

@implementation SaccadeBindingSelection

- (void)loadCommand:(id)sender {
    (void)sender;
    using namespace saccade::application;

    const Command command = static_cast<Command>(self.command.indexOfSelectedItem + 1);
    uint32_t index = UINT32_MAX;
    const bool assigned = find_binding(*settings_, command, &index) == SACCADE_OK;
    const HotkeyBinding* binding = assigned ? &settings_->bindings[index] : nullptr;
    NSInteger key_index = binding != nullptr ? binding_key_index(binding->physical_key) : 0;
    if (key_index < 0) key_index = 0;
    [self.keyboardSelection selectKeyAtIndex:key_index];

    self.control.state = binding != nullptr && (binding->modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0
                             ? NSControlStateValueOn
                             : NSControlStateValueOff;
    self.alt.state = binding != nullptr && (binding->modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
    self.shift.state = binding != nullptr && (binding->modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0
                           ? NSControlStateValueOn
                           : NSControlStateValueOff;
    self.meta.state = binding != nullptr && (binding->modifiers & SACCADE_INPUT_MODIFIER_META) != 0
                          ? NSControlStateValueOn
                          : NSControlStateValueOff;

    const bool session_available = command != Command::suspend_toggle;
    self.sessionOnly.enabled = session_available;
    self.sessionOnly.state = session_available && binding != nullptr && (binding->flags & hotkey_session_only) != 0
                                 ? NSControlStateValueOn
                                 : NSControlStateValueOff;
}

@end

SaccadeResult dispatch_command(void* context, Command command, uint64_t now_ns) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (!state->pipeline_initialized_) return state->fault_;
    if (command == Command::type_text) {
        NSString* text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
        NSData* bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
        if (bytes == nil || bytes.length == 0 || bytes.length > saccade::interaction::maximum_action_payload_bytes)
            return SACCADE_ERROR_CAPACITY;
        const SaccadeResult staged =
            state->pipeline_.set_text({static_cast<const uint8_t*>(bytes.bytes), bytes.length});
        if (staged != SACCADE_OK) return staged;
    }
    const SaccadeResult result = state->pipeline_.request(command, now_ns);
    if (result != SACCADE_OK) {
        state->fault_ = result;
        state->update_menu();
    }
    return result;
}

SaccadeResult set_suspended(void* context, bool value) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    const SaccadeResult result = state->hotkeys_.set_suspended(value);
    if (result == SACCADE_OK) state->update_menu();
    return result;
}

SaccadeResult apply_settings(void* context, const saccade::application::SettingsDocument& settings) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (state->pipeline_initialized_) {
        const SaccadeResult applied = state->pipeline_.apply_settings(settings, timestamp_ns());
        if (applied != SACCADE_OK) return applied;
    }
    const SaccadeResult replaced = state->hotkeys_.replace(settings.bindings.data(), settings.binding_count);
    if (replaced != SACCADE_OK && state->pipeline_initialized_)
        (void)state->pipeline_.apply_settings(state->settings_.current(), timestamp_ns());
    if (replaced != SACCADE_OK || !state->settings_initialized_) return replaced;
    const SaccadeResult saved = save_settings(settings);
    if (saved == SACCADE_OK) return SACCADE_OK;
    (void)state->hotkeys_.replace(state->settings_.current().bindings.data(), state->settings_.current().binding_count);
    if (state->pipeline_initialized_) (void)state->pipeline_.apply_settings(state->settings_.current(), timestamp_ns());
    return saved;
}

SaccadeResult neutralize_input(void* context) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    return state->pipeline_initialized_ ? state->pipeline_.observe_physical_input(timestamp_ns()) : SACCADE_OK;
}

void observe_input(void* context, uint64_t now_ns) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (state->pipeline_initialized_) (void)state->pipeline_.observe_physical_input(now_ns);
}

void observe_command_input(void* context, uint64_t) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (state->pipeline_initialized_) (void)state->pipeline_.neutralize_synthetic_input();
}

void observe_monitored_input(void* context, const saccade::platform::macos::PhysicalInputEvent& event) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (event.kind == saccade::platform::macos::PhysicalInputKind::modifier) {
        if (state->pipeline_initialized_) (void)state->pipeline_.neutralize_synthetic_input();
    } else if (state->host_initialized_) {
        state->host_.observe_physical_input(event.timestamp_ns);
    }
}

bool route_key(void* context, const saccade::application::KeyEvent& event) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    if (state->pipeline_initialized_ && state->pipeline_.route_key(event)) return true;
    return state->hotkeys_initialized_ &&
           state->hotkeys_.dispatch_physical(event.physical_key, event.modifiers, event.timestamp_ns) == SACCADE_OK;
}

SaccadeResult process_agent(void* context, SaccadeSpanU8 request, SaccadeAgentCapabilityBits client_capabilities,
                            uint64_t now_ns, SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    return state->pipeline_initialized_
               ? state->pipeline_.process_agent(request, client_capabilities, now_ns, output, output_size)
               : SACCADE_ERROR_STATE;
}

bool append_component(std::array<char, 4096>* path, const char* root, const char* leaf) noexcept {
    const size_t root_size = std::strlen(root);
    const size_t leaf_size = std::strlen(leaf);
    if (root_size + leaf_size + 2U > path->size()) return false;
    std::memcpy(path->data(), root, root_size);
    path->at(root_size) = '/';
    std::memcpy(path->data() + root_size + 1U, leaf, leaf_size + 1U);
    return true;
}

SaccadeResult open_settings(void* context) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    [state->delegate_ openSettings:nil];
    return SACCADE_OK;
}

SaccadeResult launch_replacement() noexcept {
    std::array<char, 4096> executable{};
    uint32_t size = static_cast<uint32_t>(executable.size());
    if (_NSGetExecutablePath(executable.data(), &size) != 0) return SACCADE_ERROR_CAPACITY;
    char* arguments[] = {executable.data(), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, executable.data(), nullptr, nullptr, arguments, environ);
    return spawned == 0 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

SaccadeResult restart_application(void* context) noexcept {
    auto* state = static_cast<ApplicationState*>(context);
    state->restart_requested_ = true;
    [NSApp terminate:nil];
    return SACCADE_OK;
}

SaccadeResult quit_application(void*) noexcept {
    [NSApp terminate:nil];
    return SACCADE_OK;
}

SaccadeResult ApplicationState::initialize_agent_socket(uint64_t now_ns) noexcept {
    if (!pipeline_initialized_ || agent_socket_initialized_ || now_ns == 0) return SACCADE_ERROR_STATE;

    next_agent_socket_retry_ns_ =
        now_ns > UINT64_MAX - agent_socket_retry_period_ns ? UINT64_MAX : now_ns + agent_socket_retry_period_ns;
    constexpr SaccadeAgentCapabilityBits capabilities =
        SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER | SACCADE_AGENT_CAPABILITY_KEYBOARD |
        SACCADE_AGENT_CAPABILITY_WINDOW;
    const SaccadeResult result = agent_socket_.initialize(
        {this, process_agent, neutralize_input, nullptr, capabilities}, &agent_socket_storage_);
    if (result == SACCADE_OK) {
        agent_socket_initialized_ = true;
        next_agent_socket_retry_ns_ = 0;
    }
    return result;
}

SaccadeResult ApplicationState::initialize_pipeline(uint64_t now_ns) noexcept {
    if (now_ns == 0 || verifier_initialized_ || pipeline_initialized_ || pipeline_cleanup_required_ ||
        agent_socket_initialized_)
        return SACCADE_ERROR_STATE;

    SaccadeResult result = verifier_.initialize(saccade::apps::model_trust::public_key);
    if (result == SACCADE_OK) verifier_initialized_ = true;
    if (result == SACCADE_OK) {
        result = pipeline_.initialize({artifact_path_.data(), model_root_.data(), metallib_path_.data(),
                                       &settings_.current(), verifier_.descriptor(), this, nullptr, now_ns, true});
        if (result == SACCADE_OK) pipeline_initialized_ = true;
    }
    if (result == SACCADE_OK) {
        const SaccadeResult agent_result = initialize_agent_socket(now_ns);
        if (agent_result != SACCADE_ERROR_ALREADY_EXISTS) result = agent_result;
    }
    if (result == SACCADE_OK) return SACCADE_OK;

    SaccadeResult cleanup = SACCADE_OK;
    if (agent_socket_initialized_) {
        const SaccadeResult stopped = agent_socket_.shutdown();
        if (stopped == SACCADE_OK)
            agent_socket_initialized_ = false;
        else
            cleanup = stopped;
    }
    const SaccadeResult pipeline_stopped = pipeline_.shutdown();
    pipeline_initialized_ = false;
    if (pipeline_stopped == SACCADE_OK)
        pipeline_cleanup_required_ = false;
    else {
        pipeline_cleanup_required_ = true;
        if (cleanup == SACCADE_OK) cleanup = pipeline_stopped;
    }
    if (verifier_initialized_) {
        const SaccadeResult stopped = verifier_.shutdown();
        if (stopped == SACCADE_OK)
            verifier_initialized_ = false;
        else if (cleanup == SACCADE_OK)
            cleanup = stopped;
    }
    return cleanup == SACCADE_OK ? result : SACCADE_ERROR_STATE;
}

SaccadeResult ApplicationState::shutdown_pipeline() noexcept {
    SaccadeResult result = SACCADE_OK;
    next_agent_socket_retry_ns_ = 0;
    if (agent_socket_initialized_) {
        const SaccadeResult stopped = agent_socket_.shutdown();
        if (stopped == SACCADE_OK)
            agent_socket_initialized_ = false;
        else
            result = stopped;
    }
    if (pipeline_initialized_ || pipeline_cleanup_required_) {
        const SaccadeResult stopped = pipeline_.shutdown();
        if (stopped == SACCADE_OK) {
            pipeline_initialized_ = false;
            pipeline_cleanup_required_ = false;
        } else if (result == SACCADE_OK) {
            result = stopped;
        }
    }
    if (verifier_initialized_) {
        const SaccadeResult stopped = verifier_.shutdown();
        if (stopped == SACCADE_OK)
            verifier_initialized_ = false;
        else if (result == SACCADE_OK)
            result = stopped;
    }
    return result;
}

void ApplicationState::begin_pipeline_recovery(SaccadeResult failure, uint64_t now_ns) noexcept {
    fault_ = failure;
    const SaccadeResult stopped = shutdown_pipeline();
    if (stopped == SACCADE_OK) {
        pipeline_recovery_.start(now_ns);
    } else {
        fault_ = stopped;
        pipeline_recovery_.complete();
        (void)restart_application(this);
    }
    update_menu();
}

SaccadeResult ApplicationState::initialize(SaccadeAppDelegate* delegate) noexcept {
    delegate_ = delegate;
    const saccade::application::DesktopHostCallbacks callbacks{
        this,          dispatch_command, set_suspended,       neutralize_input,
        observe_input, open_settings,    restart_application, quit_application};
    SaccadeResult result = host_.initialize(callbacks);
    if (result != SACCADE_OK) return result;
    host_initialized_ = true;
    result = hotkeys_.initialize({&host_, saccade::application::dispatch_desktop_command,
                                  saccade::application::observe_desktop_input, route_key, observe_command_input});
    if (result != SACCADE_OK) return result;
    hotkeys_initialized_ = true;
    saccade::application::SettingsDocument initial = saccade::application::default_settings();
    (void)load_settings(&initial);
    result = settings_.initialize(initial, {this, apply_settings});
    if (result != SACCADE_OK) return result;
    settings_initialized_ = true;
    if (!saccade::apps::model_trust::configured) {
        fault_ = SACCADE_ERROR_NOT_FOUND;
        return SACCADE_OK;
    }
    NSString* resources = NSBundle.mainBundle.resourcePath;
    const char* root = resources.fileSystemRepresentation;
    if (root == nullptr || !append_component(&artifact_path_, root, "saccade.model") ||
        !append_component(&metallib_path_, root, "saccade_overlay.metallib") ||
        std::strlen(root) + 1U > model_root_.size()) {
        fault_ = SACCADE_ERROR_CAPACITY;
        return SACCADE_OK;
    }
    std::memcpy(model_root_.data(), root, std::strlen(root) + 1U);
    request_platform_permissions();
    const uint64_t now_ns = timestamp_ns();
    result = initialize_pipeline(now_ns);
    if (result != SACCADE_OK) {
        fault_ = result;
        os_log_error(OS_LOG_DEFAULT, "Pipeline initialization failed: result=%{public}d stage=%{public}u", result,
                     static_cast<uint32_t>(pipeline_.last_stage()));
        if (result == SACCADE_ERROR_BACKEND) pipeline_recovery_.start(now_ns);
        if (result == SACCADE_ERROR_STATE && pipeline_cleanup_required_) (void)restart_application(this);
        return SACCADE_OK;
    }

    result = synchronize_input_monitor(now_ns);
    permission_attention_ = result == SACCADE_ERROR_PERMISSION || !platform_permissions_ready();
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION) fault_ = result;
    return SACCADE_OK;
}

SaccadeResult ApplicationState::synchronize_input_monitor(uint64_t now_ns) noexcept {
    if (!pipeline_initialized_ || now_ns == 0) return SACCADE_ERROR_STATE;
    const bool monitor_permitted = CGPreflightListenEventAccess() && AXIsProcessTrusted();
    if (!monitor_permitted) {
        if (input_monitor_initialized_) {
            const SaccadeResult result = input_monitor_.shutdown();
            if (result != SACCADE_OK) return result;
            input_monitor_initialized_ = false;
        }
        next_input_monitor_retry_ns_ =
            now_ns > UINT64_MAX - permission_retry_period_ns ? UINT64_MAX : now_ns + permission_retry_period_ns;
        return SACCADE_ERROR_PERMISSION;
    }
    if (!input_monitor_initialized_ && now_ns >= next_input_monitor_retry_ns_) {
        next_input_monitor_retry_ns_ =
            now_ns > UINT64_MAX - permission_retry_period_ns ? UINT64_MAX : now_ns + permission_retry_period_ns;
        const SaccadeResult result = input_monitor_.initialize({this, observe_monitored_input, route_key});
        if (result != SACCADE_OK) return result;
        input_monitor_initialized_ = true;
    }
    return input_monitor_initialized_ && platform_permissions_ready() ? SACCADE_OK : SACCADE_ERROR_PERMISSION;
}

void ApplicationState::tick(uint64_t now_ns) noexcept {
    if (!pipeline_initialized_) {
        if (!pipeline_recovery_.due(now_ns)) return;
        const SaccadeResult recovered = initialize_pipeline(now_ns);
        if (recovered == SACCADE_OK) {
            pipeline_recovery_.complete();
            fault_ = SACCADE_OK;
        } else if (recovered == SACCADE_ERROR_STATE) {
            pipeline_recovery_.complete();
            fault_ = recovered;
            (void)restart_application(this);
        } else {
            fault_ = recovered;
            pipeline_recovery_.retry(now_ns);
        }
        update_menu();
        return;
    }
    const SaccadeResult monitor = synchronize_input_monitor(now_ns);
    const bool permission_attention = monitor == SACCADE_ERROR_PERMISSION || !platform_permissions_ready();
    if (monitor != SACCADE_OK && monitor != SACCADE_ERROR_PERMISSION) fault_ = monitor;
    if (permission_attention_ != permission_attention) {
        permission_attention_ = permission_attention;
        update_menu();
    }
    saccade::platform::macos::DesktopPipelineAdvance output{};
    const SaccadeResult result = pipeline_.advance(now_ns, &output);
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION && result != SACCADE_ERROR_NOT_FOUND &&
        result != SACCADE_ERROR_BUSY) {
        if (result == SACCADE_ERROR_BACKEND)
            begin_pipeline_recovery(result, now_ns);
        else {
            fault_ = result;
            update_menu();
        }
        return;
    }
    if (output.runtime.scene.scene_published) {
        const bool incomplete = (output.runtime.scene.packet_flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0;
        if (scene_incomplete_ != incomplete) {
            scene_incomplete_ = incomplete;
            update_menu();
        }
    }
    if (!agent_socket_initialized_ && now_ns >= next_agent_socket_retry_ns_) {
        const SaccadeResult agent_result = initialize_agent_socket(now_ns);
        if (agent_result != SACCADE_OK && agent_result != SACCADE_ERROR_ALREADY_EXISTS) {
            fault_ = agent_result;
            update_menu();
        }
    }
    if (agent_socket_initialized_) {
        const SaccadeResult agent_result = agent_socket_.advance(now_ns);
        if (agent_result != SACCADE_OK) {
            fault_ = agent_result;
            update_menu();
        }
    }
}

void ApplicationState::shutdown() noexcept {
    if (input_monitor_initialized_) {
        (void)input_monitor_.shutdown();
        input_monitor_initialized_ = false;
    }
    pipeline_recovery_.complete();
    (void)shutdown_pipeline();
    if (settings_initialized_) {
        (void)settings_.shutdown();
        settings_initialized_ = false;
    }
    if (hotkeys_initialized_) {
        (void)hotkeys_.shutdown();
        hotkeys_initialized_ = false;
    }
    if (host_initialized_) {
        (void)host_.shutdown();
        host_initialized_ = false;
    }
    delegate_ = nil;
}

void ApplicationState::update_menu() noexcept {
    [delegate_ rebuildMenu];
}

@implementation SaccadeAppDelegate

static NSTextField* settings_label(NSString* text) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.alignment = NSTextAlignmentRight;
    return label;
}

static NSPopUpButton* settings_popup(NSArray<NSString*>* items, NSInteger selected) {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 230, 26) pullsDown:NO];
    [popup addItemsWithTitles:items];
    [popup selectItemAtIndex:selected];
    return popup;
}

static NSTextField* settings_number(NSInteger value) {
    NSTextField* field = [NSTextField textFieldWithString:[NSString stringWithFormat:@"%ld", static_cast<long>(value)]];
    field.alignment = NSTextAlignmentRight;
    return field;
}

static NSTextField* settings_unsigned(uint64_t value, bool hexadecimal = false) {
    NSString* text =
        hexadecimal ? [NSString stringWithFormat:@"0x%llx", value] : [NSString stringWithFormat:@"%llu", value];
    NSTextField* field = [NSTextField textFieldWithString:text];
    field.alignment = NSTextAlignmentRight;
    return field;
}

static bool settings_u64(NSTextField* field, uint64_t maximum, uint64_t* output) noexcept {
    const char* text = field.stringValue.UTF8String;
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) return false;
    *output = static_cast<uint64_t>(value);
    return true;
}

static bool settings_i32(NSTextField* field, int32_t* output) noexcept {
    const char* text = field.stringValue.UTF8String;
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) return false;
    *output = static_cast<int32_t>(value);
    return true;
}

template <size_t Size> bool settings_utf8(NSString* text, std::array<char, Size>* output) noexcept {
    NSData* bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
    if (bytes == nil || bytes.length == 0 || bytes.length >= output->size()) return false;
    output->fill(0);
    std::memcpy(output->data(), bytes.bytes, bytes.length);
    return true;
}

static bool settings_alphabet(NSString* text, saccade::application::HintSettings* output) noexcept {
    if (text.length < 2 || text.length > output->alphabet.size()) return false;
    std::array<uint16_t, saccade::interaction::maximum_hint_alphabet> symbols{};
    [text getCharacters:symbols.data() range:NSMakeRange(0, text.length)];
    return saccade::application::set_hint_alphabet(output, symbols.data(), static_cast<uint32_t>(text.length)) ==
           SACCADE_OK;
}

static NSView* settings_scroll_view(NSGridView* grid) {
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    const NSSize fitting = grid.fittingSize;
    grid.frame = NSMakeRect(0, 0, std::max<CGFloat>(fitting.width, 570), fitting.height);
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 610, 500)];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = grid;
    return scroll;
}

static NSString* binding_summary(const saccade::application::SettingsDocument& settings) {
    NSMutableString* text = [NSMutableString string];
    const auto& keys = saccade::application::binding_keys();
    for (uint32_t index = 0; index < settings.binding_count; ++index) {
        const auto& binding = settings.bindings[index];
        const char* key_name = "?";
        for (const auto& key : keys)
            if (key.usage == binding.physical_key) {
                key_name = key.name;
                break;
            }
        [text appendFormat:@"%s: %s%s%s%s%s%s\n", saccade::application::command_name(binding.command),
                           (binding.modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0 ? "Ctrl+" : "",
                           (binding.modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0 ? "Alt+" : "",
                           (binding.modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0 ? "Shift+" : "",
                           (binding.modifiers & SACCADE_INPUT_MODIFIER_META) != 0 ? "Meta+" : "", key_name,
                           (binding.flags & saccade::application::hotkey_session_only) != 0 ? " [Session]" : ""];
    }
    return text;
}

static NSInteger binding_key_index(uint32_t usage) noexcept {
    const auto& keys = saccade::application::binding_keys();
    for (size_t index = 0; index < keys.size(); ++index) {
        if (keys[index].usage == usage) return static_cast<NSInteger>(index);
    }
    return -1;
}

static NSView* binding_keyboard(const saccade::application::SettingsDocument& settings,
                                SaccadeKeyboardSelection* selection) {
    constexpr int32_t keyboard_width = 575;
    constexpr int32_t keyboard_height = 210;
    saccade::application::BindingKeyboardLayout layout{};
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, keyboard_width, keyboard_height)];
    if (saccade::application::layout_binding_keyboard(keyboard_width, keyboard_height, 4, &layout) != SACCADE_OK) {
        return view;
    }

    NSMutableArray<NSButton*>* buttons = [NSMutableArray arrayWithCapacity:layout.key_count];
    for (uint32_t index = 0; index < layout.key_count; ++index) {
        const saccade::application::BindingKeyRect& rect = layout.keys[index];
        const NSInteger key_index = binding_key_index(rect.usage);
        if (key_index < 0) continue;
        const auto& key = saccade::application::binding_keys()[static_cast<size_t>(key_index)];
        NSString* title = [NSString stringWithUTF8String:key.name];
        NSButton* button = [NSButton buttonWithTitle:title target:selection action:@selector(chooseKey:)];
        button.frame = NSMakeRect(rect.x, keyboard_height - rect.y - rect.height, rect.width, rect.height);
        button.tag = key_index;
        button.bezelStyle = NSBezelStyleRegularSquare;
        button.controlSize = NSControlSizeSmall;
        button.font = [NSFont systemFontOfSize:10];
        [button setButtonType:NSButtonTypePushOnPushOff];
        for (uint32_t binding_index = 0; binding_index < settings.binding_count; ++binding_index) {
            if (settings.bindings[binding_index].physical_key != rect.usage) continue;
            button.toolTip = [NSString
                stringWithUTF8String:saccade::application::command_name(settings.bindings[binding_index].command)];
            break;
        }
        if (key_index == selection.popup.indexOfSelectedItem) {
            button.state = NSControlStateValueOn;
            selection.selected = button;
        }
        [buttons addObject:button];
        [view addSubview:button];
    }
    selection.buttons = buttons;
    return view;
}

static SaccadeResult run_binding_editor() {
    using namespace saccade::application;
    for (;;) {
        NSMutableArray<NSString*>* commands = [NSMutableArray array];
        for (uint32_t index = 1; index <= binding_command_count; ++index)
            [commands addObject:[NSString stringWithUTF8String:command_name(static_cast<Command>(index))]];
        NSMutableArray<NSString*>* key_names = [NSMutableArray array];
        for (const BindingKey& key : binding_keys())
            [key_names addObject:[NSString stringWithUTF8String:key.name]];
        NSPopUpButton* command = settings_popup(commands, 0);
        NSPopUpButton* key = settings_popup(key_names, 0);
        SaccadeKeyboardSelection* keyboardSelection = [[SaccadeKeyboardSelection alloc] init];
        keyboardSelection.popup = key;
        NSView* keyboard = binding_keyboard(application.settings_.staged(), keyboardSelection);
        NSButton* control = [NSButton checkboxWithTitle:@"Control" target:nil action:nil];
        NSButton* alt = [NSButton checkboxWithTitle:@"Alt" target:nil action:nil];
        NSButton* shift = [NSButton checkboxWithTitle:@"Shift" target:nil action:nil];
        NSButton* meta = [NSButton checkboxWithTitle:@"Command" target:nil action:nil];
        NSButton* sessionOnly = [NSButton checkboxWithTitle:@"Session only" target:nil action:nil];
        SaccadeBindingSelection* bindingSelection = [[SaccadeBindingSelection alloc] init];
        bindingSelection->settings_ = &application.settings_.staged();
        bindingSelection.command = command;
        bindingSelection.control = control;
        bindingSelection.alt = alt;
        bindingSelection.shift = shift;
        bindingSelection.meta = meta;
        bindingSelection.sessionOnly = sessionOnly;
        bindingSelection.keyboardSelection = keyboardSelection;
        command.target = bindingSelection;
        command.action = @selector(loadCommand:);
        [bindingSelection loadCommand:nil];
        NSGridView* grid = [NSGridView gridViewWithViews:@[
            @[ settings_label(@"Command"), command ], @[ settings_label(@"Physical key"), key ],
            @[ settings_label(@"Modifiers"), [NSStackView stackViewWithViews:@[ control, alt, shift, meta ]] ],
            @[ settings_label(@"Availability"), sessionOnly ], @[ settings_label(@"Keyboard"), keyboard ]
        ]];
        grid.rowSpacing = 8;
        NSAlert* editor = [[NSAlert alloc] init];
        editor.messageText = @"Keyboard bindings";
        editor.informativeText = binding_summary(application.settings_.staged());
        editor.accessoryView = grid;
        [editor addButtonWithTitle:@"Set"];
        [editor addButtonWithTitle:@"Remove"];
        [editor addButtonWithTitle:@"Done"];
        const NSModalResponse response = [editor runModal];
        if (response == NSAlertThirdButtonReturn) return application.settings_.commit();
        SettingsDocument staged = application.settings_.staged();
        const Command selected_command = static_cast<Command>(command.indexOfSelectedItem + 1);
        SaccadeResult result = SACCADE_OK;
        BindingConflict conflict{};
        if (response == NSAlertFirstButtonReturn) {
            uint32_t modifiers = 0;
            if (control.state == NSControlStateValueOn) modifiers |= SACCADE_INPUT_MODIFIER_CONTROL;
            if (alt.state == NSControlStateValueOn) modifiers |= SACCADE_INPUT_MODIFIER_ALT;
            if (shift.state == NSControlStateValueOn) modifiers |= SACCADE_INPUT_MODIFIER_SHIFT;
            if (meta.state == NSControlStateValueOn) modifiers |= SACCADE_INPUT_MODIFIER_META;
            uint32_t flags = selected_command == Command::suspend_toggle ? hotkey_always_active : 0;
            if (sessionOnly.state == NSControlStateValueOn) flags |= hotkey_session_only;
            const uint32_t physical_key = binding_keys()[static_cast<size_t>(key.indexOfSelectedItem)].usage;
            uint16_t logical_symbol = saccade::platform::macos::logical_symbol_from_hid_usage(physical_key);
            if (logical_symbol == 0) logical_symbol = default_logical_symbol(physical_key);
            result = set_binding(
                &staged, {selected_command, physical_key, modifiers, static_cast<uint16_t>(flags), logical_symbol},
                &conflict);
        } else {
            result = remove_binding(&staged, selected_command);
        }
        if (result == SACCADE_OK) result = application.settings_.stage(staged);
        if (result == SACCADE_OK || result == SACCADE_ERROR_NOT_FOUND) continue;
        NSAlert* failure = [[NSAlert alloc] init];
        failure.messageText = result == SACCADE_ERROR_ALREADY_EXISTS
                                  ? [NSString stringWithFormat:@"Key is assigned to %s", command_name(conflict.command)]
                                  : @"Binding was not changed";
        [failure addButtonWithTitle:@"OK"];
        [failure runModal];
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSImage* status_icon = [NSImage imageNamed:@"SaccadeStatus"];
    [status_icon setTemplate:YES];
    status_icon.size = NSMakeSize(18.0, 18.0);
    self.statusItem.button.image = status_icon;
    self.statusItem.button.toolTip = @"Saccade";
    const SaccadeResult started = application.initialize(self);
    if (started != SACCADE_OK) application.fault_ = started;
    self.runtimeTimer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 120.0)
                                                         target:self
                                                       selector:@selector(tickRuntime:)
                                                       userInfo:nil
                                                        repeats:YES];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshTopology:)
                                               name:NSApplicationDidChangeScreenParametersNotification
                                             object:nil];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                       selector:@selector(inputAvailable:)
                                                           name:NSWorkspaceDidWakeNotification
                                                         object:nil];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                       selector:@selector(inputUnavailable:)
                                                           name:NSWorkspaceWillSleepNotification
                                                         object:nil];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                       selector:@selector(inputUnavailable:)
                                                           name:NSWorkspaceSessionDidResignActiveNotification
                                                         object:nil];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
                                                       selector:@selector(inputAvailable:)
                                                           name:NSWorkspaceSessionDidBecomeActiveNotification
                                                         object:nil];
    [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                      selector:@selector(inputUnavailable:)
                                                          name:@"com.apple.screenIsLocked"
                                                        object:nil];
    [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                      selector:@selector(inputAvailable:)
                                                          name:@"com.apple.screenIsUnlocked"
                                                        object:nil];
    [self rebuildMenu];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self.runtimeTimer invalidate];
    self.runtimeTimer = nil;
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
    [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
    application.shutdown();
    if (application.restart_requested_) (void)launch_replacement();
}

- (void)rebuildMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Saccade"];
    [menu addItemWithTitle:@"Settings..." action:@selector(openSettings:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Diagnostics..." action:@selector(openDiagnostics:) keyEquivalent:@""];
    self.suspendItem = [menu addItemWithTitle:@"Suspend hotkeys" action:@selector(toggleSuspended:) keyEquivalent:@""];
    self.suspendItem.state = application.host_.suspended() ? NSControlStateValueOn : NSControlStateValueOff;
    if (application.permission_attention_ || application.scene_incomplete_ || application.fault_ != SACCADE_OK) {
        [menu addItem:[NSMenuItem separatorItem]];
        if (application.permission_attention_) {
            self.faultItem = [menu addItemWithTitle:@"Permissions required" action:nil keyEquivalent:@""];
            self.faultItem.enabled = NO;
            [menu addItemWithTitle:@"Grant permissions..." action:@selector(requestPermissions:) keyEquivalent:@""];
        }
        if (application.fault_ != SACCADE_OK) {
            self.faultItem = [menu addItemWithTitle:@"Targeting runtime is not connected" action:nil keyEquivalent:@""];
            self.faultItem.enabled = NO;
        }
        if (application.scene_incomplete_) {
            self.faultItem = [menu addItemWithTitle:@"Partial target coverage" action:nil keyEquivalent:@""];
            self.faultItem.enabled = NO;
        }
    }
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Restart" action:@selector(restart:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Quit Saccade" action:@selector(quit:) keyEquivalent:@""];
    for (NSMenuItem* item in menu.itemArray)
        item.target = self;
    self.statusItem.menu = menu;
    self.statusItem.button.toolTip = application.host_.suspended()   ? @"Saccade - suspended"
                                     : application.scene_incomplete_ ? @"Saccade - partial target coverage"
                                     : !application.permission_attention_ && application.fault_ == SACCADE_OK
                                         ? @"Saccade"
                                         : @"Saccade - attention required";
}

- (void)requestPermissions:(id)sender {
    (void)sender;
    request_platform_permissions();
    application.next_input_monitor_retry_ns_ = 0;
    const SaccadeResult result = application.synchronize_input_monitor(timestamp_ns());
    application.permission_attention_ = result == SACCADE_ERROR_PERMISSION || !platform_permissions_ready();
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION) application.fault_ = result;
    [self rebuildMenu];
}

- (void)openSettings:(id)sender {
    (void)sender;
    if (application.settings_.editing()) (void)application.settings_.cancel();
    if (application.settings_.begin_edit() != SACCADE_OK) return;
    const saccade::application::SettingsDocument& settings = application.settings_.staged();

    NSTextField* alphabet =
        [NSTextField textFieldWithString:[NSString stringWithCharacters:settings.hints.alphabet.data()
                                                                 length:settings.hints.alphabet_count]];
    NSString* languageValue = [NSString stringWithUTF8String:settings.hints.language.data()];
    NSTextField* language = [NSTextField textFieldWithString:languageValue != nil ? languageValue : @""];
    NSPopUpButton* hintPriority = settings_popup(@[ @"Scene order", @"Pointer", @"Scope center", @"Randomized" ],
                                                 static_cast<NSInteger>(settings.hints.priority));
    NSPopUpButton* hintPlacement = settings_popup(@[ @"Automatic", @"Above", @"Below", @"Left", @"Right" ],
                                                  static_cast<NSInteger>(settings.hints.placement));
    NSPopUpButton* hintSorting =
        settings_popup(@[ @"Sorted", @"Randomized" ], static_cast<NSInteger>(settings.hints.sorting));

    NSPopUpButton* source =
        settings_popup(@[ @"Pixel", @"Semantic", @"Grid", @"Fused" ], static_cast<NSInteger>(settings.source));
    NSTextField* confidence = settings_number(static_cast<NSInteger>(settings.detector.confidence_q16));
    NSTextField* textConfidence = settings_number(static_cast<NSInteger>(settings.detector.text_sensitivity_q16));
    NSTextField* duplicateIou = settings_number(static_cast<NSInteger>(settings.detector.duplicate_iou_q16));
    NSTextField* minimumWidth = settings_number(static_cast<NSInteger>(settings.detector.minimum_width_q8));
    NSTextField* minimumHeight = settings_number(static_cast<NSInteger>(settings.detector.minimum_height_q8));
    NSPopUpButton* mergePolicy = settings_popup(@[ @"Balanced", @"Text first", @"Controls first", @"Disabled" ],
                                                static_cast<NSInteger>(settings.detector.merge_policy));
    NSTextField* rows = settings_number(settings.grid.rows);
    NSTextField* columns = settings_number(settings.grid.columns);
    NSTextField* marginX = settings_number(settings.grid.margin_x_q8);
    NSTextField* marginY = settings_number(settings.grid.margin_y_q8);

    NSPopUpButton* scope =
        settings_popup(@[ @"Desktop", @"Active window", @"Monitor" ], static_cast<NSInteger>(settings.scope));
    NSTextField* monitor = settings_unsigned(settings.monitor_stable_id, true);

    NSPopUpButton* finalPointer = settings_popup(@[ @"Target", @"Original position", @"Anchor" ],
                                                 static_cast<NSInteger>(settings.pointer.final_position));
    NSTextField* movement = settings_number(settings.pointer.movement_duration_ms);
    NSTextField* anchorX = settings_number(settings.pointer.anchor_x_q8);
    NSTextField* anchorY = settings_number(settings.pointer.anchor_y_q8);
    NSPopUpButton* initialMode = settings_popup(@[ @"Single", @"Dual", @"Multi", @"Path" ],
                                                static_cast<NSInteger>(settings.actions.initial_mode) - 1);
    NSTextField* timeout = settings_number(settings.actions.timeout_ms);
    NSTextField* hold = settings_number(settings.actions.hold_duration_ms);
    NSTextField* drag = settings_number(settings.actions.drag_duration_ms);
    NSTextField* scrollDuration = settings_number(settings.actions.scroll_duration_ms);
    NSTextField* scrollVertical = settings_number(settings.actions.scroll_vertical_q8);
    NSTextField* scrollHorizontal = settings_number(settings.actions.scroll_horizontal_q8);
    NSButton* clickControl = [NSButton checkboxWithTitle:@"Control" target:nil action:nil];
    NSButton* clickAlt = [NSButton checkboxWithTitle:@"Alt" target:nil action:nil];
    NSButton* clickShift = [NSButton checkboxWithTitle:@"Shift" target:nil action:nil];
    NSButton* clickMeta = [NSButton checkboxWithTitle:@"Command" target:nil action:nil];
    clickControl.state = (settings.actions.click_modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0;
    clickAlt.state = (settings.actions.click_modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0;
    clickShift.state = (settings.actions.click_modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0;
    clickMeta.state = (settings.actions.click_modifiers & SACCADE_INPUT_MODIFIER_META) != 0;

    NSString* fontFamilyValue = [NSString stringWithUTF8String:settings.appearance.font_family.data()];
    NSTextField* fontFamily = [NSTextField textFieldWithString:fontFamilyValue != nil ? fontFamilyValue : @""];
    NSPopUpButton* theme = settings_popup(@[ @"System", @"High contrast", @"Light", @"Dark", @"Custom" ],
                                          static_cast<NSInteger>(settings.appearance.theme));
    NSPopUpButton* placement = settings_popup(@[ @"Automatic", @"Above", @"Below", @"Left", @"Right" ],
                                              static_cast<NSInteger>(settings.appearance.placement));
    NSTextField* fontSize = settings_number(settings.appearance.font_size_q8);
    NSTextField* fontWeight = settings_number(settings.appearance.font_weight);
    NSTextField* labelColor = settings_unsigned(settings.appearance.label_rgba, true);
    NSTextField* backgroundColor = settings_unsigned(settings.appearance.background_rgba, true);
    NSTextField* outlineColor = settings_unsigned(settings.appearance.outline_rgba, true);
    NSTextField* glowColor = settings_unsigned(settings.appearance.glow_rgba, true);
    NSTextField* outlineWidth = settings_number(settings.appearance.outline_width_q8);
    NSTextField* glowRadius = settings_number(settings.appearance.glow_radius_q8);
    NSButton* animate = [NSButton checkboxWithTitle:@"Animate overlay" target:nil action:nil];
    animate.state = (settings.flags & saccade::application::settings_animate_overlay) != 0 ? NSControlStateValueOn
                                                                                           : NSControlStateValueOff;
    NSButton* reduced = [NSButton checkboxWithTitle:@"Reduced motion" target:nil action:nil];
    reduced.state = (settings.flags & saccade::application::settings_reduced_motion) != 0 ? NSControlStateValueOn
                                                                                          : NSControlStateValueOff;

    NSPopUpButton* compute =
        settings_popup(@[ @"All compute units", @"CPU only", @"CPU + GPU", @"CPU + Neural Engine", @"Named device" ],
                       static_cast<NSInteger>(settings.compute.policy));
    NSTextField* device = settings_unsigned(settings.compute.device_stable_id, true);
    NSPopUpButton* resetPage = settings_popup(
        @[ @"Bindings", @"Hints", @"Detector", @"Scope", @"Pointer and actions", @"Appearance", @"Compute" ], 1);

    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ settings_label(@"Hint alphabet"), alphabet ],
        @[ settings_label(@"Hint language"), language ],
        @[ settings_label(@"Hint priority"), hintPriority ],
        @[ settings_label(@"Hint placement"), hintPlacement ],
        @[ settings_label(@"Hint sorting"), hintSorting ],
        @[ settings_label(@"Target source"), source ],
        @[ settings_label(@"Confidence (Q16)"), confidence ],
        @[ settings_label(@"Text sensitivity (Q16)"), textConfidence ],
        @[ settings_label(@"Duplicate IoU (Q16)"), duplicateIou ],
        @[ settings_label(@"Minimum width (Q8)"), minimumWidth ],
        @[ settings_label(@"Minimum height (Q8)"), minimumHeight ],
        @[ settings_label(@"Merge policy"), mergePolicy ],
        @[ settings_label(@"Grid rows"), rows ],
        @[ settings_label(@"Grid columns"), columns ],
        @[ settings_label(@"Grid margin X (Q8)"), marginX ],
        @[ settings_label(@"Grid margin Y (Q8)"), marginY ],
        @[ settings_label(@"Scope"), scope ],
        @[ settings_label(@"Monitor stable ID"), monitor ],
        @[ settings_label(@"Final pointer"), finalPointer ],
        @[ settings_label(@"Movement (ms)"), movement ],
        @[ settings_label(@"Anchor X (Q8)"), anchorX ],
        @[ settings_label(@"Anchor Y (Q8)"), anchorY ],
        @[ settings_label(@"Initial mode"), initialMode ],
        @[ settings_label(@"Timeout (ms)"), timeout ],
        @[ settings_label(@"Hold (ms)"), hold ],
        @[ settings_label(@"Drag (ms)"), drag ],
        @[ settings_label(@"Continuous scroll lease (ms; 0 = 250)"), scrollDuration ],
        @[ settings_label(@"Vertical scroll (Q8)"), scrollVertical ],
        @[ settings_label(@"Horizontal scroll (Q8)"), scrollHorizontal ],
        @[
            settings_label(@"Click modifiers"),
            [NSStackView stackViewWithViews:@[ clickControl, clickAlt, clickShift, clickMeta ]]
        ],
        @[ settings_label(@"Font family"), fontFamily ],
        @[ settings_label(@"Theme"), theme ],
        @[ settings_label(@"Label placement"), placement ],
        @[ settings_label(@"Font size (Q8)"), fontSize ],
        @[ settings_label(@"Font weight"), fontWeight ],
        @[ settings_label(@"Label RGBA"), labelColor ],
        @[ settings_label(@"Background RGBA"), backgroundColor ],
        @[ settings_label(@"Outline RGBA"), outlineColor ],
        @[ settings_label(@"Glow RGBA"), glowColor ],
        @[ settings_label(@"Outline width (Q8)"), outlineWidth ],
        @[ settings_label(@"Glow radius (Q8)"), glowRadius ],
        @[ settings_label(@"Motion"), [NSStackView stackViewWithViews:@[ animate, reduced ]] ],
        @[ settings_label(@"Compute policy"), compute ],
        @[ settings_label(@"Device stable ID"), device ],
        @[ settings_label(@"Reset page"), resetPage ]
    ]];

    auto collect = [&]() noexcept -> SaccadeResult {
        using namespace saccade::application;
        SettingsDocument staged = settings;
        uint64_t value = 0;
        if (!settings_alphabet(alphabet.stringValue, &staged.hints) ||
            !settings_utf8(language.stringValue, &staged.hints.language) ||
            !settings_utf8(fontFamily.stringValue, &staged.appearance.font_family))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.hints.priority = static_cast<saccade::interaction::HintPriority>(hintPriority.indexOfSelectedItem);
        staged.hints.placement = static_cast<HintPlacement>(hintPlacement.indexOfSelectedItem);
        staged.hints.sorting = static_cast<HintSorting>(hintSorting.indexOfSelectedItem);
        staged.source = static_cast<TargetSource>(source.indexOfSelectedItem);
        if (!settings_u64(confidence, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.detector.confidence_q16 = static_cast<uint16_t>(value);
        if (!settings_u64(textConfidence, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.detector.text_sensitivity_q16 = static_cast<uint16_t>(value);
        if (!settings_u64(duplicateIou, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.detector.duplicate_iou_q16 = static_cast<uint16_t>(value);
        if (!settings_u64(minimumWidth, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.detector.minimum_width_q8 = static_cast<uint16_t>(value);
        if (!settings_u64(minimumHeight, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.detector.minimum_height_q8 = static_cast<uint16_t>(value);
        staged.detector.merge_policy = static_cast<MergePolicy>(mergePolicy.indexOfSelectedItem);
        if (!settings_u64(rows, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.grid.rows = static_cast<uint16_t>(value);
        if (!settings_u64(columns, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.grid.columns = static_cast<uint16_t>(value);
        if (!settings_u64(marginX, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.grid.margin_x_q8 = static_cast<uint16_t>(value);
        if (!settings_u64(marginY, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.grid.margin_y_q8 = static_cast<uint16_t>(value);
        staged.scope = static_cast<TargetScope>(scope.indexOfSelectedItem);
        if (!settings_u64(monitor, UINT64_MAX, &staged.monitor_stable_id)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.pointer.final_position = static_cast<FinalPointerPosition>(finalPointer.indexOfSelectedItem);
        if (!settings_u64(movement, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.pointer.movement_duration_ms = static_cast<uint32_t>(value);
        if (!settings_i32(anchorX, &staged.pointer.anchor_x_q8) || !settings_i32(anchorY, &staged.pointer.anchor_y_q8))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.initial_mode =
            static_cast<saccade::interaction::SelectionMode>(initialMode.indexOfSelectedItem + 1);
        if (!settings_u64(timeout, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.timeout_ms = static_cast<uint32_t>(value);
        if (!settings_u64(hold, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.hold_duration_ms = static_cast<uint32_t>(value);
        if (!settings_u64(drag, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.drag_duration_ms = static_cast<uint32_t>(value);
        if (!settings_u64(scrollDuration, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.scroll_duration_ms = static_cast<uint32_t>(value);
        if (!settings_i32(scrollVertical, &staged.actions.scroll_vertical_q8) ||
            !settings_i32(scrollHorizontal, &staged.actions.scroll_horizontal_q8))
            return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.actions.click_modifiers = 0;
        if (clickControl.state == NSControlStateValueOn)
            staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_CONTROL;
        if (clickAlt.state == NSControlStateValueOn) staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_ALT;
        if (clickShift.state == NSControlStateValueOn) staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_SHIFT;
        if (clickMeta.state == NSControlStateValueOn) staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_META;
        staged.appearance.theme = static_cast<Theme>(theme.indexOfSelectedItem);
        staged.appearance.placement = static_cast<HintPlacement>(placement.indexOfSelectedItem);
        if (!settings_u64(fontSize, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.font_size_q8 = static_cast<uint32_t>(value);
        if (!settings_u64(fontWeight, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.font_weight = static_cast<uint32_t>(value);
        if (!settings_u64(labelColor, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.label_rgba = static_cast<uint32_t>(value);
        if (!settings_u64(backgroundColor, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.background_rgba = static_cast<uint32_t>(value);
        if (!settings_u64(outlineColor, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.outline_rgba = static_cast<uint32_t>(value);
        if (!settings_u64(glowColor, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.glow_rgba = static_cast<uint32_t>(value);
        if (!settings_u64(outlineWidth, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.outline_width_q8 = static_cast<uint16_t>(value);
        if (!settings_u64(glowRadius, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
        staged.appearance.glow_radius_q8 = static_cast<uint16_t>(value);
        staged.flags = 0;
        if (animate.state == NSControlStateValueOn) staged.flags |= settings_animate_overlay;
        if (reduced.state == NSControlStateValueOn) staged.flags |= settings_reduced_motion;
        staged.compute.policy = static_cast<ComputePolicy>(compute.indexOfSelectedItem);
        if (!settings_u64(device, UINT64_MAX, &staged.compute.device_stable_id)) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (staged.compute.policy != ComputePolicy::named_device) staged.compute.device_stable_id = 0;
        return application.settings_.stage(staged);
    };

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Saccade settings";
    const auto stats = application.pipeline_.stats();
    alert.informativeText =
        [NSString stringWithFormat:@"Frames %@  Targets %@  Overlay %@  Failures %@", @(stats.frames_offered),
                                   @(stats.activations), @(stats.overlay_publications), @(stats.failures)];
    alert.accessoryView = settings_scroll_view(grid);
    [alert addButtonWithTitle:@"Apply"];
    [alert addButtonWithTitle:@"Cancel"];
    [alert addButtonWithTitle:@"Reset Page"];
    [alert addButtonWithTitle:@"Restore All"];
    [alert addButtonWithTitle:@"Import..."];
    [alert addButtonWithTitle:@"Export..."];
    [alert addButtonWithTitle:@"Bindings..."];
    [NSApp activateIgnoringOtherApps:YES];
    const NSModalResponse response = [alert runModal];
    if (response == NSAlertSecondButtonReturn) {
        (void)application.settings_.cancel();
        return;
    }
    if (response == NSAlertFirstButtonReturn + 4) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] == NSModalResponseOK) {
            NSData* data = [NSData dataWithContentsOfURL:panel.URL];
            const SaccadeResult imported =
                data == nil
                    ? SACCADE_ERROR_NOT_FOUND
                    : application.settings_.import_document({static_cast<const uint8_t*>(data.bytes), data.length});
            const SaccadeResult committed = imported == SACCADE_OK ? application.settings_.commit() : imported;
            if (committed == SACCADE_OK) return;
        }
        if (application.settings_.editing()) (void)application.settings_.cancel();
    } else if (response == NSAlertFirstButtonReturn + 5) {
        std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
        size_t size = 0;
        const SaccadeResult exported = application.settings_.export_document({bytes.data(), bytes.size()}, &size);
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"saccade-settings.bin";
        if (exported == SACCADE_OK && [panel runModal] == NSModalResponseOK) {
            NSData* data = [NSData dataWithBytes:bytes.data() length:size];
            if ([data writeToURL:panel.URL options:NSDataWritingAtomic error:nil]) {
                (void)application.settings_.cancel();
                return;
            }
        }
        (void)application.settings_.cancel();
    } else if (response == NSAlertFirstButtonReturn + 6) {
        if (run_binding_editor() == SACCADE_OK) return;
        if (application.settings_.editing()) (void)application.settings_.cancel();
    } else if (response == NSAlertFirstButtonReturn + 3) {
        const SaccadeResult reset = application.settings_.reset_all();
        const SaccadeResult committed = reset == SACCADE_OK ? application.settings_.commit() : reset;
        if (committed == SACCADE_OK) return;
        (void)application.settings_.cancel();
    } else if (response == NSAlertThirdButtonReturn) {
        SaccadeResult result = collect();
        if (result == SACCADE_OK)
            result = application.settings_.reset_page(
                static_cast<saccade::application::SettingsPage>(resetPage.indexOfSelectedItem));
        if (result == SACCADE_OK) result = application.settings_.commit();
        if (result == SACCADE_OK) return;
        (void)application.settings_.cancel();
    } else {
        const SaccadeResult stagedResult = collect();
        const SaccadeResult committed = stagedResult == SACCADE_OK ? application.settings_.commit() : stagedResult;
        if (committed == SACCADE_OK) return;
        (void)application.settings_.cancel();
    }
    NSAlert* failure = [[NSAlert alloc] init];
    failure.messageText = @"Settings were not applied";
    [failure addButtonWithTitle:@"OK"];
    [failure runModal];
}

- (void)toggleSuspended:(id)sender {
    (void)sender;
    (void)application.host_.dispatch(CommandEvent{timestamp_ns(), Command::suspend_toggle});
}

- (void)openDiagnostics:(id)sender {
    (void)sender;
    if (self.diagnosticsPanel != nil) {
        [self refreshDiagnostics];
        [self.diagnosticsPanel makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return;
    }
    self.diagnosticsPanel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 560)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.diagnosticsPanel.title = @"Saccade Diagnostics";
    self.diagnosticsPanel.releasedWhenClosed = NO;
    self.diagnosticsPanel.contentMinSize =
        NSMakeSize(saccade::application::debugger_minimum_width, saccade::application::debugger_minimum_height);
    self.diagnosticsPanel.delegate = self;
    self.diagnosticsView = [NSSegmentedControl segmentedControlWithLabels:@[
        @"Overview", @"Displays", @"Runtime", @"Overlay / GPU", @"Memory", @"Trace", @"Frames / Transforms",
        @"Scene / Fusion"
    ]
                                                             trackingMode:NSSegmentSwitchTrackingSelectOne
                                                                   target:self
                                                                   action:@selector(changeDiagnosticsView:)];
    self.diagnosticsView.selectedSegment = 0;
    self.diagnosticsScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    self.diagnosticsScroll.hasVerticalScroller = YES;
    self.diagnosticsText = [[NSTextView alloc] initWithFrame:self.diagnosticsScroll.contentView.bounds];
    self.diagnosticsText.editable = NO;
    self.diagnosticsText.selectable = YES;
    self.diagnosticsText.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.diagnosticsText.textContainerInset = NSMakeSize(8, 8);
    self.diagnosticsText.autoresizingMask = NSViewWidthSizable;
    self.diagnosticsScroll.documentView = self.diagnosticsText;
    [self.diagnosticsPanel.contentView addSubview:self.diagnosticsView];
    [self.diagnosticsPanel.contentView addSubview:self.diagnosticsScroll];
    NSArray<NSString*>* titles = @[ @"Capture Scene", @"Dry Run", @"Replay", @"Clear" ];
    SEL actions[] = {@selector(captureDebugScene:), @selector(dryRunDebugPlan:), @selector(replayDebugPlan:),
                     @selector(clearDebugCapture:)};
    NSMutableArray<NSButton*>* buttons = [NSMutableArray arrayWithCapacity:titles.count];
    for (NSUInteger index = 0; index < 4; ++index) {
        NSButton* button = [NSButton buttonWithTitle:titles[index] target:self action:actions[index]];
        [buttons addObject:button];
        [self.diagnosticsPanel.contentView addSubview:button];
    }
    self.diagnosticsActions = buttons;
    self.debugFault = settings_popup(@[ @"Capture", @"Inference", @"Scene", @"Overlay", @"Input" ], 0);
    [self.diagnosticsPanel.contentView addSubview:self.debugFault];
    self.debugArmFault = [NSButton buttonWithTitle:@"Arm Fault" target:self action:@selector(armDebugFault:)];
    [self.diagnosticsPanel.contentView addSubview:self.debugArmFault];
    [self layoutDiagnostics];
    [self refreshDiagnostics];
    self.diagnosticsTicks = 0;
    [self.diagnosticsPanel center];
    [self.diagnosticsPanel makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)layoutDiagnostics {
    if (self.diagnosticsPanel == nil) return;
    const NSRect bounds = self.diagnosticsPanel.contentView.bounds;
    saccade::application::DebuggerLayout layout{};
    if (saccade::application::make_debugger_layout(static_cast<int32_t>(NSWidth(bounds)),
                                                   static_cast<int32_t>(NSHeight(bounds)), &layout) != SACCADE_OK)
        return;
    self.diagnosticsView.frame = debugger_rect(layout.views, NSHeight(bounds));
    self.diagnosticsScroll.frame = debugger_rect(layout.content, NSHeight(bounds));
    NSRect textFrame = self.diagnosticsText.frame;
    textFrame.size.width = NSWidth(self.diagnosticsScroll.contentView.bounds);
    textFrame.size.height = std::max(NSHeight(textFrame), NSHeight(self.diagnosticsScroll.contentView.bounds));
    self.diagnosticsText.frame = textFrame;
    for (NSUInteger index = 0; index < self.diagnosticsActions.count && index < layout.actions.size(); ++index)
        self.diagnosticsActions[index].frame = debugger_rect(layout.actions[index], NSHeight(bounds));
    self.debugFault.frame = debugger_rect(layout.fault, NSHeight(bounds));
    self.debugArmFault.frame = debugger_rect(layout.arm_fault, NSHeight(bounds));
}

- (void)windowDidResize:(NSNotification*)notification {
    if (notification.object == self.diagnosticsPanel) [self layoutDiagnostics];
}

- (void)changeDiagnosticsView:(id)sender {
    (void)sender;
    [self refreshDiagnostics];
}

- (void)captureDebugScene:(id)sender {
    (void)sender;
    const SaccadeResult result = application.pipeline_.debug_capture_scene();
    self.debuggerOperation = [NSString stringWithFormat:@"Capture scene: %d", result];
    [self refreshDiagnostics];
}

- (void)dryRunDebugPlan:(id)sender {
    (void)sender;
    saccade::application::DebuggerPlanView plan{};
    const SaccadeResult result = application.pipeline_.debug_dry_run(timestamp_ns(), &plan);
    self.debuggerOperation =
        [NSString stringWithFormat:@"Dry run: %d  bytes %zu  commands %u", result, plan.bytes.size,
                                   plan.plan.header == nullptr ? 0 : plan.plan.header->command_count];
    [self refreshDiagnostics];
}

- (void)replayDebugPlan:(id)sender {
    (void)sender;
    saccade::application::DebuggerPlanView plan{};
    const SaccadeResult result = application.pipeline_.debug_replay(&plan);
    self.debuggerOperation =
        [NSString stringWithFormat:@"Replay: %d  bytes %zu  commands %u", result, plan.bytes.size,
                                   plan.plan.header == nullptr ? 0 : plan.plan.header->command_count];
    [self refreshDiagnostics];
}

- (void)clearDebugCapture:(id)sender {
    (void)sender;
    const SaccadeResult result = application.pipeline_.debug_clear();
    self.debuggerOperation = [NSString stringWithFormat:@"Clear: %d", result];
    [self refreshDiagnostics];
}

- (void)armDebugFault:(id)sender {
    (void)sender;
    const auto point = static_cast<saccade::application::DebugFaultPoint>(self.debugFault.indexOfSelectedItem);
    const SaccadeResult result = application.pipeline_.debug_arm_fault(point, 1, SACCADE_ERROR_BACKEND);
    self.debuggerOperation =
        [NSString stringWithFormat:@"Arm fault %ld: %d", self.debugFault.indexOfSelectedItem, result];
    [self refreshDiagnostics];
}

- (void)refreshDiagnostics {
    saccade::platform::macos::DesktopPipelineDiagnostics diagnostics{};
    const SaccadeResult result =
        application.pipeline_initialized_ ? application.pipeline_.read_diagnostics(&diagnostics) : SACCADE_ERROR_STATE;
    if (result != SACCADE_OK) {
        self.diagnosticsText.string = [NSString
            stringWithFormat:@"Runtime unavailable\nResult %d\nStage %u\nRecovery %s\nAttempt %u\nNext %llu ns",
                             application.fault_, static_cast<uint32_t>(application.pipeline_.last_stage()),
                             application.pipeline_recovery_.pending() ? "pending" : "inactive",
                             application.pipeline_recovery_.attempt(),
                             application.pipeline_recovery_.next_attempt_ns()];
        return;
    }
    NSMutableString* text = [NSMutableString string];
    if (self.debuggerOperation != nil) [text appendFormat:@"%@\n\n", self.debuggerOperation];
    const auto view = static_cast<saccade::application::DebuggerView>(self.diagnosticsView.selectedSegment);
    if (view == saccade::application::DebuggerView::overview) {
        [text
            appendFormat:@"Permissions  capture %s  accessibility %s  input %s\n"
                          "Surface  disposition %u  reasons 0x%x  epoch %llu\n"
                          "Displays %u  topology %llu  source %u  scope %u  compute %u\n"
                          "Model %016llx  provider %016llx  device %016llx  precision 0x%x\n"
                          "Scene %llu  targets %u  flags 0x%x  partial %s\n"
                          "Failures pipeline %llu  runtime %llu  neural %llu  scene %llu\n",
                         (diagnostics.permissions & saccade::platform::macos::diagnostic_capture_permission) != 0
                             ? "yes"
                             : "no",
                         (diagnostics.permissions & saccade::platform::macos::diagnostic_accessibility_permission) != 0
                             ? "yes"
                             : "no",
                         (diagnostics.permissions & saccade::platform::macos::diagnostic_input_permission) != 0 ? "yes"
                                                                                                                : "no",
                         static_cast<uint32_t>(diagnostics.surface), diagnostics.surface_reason_bits,
                         diagnostics.surface_epoch, diagnostics.display_count, diagnostics.topology_epoch,
                         static_cast<uint32_t>(diagnostics.source), static_cast<uint32_t>(diagnostics.scope),
                         static_cast<uint32_t>(diagnostics.compute), diagnostics.model.model_stable_id,
                         diagnostics.model.provider_stable_id, diagnostics.model.device_stable_id,
                         diagnostics.model.precision_bits, diagnostics.runtime.scene_status.scene_epoch,
                         diagnostics.runtime.scene_status.target_count, diagnostics.runtime.scene_status.packet_flags,
                         (diagnostics.runtime.scene_status.packet_flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0 ? "yes"
                                                                                                                 : "no",
                         diagnostics.pipeline.failures, diagnostics.runtime.runtime.failures,
                         diagnostics.runtime.neural.failures, diagnostics.runtime.scene.failures];
    } else if (view == saccade::application::DebuggerView::displays) {
        for (uint32_t index = 0; index < diagnostics.display_count; ++index) {
            const auto& display = diagnostics.displays[index].display;
            [text appendFormat:@"Display %u  id %016llx  %ux%u @ %u Hz  rotation %u  flags 0x%x\n"
                                "  desktop (%d,%d) %dx%d  work (%d,%d) %dx%d\n",
                               index, display.display_id, display.backing_width, display.backing_height,
                               display.maximum_fps, static_cast<uint32_t>(display.rotation), display.flags,
                               display.desktop_bounds.x, display.desktop_bounds.y, display.desktop_bounds.width,
                               display.desktop_bounds.height, display.work_bounds.x, display.work_bounds.y,
                               display.work_bounds.width, display.work_bounds.height];
        }
    } else if (view == saccade::application::DebuggerView::runtime) {
        const auto& runtime = diagnostics.runtime;
        const uint64_t average_batch_ns =
            runtime.neural.batches_published == 0
                ? 0
                : runtime.neural.batch_latency_total_ns / runtime.neural.batches_published;
        const uint64_t average_full_scope_ns =
            runtime.neural.batches_published == 0
                ? 0
                : runtime.neural.full_scope_latency_total_ns / runtime.neural.batches_published;
        [text appendFormat:@"Frames offered %llu  replaced %llu  stale %llu\n"
                            "Batches started %llu  published %llu  sources completed %llu  failed %llu\n"
                            "Batch latency avg %llu ns  max %llu ns  missed %llu\n"
                            "Full scope avg %llu ns  max %llu ns  missed %llu\n"
                            "Scene advances %llu  neural %llu  fused %llu  targets %llu\n"
                            "Semantic partial %llu  partial publications %llu  text truncations %llu\n"
                            "Commands %llu  symbols %llu  overlay compositions %llu\n",
                           runtime.neural.frames_offered, runtime.neural.frames_replaced, runtime.neural.frames_stale,
                           runtime.neural.batches_started, runtime.neural.batches_published,
                           runtime.neural.sources_completed, runtime.neural.sources_failed, average_batch_ns,
                           runtime.neural.batch_latency_max_ns, runtime.neural.batch_deadlines_missed,
                           average_full_scope_ns, runtime.neural.full_scope_latency_max_ns,
                           runtime.neural.full_scope_deadlines_missed, runtime.scene.advances,
                           runtime.scene.neural_updates, runtime.scene.fused_publications,
                           runtime.scene.targets_published, runtime.scene.semantic_incomplete,
                           runtime.scene.incomplete_publications, runtime.scene.text_truncated_publications,
                           runtime.runtime.commands, runtime.runtime.symbols, runtime.runtime.overlay_compositions];
    } else if (view == saccade::application::DebuggerView::overlay_gpu) {
        for (uint32_t index = 0; index < diagnostics.display_count; ++index) {
            const auto& display = diagnostics.displays[index];
            [text appendFormat:@"Display %016llx  ticks %llu  rendered %llu  busy %llu  deadlines %llu\n"
                                "  GPU path %u  slots %u  target cap %u  instance cap %u\n"
                                "  submissions %llu  busy %llu  static %llu  active %llu  draw calls %llu\n",
                               display.display.display_id, display.overlay.display_ticks,
                               display.overlay.rendered_frames, display.overlay.busy_frames,
                               display.overlay.deadline_misses, static_cast<uint32_t>(display.gpu.path),
                               display.gpu.slot_count, display.gpu.target_capacity, display.gpu.instance_capacity,
                               display.gpu.submissions, display.gpu.busy_submissions, display.gpu.static_dispatches,
                               display.gpu.active_dispatches, display.gpu.draw_calls];
        }
    } else if (view == saccade::application::DebuggerView::memory) {
        const uint64_t inference =
            diagnostics.inference_memory.device_owned + diagnostics.inference_memory.device_imported;
        [text appendFormat:@"Inference device %llu  host %llu  framework %llu  high water %llu\n"
                            "Preprocess high water %llu  capture high water %llu  overlay known %llu\n",
                           inference, diagnostics.inference_memory.host_committed,
                           diagnostics.inference_memory.framework_opaque, diagnostics.inference_memory.high_water_bytes,
                           diagnostics.preprocess_memory.high_water_bytes, diagnostics.capture_memory.high_water_bytes,
                           diagnostics.overlay.known_memory_bytes];
        for (uint32_t index = 0; index < diagnostics.display_count; ++index)
            [text appendFormat:@"Display %016llx  surface %llu  drawable %llu  total %llu\n",
                               diagnostics.displays[index].display.display_id,
                               diagnostics.displays[index].memory.surface_host_bytes,
                               diagnostics.displays[index].memory.drawable_bytes_estimate,
                               diagnostics.displays[index].memory.total_known_and_estimated];
    } else if (view == saccade::application::DebuggerView::trace) {
        const auto& trace = diagnostics.runtime.trace;
        [text appendFormat:@"Events %u  overwritten %llu  next %llu\n\n", trace.count, trace.overwritten,
                           trace.next_sequence];
        for (uint32_t index = 0; index < trace.count; ++index) {
            const auto& event = trace.events[index];
            [text appendFormat:@"#%llu  time %llu  code %u  result %d  flags 0x%x  argument %llu\n", event.sequence,
                               event.timestamp_ns, static_cast<uint32_t>(event.code), event.result, event.flags,
                               event.argument];
        }
    } else if (view == saccade::application::DebuggerView::frames_transforms) {
        const auto& frames = diagnostics.debugger_frames_transforms;
        [text appendFormat:@"Scene %llu  frame %llu  model %llu  session %llu\n"
                            "Transform %llu  topology %llu  source %llu  targets %u\n"
                            "Bytes %llu  captured %llu ns  transforms %u\n\n",
                           frames.frame.scene.scene_epoch, frames.frame.scene.frame_id, frames.frame.scene.model_epoch,
                           frames.frame.scene.session_epoch, frames.frame.scene.transform_epoch,
                           frames.frame.scene.topology_epoch, frames.frame.scene.source_id,
                           frames.frame.scene.target_count, frames.frame.byte_size, frames.frame.timestamp_ns,
                           frames.transform_count];
        for (uint32_t index = 0; index < frames.transform_count; ++index) {
            const auto& record = frames.transforms[index];
            const auto& transform = record.transform;
            [text appendFormat:@"Transform %u  source %016llx  display %016llx  epoch %llu\n"
                                "  space %u -> %u  rotation %u  flags 0x%x\n"
                                "  source (%d,%d) %dx%d  destination (%d,%d) %dx%d\n",
                               index, record.source_id, record.display_id, transform.epoch,
                               static_cast<uint32_t>(transform.source_space),
                               static_cast<uint32_t>(transform.destination_space),
                               static_cast<uint32_t>(transform.rotation), transform.flags, transform.source.x,
                               transform.source.y, transform.source.width, transform.source.height,
                               transform.destination.x, transform.destination.y, transform.destination.width,
                               transform.destination.height];
        }
    } else {
        const auto& scene = diagnostics.debugger_scene_fusion;
        const auto& targets = scene.targets;
        const auto& fusion = scene.fusion;
        [text appendFormat:@"Scene %llu  frame %llu  targets %u  samples %u  omitted %u\n"
                            "Inputs %u  candidates %llu  written %u  dropped %llu\n"
                            "Buckets %llu  overlap tests %llu  duplicates %llu  safety merges %llu\n"
                            "Sources neural %u  accessibility %u  pixel %u  grid %u  fused %u\n"
                            "State actionable %u  disabled %u  occluded %u  secure %u  approximate %u\n"
                            "Text bytes %u  redacted %u  truncated %u  capabilities 0x%x\n\n",
                           scene.scene.scene_epoch, scene.scene.frame_id, targets.target_count, scene.sample_count,
                           scene.samples_omitted, scene.fusion_input_count, fusion.candidates_read,
                           fusion.targets_written, fusion.capacity_drops, fusion.bucket_visits, fusion.overlap_tests,
                           fusion.duplicates_merged, fusion.safety_merges, targets.neural, targets.accessibility,
                           targets.pixel, targets.grid, targets.fused, targets.actionable, targets.disabled,
                           targets.occluded, targets.secure, targets.approximate, targets.text_bytes,
                           targets.text_redacted, targets.text_truncated, targets.capability_bits];
        for (uint32_t index = 0; index < scene.sample_count; ++index) {
            const auto& target = scene.samples[index];
            [text appendFormat:@"#%u  id %016llx  role %u  source 0x%x  confidence %u  flags 0x%x\n"
                                "  bounds (%d,%d) %dx%d  safe (%d,%d)  window %016llx  order %u\n",
                               index, target.target_id, target.role, target.source_bits, target.confidence_q16,
                               target.flags, target.bounds.x, target.bounds.y, target.bounds.width,
                               target.bounds.height, target.safe_point.x, target.safe_point.y, target.window_id,
                               target.order];
        }
    }
    self.diagnosticsText.string = text;
}

- (void)restart:(id)sender {
    (void)sender;
    (void)application.host_.dispatch(CommandEvent{timestamp_ns(), Command::restart});
}

- (void)quit:(id)sender {
    (void)sender;
    (void)application.host_.dispatch(CommandEvent{timestamp_ns(), Command::quit});
}

- (void)tickRuntime:(NSTimer*)timer {
    (void)timer;
    application.tick(timestamp_ns());
    if (self.diagnosticsPanel.visible && ++self.diagnosticsTicks >= 60) {
        self.diagnosticsTicks = 0;
        [self refreshDiagnostics];
    }
}

- (void)refreshTopology:(NSNotification*)notification {
    (void)notification;
    if (!application.pipeline_initialized_) return;
    const SaccadeResult result = application.pipeline_.refresh_topology();
    if (result != SACCADE_OK) {
        if (result == SACCADE_ERROR_BACKEND)
            application.begin_pipeline_recovery(result, timestamp_ns());
        else {
            application.fault_ = result;
            [self rebuildMenu];
        }
    }
}

- (void)inputUnavailable:(NSNotification*)notification {
    (void)notification;
    if (!application.pipeline_initialized_) return;
    const uint64_t now_ns = timestamp_ns();
    const SaccadeResult result = application.pipeline_.set_input_available(false, now_ns);
    if (result == SACCADE_ERROR_BACKEND) application.begin_pipeline_recovery(result, now_ns);
}

- (void)inputAvailable:(NSNotification*)notification {
    (void)notification;
    if (!application.pipeline_initialized_) return;
    const uint64_t now_ns = timestamp_ns();
    SaccadeResult result = application.pipeline_.refresh_permissions(now_ns);
    if (result == SACCADE_ERROR_BACKEND) {
        application.begin_pipeline_recovery(result, now_ns);
        return;
    }
    result = application.pipeline_.set_input_available(true, now_ns);
    if (result == SACCADE_ERROR_BACKEND) {
        application.begin_pipeline_recovery(result, now_ns);
        return;
    }
    [self refreshTopology:notification];
}

@end

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        SaccadeAppDelegate* delegate = [[SaccadeAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}

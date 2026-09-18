#include "platform/macos/accessibility_provider.hpp"
#include "scene/packet.hpp"

#import <AppKit/AppKit.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

enum class TestResult : int {
    success,
    initialization_failed,
    invalid_window,
    request_failed,
    wait_failed,
    invalid_status,
    collection_failed,
    invalid_packet,
    invalid_provenance,
    invalid_memory_stats,
    invalid_action,
    invalid_action_lifetime,
    invalid_action_refusal,
    invalid_action_outcome,
    invalid_action_capacity,
    ambiguous_window_failed,
    controlled_window_failed,
    minimized_window_failed,
    shutdown_failed
};

constexpr int test_skipped = 77;
constexpr uint32_t maximum_windows_to_probe = 256;
constexpr uint32_t target_capacity = 1000;
constexpr uint64_t wait_timeout_ns = UINT64_C(3000000000);
constexpr uint64_t session_epoch = 7;
constexpr uint64_t transform_epoch = 9;
constexpr uint64_t topology_epoch = 11;
constexpr uint64_t frame_id = 13;
constexpr char window_host_argument[] = "--window-host";
constexpr size_t packet_bytes =
    sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);

enum class HostCommand : uint8_t { minimize = 1, duplicate = 2, hide_button = 3, show_button = 4, quit = 5 };

enum class HostReply : uint8_t {
    minimized = 1,
    duplicated = 2,
    button_hidden = 3,
    button_shown = 4,
    pressed_in_background = 5,
    pressed_while_active = 6
};

struct HostReady {
    uint64_t window_id = 0;
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

template <typename Structure> Structure output_structure() {
    Structure value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

bool read_bytes(int file, void* bytes, size_t size) noexcept {
    auto* destination = static_cast<uint8_t*>(bytes);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(file, destination + offset, size - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool write_bytes(int file, const void* bytes, size_t size) noexcept {
    const auto* source = static_cast<const uint8_t*>(bytes);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(file, source + offset, size - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void stop_application() {
    [NSApp stop:nil];
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
}

} // namespace

@interface QualificationPressTarget : NSObject

@property(nonatomic, assign) int replyFile;

- (void)press:(id)sender;

@end


@implementation QualificationPressTarget

- (void)press:(id)sender {
    (void)sender;
    const HostReply reply = NSApp.isActive ? HostReply::pressed_while_active : HostReply::pressed_in_background;
    (void)write_bytes(self.replyFile, &reply, sizeof(reply));
}

@end

namespace {

int run_window_host(int command_file, int reply_file) {
    (void)[NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(160.0, 160.0, 520.0, 360.0)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (window == nil || window.contentView == nil)
        return result(TestResult::controlled_window_failed);

    window.title = @"Saccade Accessibility Qualification";
    NSTextField* label = [NSTextField labelWithString:@"Controlled accessibility target"];
    label.frame = NSMakeRect(32.0, 288.0, 360.0, 24.0);
    label.accessibilityIdentifier = @"saccade-qualification-label";
    [window.contentView addSubview:label];

    __attribute__((objc_precise_lifetime)) QualificationPressTarget* press_target = [[QualificationPressTarget alloc] init];
    press_target.replyFile = reply_file;
    NSButton* button = [NSButton buttonWithTitle:@"Qualification target" target:press_target action:@selector(press:)];
    button.frame = NSMakeRect(32.0, 220.0, 180.0, 36.0);
    button.accessibilityIdentifier = @"saccade-qualification-button";
    [window.contentView addSubview:button];

    NSButton* disabled = [NSButton buttonWithTitle:@"Disabled target" target:press_target action:@selector(press:)];
    disabled.frame = NSMakeRect(240.0, 220.0, 180.0, 36.0);
    disabled.accessibilityIdentifier = @"saccade-disabled-button";
    disabled.enabled = NO;
    [window.contentView addSubview:disabled];

    NSSecureTextField* secure = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(32.0, 152.0, 180.0, 28.0)];
    secure.accessibilityIdentifier = @"saccade-secure-field";
    secure.stringValue = @"never-retain-this";
    [window.contentView addSubview:secure];

    NSTextField* hidden = [NSTextField labelWithString:@"Hidden target"];
    hidden.frame = NSMakeRect(240.0, 152.0, 180.0, 28.0);
    hidden.accessibilityIdentifier = @"saccade-hidden-target";
    hidden.hidden = YES;
    [window.contentView addSubview:hidden];

    [window orderFrontRegardless];
    [NSApp updateWindows];
    __block NSWindow* duplicate_window = nil;
    const HostReady ready{static_cast<uint64_t>(window.windowNumber)};
    if (ready.window_id == 0 || !write_bytes(reply_file, &ready, sizeof(ready)))
        return result(TestResult::controlled_window_failed);

    dispatch_source_t commands =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(command_file), 0, dispatch_get_main_queue());
    if (commands == nullptr)
        return result(TestResult::controlled_window_failed);
    dispatch_source_set_event_handler(commands, ^{
      HostCommand command{};
      if (!read_bytes(command_file, &command, sizeof(command))) {
          stop_application();
          return;
      }
      if (command == HostCommand::minimize) {
          [window miniaturize:nil];
          [NSApp updateWindows];
          const HostReply reply = HostReply::minimized;
          (void)write_bytes(reply_file, &reply, sizeof(reply));
          return;
      }
      if (command == HostCommand::duplicate) {
          duplicate_window = [[NSWindow alloc] initWithContentRect:window.frame
                                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
          [duplicate_window setFrame:window.frame display:YES];
          duplicate_window.title = window.title;
          [duplicate_window orderFrontRegardless];
          [NSApp updateWindows];
          const HostReply reply = HostReply::duplicated;
          (void)write_bytes(reply_file, &reply, sizeof(reply));
          return;
      }
      if (command == HostCommand::hide_button || command == HostCommand::show_button) {
          button.hidden = command == HostCommand::hide_button;
          [NSApp updateWindows];
          const HostReply reply = button.hidden ? HostReply::button_hidden : HostReply::button_shown;
          (void)write_bytes(reply_file, &reply, sizeof(reply));
          return;
      }
      stop_application();
    });
    dispatch_resume(commands);
    [NSApp run];
    dispatch_source_cancel(commands);
    [duplicate_window orderOut:nil];
    [duplicate_window close];
    [window orderOut:nil];
    [window close];
    close(command_file);
    close(reply_file);
    return result(TestResult::success);
}

class ControlledWindow final {
  public:
    ControlledWindow() = default;

    ~ControlledWindow() { stop(); }

    ControlledWindow(const ControlledWindow&) = delete;
    ControlledWindow& operator=(const ControlledWindow&) = delete;

    bool open(const char* executable) noexcept {
        int command_pipe[2] = {-1, -1};
        int reply_pipe[2] = {-1, -1};
        if (pipe(command_pipe) != 0 || pipe(reply_pipe) != 0) {
            close_pair(command_pipe);
            close_pair(reply_pipe);
            return false;
        }

        posix_spawn_file_actions_t actions{};
        if (posix_spawn_file_actions_init(&actions) != 0) {
            close_pair(command_pipe);
            close_pair(reply_pipe);
            return false;
        }
        (void)posix_spawn_file_actions_addclose(&actions, command_pipe[1]);
        (void)posix_spawn_file_actions_addclose(&actions, reply_pipe[0]);

        std::array<char, 16> command_file{};
        std::array<char, 16> reply_file{};
        (void)std::snprintf(command_file.data(), command_file.size(), "%d", command_pipe[0]);
        (void)std::snprintf(reply_file.data(), reply_file.size(), "%d", reply_pipe[1]);
        char* arguments[] = {const_cast<char*>(executable), const_cast<char*>(window_host_argument), command_file.data(), reply_file.data(),
                             nullptr};
        const int spawned = posix_spawn(&process_, executable, &actions, nullptr, arguments, environ);
        (void)posix_spawn_file_actions_destroy(&actions);
        close(command_pipe[0]);
        close(reply_pipe[1]);
        if (spawned != 0) {
            close(command_pipe[1]);
            close(reply_pipe[0]);
            process_ = -1;
            return false;
        }

        command_file_ = command_pipe[1];
        reply_file_ = reply_pipe[0];
        HostReady ready{};
        if (!read_bytes(reply_file_, &ready, sizeof(ready)) || ready.window_id == 0) {
            stop();
            return false;
        }
        window_id_ = ready.window_id;
        return true;
    }

    [[nodiscard]] uint64_t id() const noexcept { return window_id_; }

    [[nodiscard]] uint64_t process_id() const noexcept { return static_cast<uint64_t>(process_); }

    bool minimize() noexcept {
        const HostCommand command = HostCommand::minimize;
        HostReply reply{};
        return write_bytes(command_file_, &command, sizeof(command)) && read_bytes(reply_file_, &reply, sizeof(reply)) &&
               reply == HostReply::minimized;
    }

    bool duplicate() noexcept { return command(HostCommand::duplicate, HostReply::duplicated); }

    bool wait_for_background_press() noexcept {
        HostReply reply{};
        return read_bytes(reply_file_, &reply, sizeof(reply)) && reply == HostReply::pressed_in_background;
    }

    bool set_button_hidden(bool hidden) noexcept {
        return command(hidden ? HostCommand::hide_button : HostCommand::show_button,
                       hidden ? HostReply::button_hidden : HostReply::button_shown);
    }

  private:
    bool command(HostCommand command_value, HostReply expected) noexcept {
        HostReply reply{};
        return write_bytes(command_file_, &command_value, sizeof(command_value)) && read_bytes(reply_file_, &reply, sizeof(reply)) &&
               reply == expected;
    }

    static void close_pair(int (&files)[2]) noexcept {
        for (int& file : files) {
            if (file >= 0)
                close(file);
            file = -1;
        }
    }

    void stop() noexcept {
        if (process_ <= 0)
            return;
        const HostCommand command = HostCommand::quit;
        (void)write_bytes(command_file_, &command, sizeof(command));
        close(command_file_);
        close(reply_file_);
        command_file_ = -1;
        reply_file_ = -1;
        int status = 0;
        while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {}
        process_ = -1;
        window_id_ = 0;
    }

    pid_t process_ = -1;
    int command_file_ = -1;
    int reply_file_ = -1;
    uint64_t window_id_ = 0;
};

bool enumerates_window(const SaccadeAccessibilityProviderDesc& desc, uint64_t window_id) noexcept {
    for (uint32_t index = 0; index < maximum_windows_to_probe; ++index) {
        SaccadeWindowInfo window = output_structure<SaccadeWindowInfo>();
        const SaccadeResult result = desc.ops.enumerate_windows(desc.context, index, &window);
        if (result == SACCADE_ERROR_NOT_FOUND)
            return false;
        if (result != SACCADE_OK)
            return false;
        if (window.stable_id == window_id)
            return true;
    }
    return false;
}

bool wait_for_window(const SaccadeAccessibilityProviderDesc& desc, uint64_t window_id) noexcept {
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + wait_timeout_ns;
    do {
        if (enumerates_window(desc, window_id))
            return true;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    return false;
}

const SaccadeTargetRecord* target_named(const saccade::scene::PacketView& view, const char* name) noexcept {
    const size_t length = std::strlen(name);
    for (uint32_t index = 0; index < view.header->target_count; ++index) {
        const SaccadeSpanU8 text = view.target_text(index);
        if (text.size == length && std::memcmp(text.data, name, length) == 0)
            return &view.targets[index];
    }
    return nullptr;
}

saccade::platform::macos::AccessibilityGenerationKey generation(uint64_t process_id, uint64_t window_id,
                                                                uint64_t generation_frame_id) noexcept {
    return {session_epoch, generation_frame_id, transform_epoch, topology_epoch, process_id, window_id};
}

saccade::platform::macos::AccessibilityPressRequest press_request(const saccade::platform::macos::AccessibilityGenerationKey& key,
                                                                  const SaccadeTargetRecord& target) noexcept {
    return {key, target.target_id, {target.x_q8, target.y_q8, target.width_q8, target.height_q8}};
}

struct PressHook {
    std::atomic<int32_t> next_result{kAXErrorSuccess};
    std::atomic<uint32_t> calls{0};
    std::atomic<bool> block_before_perform{false};
    std::atomic<bool> entered_before_perform{false};
    std::atomic<bool> release_before_perform{false};
    std::atomic<bool> block_in_perform{false};
    std::atomic<bool> entered_perform{false};
    std::atomic<bool> release_perform{false};
};

void before_perform_press(void* context) noexcept {
    auto& hook = *static_cast<PressHook*>(context);
    if (!hook.block_before_perform.load(std::memory_order_acquire))
        return;
    hook.entered_before_perform.store(true, std::memory_order_release);
    while (!hook.release_before_perform.load(std::memory_order_acquire))
        sched_yield();
}

AXError perform_press(void* context, AXUIElementRef element) noexcept {
    auto& hook = *static_cast<PressHook*>(context);
    hook.calls.fetch_add(1, std::memory_order_relaxed);
    if (hook.block_in_perform.load(std::memory_order_acquire)) {
        hook.entered_perform.store(true, std::memory_order_release);
        while (!hook.release_perform.load(std::memory_order_acquire))
            sched_yield();
    }
    const AXError result = static_cast<AXError>(hook.next_result.load(std::memory_order_acquire));
    return result == kAXErrorSuccess ? AXUIElementPerformAction(element, kAXPressAction) : result;
}

bool wait_until(const std::atomic<bool>& value) noexcept {
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + wait_timeout_ns;
    do {
        if (value.load(std::memory_order_acquire))
            return true;
        sched_yield();
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    return false;
}

bool await_press(saccade::platform::macos::AccessibilityProvider& provider, SaccadeTicketHandle ticket,
                 saccade::platform::macos::AccessibilityPressStatus* output) noexcept {
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + wait_timeout_ns;
    do {
        const SaccadeResult polled = provider.poll_press(ticket, output);
        if (polled != SACCADE_OK)
            return false;
        if (output->state != SACCADE_TICKET_QUEUED && output->state != SACCADE_TICKET_RUNNING)
            return true;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    return false;
}

bool expect_press_result(saccade::platform::macos::AccessibilityProvider& provider,
                         const saccade::platform::macos::AccessibilityPressRequest& request, SaccadeAgentResult expected_result,
                         uint32_t expected_attempts) noexcept {
    SaccadeTicketHandle ticket = 0;
    saccade::platform::macos::AccessibilityPressStatus status{};
    const SaccadeResult requested = provider.request_press(request, &ticket);
    const bool waited = requested == SACCADE_OK && ticket != 0 && await_press(provider, ticket, &status);
    return waited && status.state == SACCADE_TICKET_COMPLETE && status.result == expected_result &&
           status.attempt_count == expected_attempts;
}

} // namespace

int run_test(const char* executable) {
    PressHook press_hook{};
    const saccade::platform::macos::AccessibilityActionHooks action_hooks{&press_hook, &perform_press, &before_perform_press};
    saccade::platform::macos::AccessibilityProvider provider;
    if (provider.initialize(&action_hooks) != SACCADE_OK)
        return result(TestResult::initialization_failed);
    if (!provider.permission_granted())
        return test_skipped;

    ControlledWindow controlled_window;
    if (!controlled_window.open(executable))
        return result(TestResult::controlled_window_failed);

    const SaccadeAccessibilityProviderDesc desc = provider.descriptor();
    if (!wait_for_window(desc, controlled_window.id()))
        return result(TestResult::controlled_window_failed);
    alignas(SaccadeTargetPacketHeader) static std::array<uint8_t, packet_bytes> packet{};

    for (uint32_t window_index = 0; window_index < maximum_windows_to_probe; ++window_index) {
        SaccadeWindowInfo window = output_structure<SaccadeWindowInfo>();
        const SaccadeResult enumerated = desc.ops.enumerate_windows(desc.context, window_index, &window);
        if (enumerated == SACCADE_ERROR_NOT_FOUND)
            break;
        if (enumerated != SACCADE_OK || window.stable_id == 0 || window.process_id == 0 || window.desktop_bounds.width <= 0 ||
            window.desktop_bounds.height <= 0) {
            return result(TestResult::invalid_window);
        }
        if (window.process_id != controlled_window.process_id() || window.stable_id != controlled_window.id()) {
            continue;
        }

        SaccadeAccessibilityQueryDesc query{};
        query.struct_size = sizeof(query);
        query.api_version = SACCADE_API_VERSION;
        query.window_id = window.stable_id;
        query.scope = window.desktop_bounds;
        query.target_capacity = target_capacity;
        query.session_epoch = session_epoch;
        query.transform_epoch = transform_epoch;
        query.topology_epoch = topology_epoch;
        query.frame_id = frame_id;
        SaccadeTicketHandle ticket = 0;
        if (desc.ops.request(desc.context, &query, &ticket) != SACCADE_OK || ticket == 0)
            return result(TestResult::request_failed);
        SaccadeAccessibilityStatus status = output_structure<SaccadeAccessibilityStatus>();
        const SaccadeResult waited = desc.ops.wait(desc.context, ticket, wait_timeout_ns, &status);
        if (waited == SACCADE_ERROR_TIMEOUT) {
            desc.ops.cancel(desc.context, ticket);
            desc.ops.synchronize(desc.context, wait_timeout_ns);
            return result(TestResult::wait_failed);
        }
        if (waited != SACCADE_OK)
            return result(TestResult::wait_failed);
        if (status.state != SACCADE_TICKET_COMPLETE || status.snapshot == 0 || status.session_epoch != session_epoch ||
            status.transform_epoch != transform_epoch || status.topology_epoch != topology_epoch || status.frame_id != frame_id) {
            return result(TestResult::invalid_status);
        }
        if (status.target_count == 0) {
            (void)desc.ops.release(desc.context, status.snapshot);
            return result(TestResult::invalid_packet);
        }
        size_t required = 0;
        if (desc.ops.collect(desc.context, status.snapshot, {}, &required) != SACCADE_ERROR_CAPACITY || required > packet.size() ||
            required != status.required_bytes ||
            desc.ops.collect(desc.context, status.snapshot, {packet.data(), packet.size()}, &required) != SACCADE_OK) {
            return result(TestResult::collection_failed);
        }
        saccade::scene::PacketView view{};
        if (saccade::scene::validate_packet({packet.data(), required}, &view) != SACCADE_OK ||
            view.header->target_count != status.target_count || view.header->source_id != window.stable_id) {
            return result(TestResult::invalid_packet);
        }
        for (uint32_t index = 0; index < view.header->target_count; ++index) {
            if ((view.targets[index].source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) == 0)
                return result(TestResult::invalid_provenance);
        }
        SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
        saccade::platform::macos::AccessibilityProviderStats stats{};
        if (desc.ops.memory_stats(desc.context, &memory) != SACCADE_OK || memory.host_committed < packet_bytes ||
            provider.read_stats(&stats) != SACCADE_OK || stats.completed == 0 || stats.elements == 0 || stats.targets == 0) {
            return result(TestResult::invalid_memory_stats);
        }

        const auto first_generation = generation(controlled_window.process_id(), controlled_window.id(), frame_id);
        const SaccadeTargetRecord* button = target_named(view, "Qualification target");
        const SaccadeTargetRecord* disabled = target_named(view, "Disabled target");
        const SaccadeTargetRecord* hidden = target_named(view, "Hidden target");
        const SaccadeTargetRecord* no_press = nullptr;
        const SaccadeTargetRecord* secure = nullptr;
        for (uint32_t index = 0; index < view.header->target_count; ++index) {
            if ((view.targets[index].flags & SACCADE_TARGET_SECURE) != 0)
                secure = &view.targets[index];
            if ((view.targets[index].flags & SACCADE_TARGET_ACTIONABLE) != 0 &&
                (view.targets[index].capability_bits & SACCADE_TARGET_CAPABILITY_INVOKE) == 0)
                no_press = &view.targets[index];
        }
        if (button == nullptr || disabled == nullptr || hidden != nullptr || no_press == nullptr || secure == nullptr ||
            provider.promote_action_generation(first_generation) != SACCADE_OK ||
            desc.ops.release(desc.context, status.snapshot) != SACCADE_OK) {
            return result(TestResult::invalid_action);
        }

        const auto button_request = press_request(first_generation, *button);
        if (!expect_press_result(provider, button_request, SACCADE_AGENT_OK, 1))
            return result(TestResult::invalid_action);
        if (!controlled_window.wait_for_background_press())
            return result(TestResult::invalid_action);
        const uint32_t before_cancellation = press_hook.calls.load(std::memory_order_relaxed);
        press_hook.block_before_perform.store(true, std::memory_order_release);
        press_hook.entered_before_perform.store(false, std::memory_order_relaxed);
        press_hook.release_before_perform.store(false, std::memory_order_relaxed);
        SaccadeTicketHandle cancelled_ticket = 0;
        saccade::platform::macos::AccessibilityPressStatus cancelled_status{};
        if (provider.request_press(button_request, &cancelled_ticket) != SACCADE_OK || cancelled_ticket == 0 ||
            !wait_until(press_hook.entered_before_perform) || provider.cancel_press(cancelled_ticket) != SACCADE_OK) {
            return result(TestResult::invalid_action_outcome);
        }
        press_hook.release_before_perform.store(true, std::memory_order_release);
        if (provider.wait_press(cancelled_ticket, wait_timeout_ns, &cancelled_status) != SACCADE_OK ||
            cancelled_status.result != SACCADE_AGENT_ERROR_CANCELLED || cancelled_status.attempt_count != 0 ||
            press_hook.calls.load(std::memory_order_relaxed) != before_cancellation) {
            return result(TestResult::invalid_action_outcome);
        }
        press_hook.block_before_perform.store(false, std::memory_order_release);

        press_hook.next_result.store(kAXErrorCannotComplete, std::memory_order_release);
        press_hook.block_in_perform.store(true, std::memory_order_release);
        press_hook.entered_perform.store(false, std::memory_order_relaxed);
        press_hook.release_perform.store(false, std::memory_order_relaxed);
        cancelled_ticket = 0;
        cancelled_status = {};
        if (provider.request_press(button_request, &cancelled_ticket) != SACCADE_OK || cancelled_ticket == 0 ||
            !wait_until(press_hook.entered_perform) || provider.cancel_press(cancelled_ticket) != SACCADE_OK) {
            return result(TestResult::invalid_action_outcome);
        }
        press_hook.release_perform.store(true, std::memory_order_release);
        if (provider.wait_press(cancelled_ticket, wait_timeout_ns, &cancelled_status) != SACCADE_OK ||
            cancelled_status.result != SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED || cancelled_status.attempt_count != 1 ||
            press_hook.calls.load(std::memory_order_relaxed) != before_cancellation + 1) {
            return result(TestResult::invalid_action_outcome);
        }
        press_hook.block_in_perform.store(false, std::memory_order_release);
        press_hook.next_result.store(kAXErrorSuccess, std::memory_order_release);

        const uint32_t before_hidden = press_hook.calls.load(std::memory_order_relaxed);
        if (!controlled_window.set_button_hidden(true) ||
            !expect_press_result(provider, button_request, SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, 0) ||
            press_hook.calls.load(std::memory_order_relaxed) != before_hidden || !controlled_window.set_button_hidden(false)) {
            return result(TestResult::invalid_action_refusal);
        }

        auto stale = button_request;
        stale.generation.frame_id += 1;
        const uint32_t before_refusals = press_hook.calls.load(std::memory_order_relaxed);
        if (!expect_press_result(provider, stale, SACCADE_AGENT_ERROR_STALE_GENERATION, 0))
            return result(TestResult::invalid_action_refusal);
        stale = button_request;
        stale.generation.process_id += 1;
        if (!expect_press_result(provider, stale, SACCADE_AGENT_ERROR_STALE_GENERATION, 0))
            return result(TestResult::invalid_action_refusal);
        stale = button_request;
        stale.generation.window_id += 1;
        if (!expect_press_result(provider, stale, SACCADE_AGENT_ERROR_STALE_GENERATION, 0))
            return result(TestResult::invalid_action_refusal);
        stale = button_request;
        stale.bounds_q8.x += 1;
        if (!expect_press_result(provider, stale, SACCADE_AGENT_ERROR_STALE_GENERATION, 0))
            return result(TestResult::invalid_action_refusal);
        auto missing = button_request;
        missing.target_id ^= UINT64_C(0x9e3779b97f4a7c15);
        if (!expect_press_result(provider, missing, SACCADE_AGENT_ERROR_TARGET_NOT_FOUND, 0) ||
            !expect_press_result(provider, press_request(first_generation, *disabled), SACCADE_AGENT_ERROR_TARGET_INACCESSIBLE, 0) ||
            !expect_press_result(provider, press_request(first_generation, *secure), SACCADE_AGENT_ERROR_SECURE_SURFACE, 0) ||
            !expect_press_result(provider, press_request(first_generation, *no_press), SACCADE_AGENT_ERROR_ACTION_UNSUPPORTED, 0) ||
            press_hook.calls.load(std::memory_order_relaxed) != before_refusals) {
            return result(TestResult::invalid_action_refusal);
        }

        press_hook.next_result.store(kAXErrorCannotComplete, std::memory_order_release);
        if (!expect_press_result(provider, button_request, SACCADE_AGENT_ERROR_OUTCOME_UNCONFIRMED, 1) ||
            press_hook.calls.load(std::memory_order_relaxed) != before_refusals + 1) {
            return result(TestResult::invalid_action_outcome);
        }
        press_hook.next_result.store(kAXErrorSuccess, std::memory_order_release);

        query.frame_id = frame_id + 1;
        if (desc.ops.request(desc.context, &query, &ticket) != SACCADE_OK || ticket == 0 ||
            desc.ops.wait(desc.context, ticket, wait_timeout_ns, &status) != SACCADE_OK || status.state != SACCADE_TICKET_COMPLETE ||
            provider.promote_action_generation(generation(controlled_window.process_id(), controlled_window.id(), query.frame_id)) !=
                SACCADE_OK ||
            desc.ops.release(desc.context, status.snapshot) != SACCADE_OK ||
            !expect_press_result(provider, button_request, SACCADE_AGENT_OK, 1) || !controlled_window.wait_for_background_press()) {
            return result(TestResult::invalid_action_lifetime);
        }

        query.frame_id = frame_id + 2;
        if (desc.ops.request(desc.context, &query, &ticket) != SACCADE_ERROR_CAPACITY ||
            provider.retire_action_generation(first_generation) != SACCADE_OK ||
            !expect_press_result(provider, button_request, SACCADE_AGENT_ERROR_STALE_GENERATION, 0) ||
            provider.retire_action_session(session_epoch) != SACCADE_OK ||
            !expect_press_result(provider,
                                 press_request(generation(controlled_window.process_id(), controlled_window.id(), frame_id + 1), *button),
                                 SACCADE_AGENT_ERROR_STALE_GENERATION, 0)) {
            return result(TestResult::invalid_action_capacity);
        }

        if (!controlled_window.duplicate())
            return result(TestResult::ambiguous_window_failed);
        if (!enumerates_window(desc, controlled_window.id()))
            return result(TestResult::ambiguous_window_failed);
        query.frame_id = frame_id + 3;
        const SaccadeResult ambiguous_requested = desc.ops.request(desc.context, &query, &ticket);
        const SaccadeResult ambiguous_waited =
            ambiguous_requested == SACCADE_OK ? desc.ops.wait(desc.context, ticket, wait_timeout_ns, &status) : ambiguous_requested;
        if (ambiguous_requested != SACCADE_OK || ambiguous_waited != SACCADE_OK || status.state != SACCADE_TICKET_FAILED ||
            status.result != SACCADE_ERROR_NOT_FOUND) {
            return result(TestResult::ambiguous_window_failed);
        }
        if (!controlled_window.minimize() || !enumerates_window(desc, controlled_window.id()))
            return result(TestResult::minimized_window_failed);
        return provider.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::shutdown_failed);
    }
    return result(TestResult::controlled_window_failed);
}

int main(int argument_count, char** arguments) {
    @autoreleasepool {
        (void)signal(SIGPIPE, SIG_IGN);
        if (argument_count == 4 && std::strcmp(arguments[1], window_host_argument) == 0) {
            char* command_end = nullptr;
            char* reply_end = nullptr;
            const long command_file = std::strtol(arguments[2], &command_end, 10);
            const long reply_file = std::strtol(arguments[3], &reply_end, 10);
            if (command_end == arguments[2] || *command_end != '\0' || reply_end == arguments[3] || *reply_end != '\0' ||
                command_file < 0 || command_file > INT_MAX || reply_file < 0 || reply_file > INT_MAX) {
                return result(TestResult::controlled_window_failed);
            }
            return run_window_host(static_cast<int>(command_file), static_cast<int>(reply_file));
        }
        return run_test(arguments[0]);
    }
}

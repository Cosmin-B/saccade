#include "platform/macos/accessibility_provider.hpp"
#include "scene/packet.hpp"

#import <AppKit/AppKit.h>

#include <array>
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
constexpr size_t packet_bytes = sizeof(SaccadeTargetPacketHeader) +
                                static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);

enum class HostCommand : uint8_t { minimize = 1, quit = 2 };

enum class HostReply : uint8_t { minimized = 1 };

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
        if (count < 0 && errno == EINTR) continue;
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
        if (count < 0 && errno == EINTR) continue;
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

int run_window_host(int command_file, int reply_file) {
    (void)[NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(160.0, 160.0, 520.0, 220.0)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (window == nil || window.contentView == nil) return result(TestResult::controlled_window_failed);

    window.title = @"Saccade Accessibility Qualification";
    NSTextField* label = [NSTextField labelWithString:@"Controlled accessibility target"];
    label.frame = NSMakeRect(32.0, 140.0, 360.0, 24.0);
    label.accessibilityIdentifier = @"saccade-qualification-label";
    [window.contentView addSubview:label];

    NSButton* button = [NSButton buttonWithTitle:@"Qualification target" target:nil action:nil];
    button.frame = NSMakeRect(32.0, 72.0, 180.0, 36.0);
    button.accessibilityIdentifier = @"saccade-qualification-button";
    [window.contentView addSubview:button];

    [window orderFrontRegardless];
    [NSApp updateWindows];
    const HostReady ready{static_cast<uint64_t>(window.windowNumber)};
    if (ready.window_id == 0 || !write_bytes(reply_file, &ready, sizeof(ready)))
        return result(TestResult::controlled_window_failed);

    dispatch_source_t commands = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(command_file),
                                                        0, dispatch_get_main_queue());
    if (commands == nullptr) return result(TestResult::controlled_window_failed);
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
      stop_application();
    });
    dispatch_resume(commands);
    [NSApp run];
    dispatch_source_cancel(commands);
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
        char* arguments[] = {const_cast<char*>(executable), const_cast<char*>(window_host_argument),
                             command_file.data(), reply_file.data(), nullptr};
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
        return write_bytes(command_file_, &command, sizeof(command)) &&
               read_bytes(reply_file_, &reply, sizeof(reply)) && reply == HostReply::minimized;
    }

  private:
    static void close_pair(int (&files)[2]) noexcept {
        for (int& file : files) {
            if (file >= 0) close(file);
            file = -1;
        }
    }

    void stop() noexcept {
        if (process_ <= 0) return;
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
        if (result == SACCADE_ERROR_NOT_FOUND) return false;
        if (result != SACCADE_OK) return false;
        if (window.stable_id == window_id) return true;
    }
    return false;
}

} // namespace

int run_test(const char* executable) {
    saccade::platform::macos::AccessibilityProvider provider;
    if (provider.initialize() != SACCADE_OK) return result(TestResult::initialization_failed);
    if (!provider.permission_granted()) return test_skipped;

    ControlledWindow controlled_window;
    if (!controlled_window.open(executable)) return result(TestResult::controlled_window_failed);

    const SaccadeAccessibilityProviderDesc desc = provider.descriptor();
    alignas(SaccadeTargetPacketHeader) static std::array<uint8_t, packet_bytes> packet{};

    for (uint32_t window_index = 0; window_index < maximum_windows_to_probe; ++window_index) {
        SaccadeWindowInfo window = output_structure<SaccadeWindowInfo>();
        const SaccadeResult enumerated = desc.ops.enumerate_windows(desc.context, window_index, &window);
        if (enumerated == SACCADE_ERROR_NOT_FOUND) break;
        if (enumerated != SACCADE_OK || window.stable_id == 0 || window.process_id == 0 ||
            window.desktop_bounds.width <= 0 || window.desktop_bounds.height <= 0) {
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
        if (waited != SACCADE_OK) return result(TestResult::wait_failed);
        if (status.state != SACCADE_TICKET_COMPLETE || status.snapshot == 0 || status.session_epoch != session_epoch ||
            status.transform_epoch != transform_epoch || status.topology_epoch != topology_epoch ||
            status.frame_id != frame_id) {
            return result(TestResult::invalid_status);
        }
        if (status.target_count == 0) {
            (void)desc.ops.release(desc.context, status.snapshot);
            return result(TestResult::invalid_packet);
        }
        size_t required = 0;
        if (desc.ops.collect(desc.context, status.snapshot, {}, &required) != SACCADE_ERROR_CAPACITY ||
            required > packet.size() || required != status.required_bytes ||
            desc.ops.collect(desc.context, status.snapshot, {packet.data(), packet.size()}, &required) != SACCADE_OK) {
            return result(TestResult::collection_failed);
        }
        saccade::scene::PacketView view{};
        if (saccade::scene::validate_packet({packet.data(), required}, &view) != SACCADE_OK ||
            view.header->target_count != status.target_count || view.header->source_id != window.stable_id ||
            desc.ops.release(desc.context, status.snapshot) != SACCADE_OK) {
            return result(TestResult::invalid_packet);
        }
        for (uint32_t index = 0; index < view.header->target_count; ++index) {
            if ((view.targets[index].source_bits & SACCADE_TARGET_SOURCE_ACCESSIBILITY) == 0)
                return result(TestResult::invalid_provenance);
        }
        SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
        saccade::platform::macos::AccessibilityProviderStats stats{};
        if (desc.ops.memory_stats(desc.context, &memory) != SACCADE_OK || memory.host_committed < packet_bytes ||
            provider.read_stats(&stats) != SACCADE_OK || stats.completed == 0 || stats.elements == 0 ||
            stats.targets == 0) {
            return result(TestResult::invalid_memory_stats);
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
            if (command_end == arguments[2] || *command_end != '\0' || reply_end == arguments[3] ||
                *reply_end != '\0' || command_file < 0 || command_file > INT_MAX || reply_file < 0 ||
                reply_file > INT_MAX) {
                return result(TestResult::controlled_window_failed);
            }
            return run_window_host(static_cast<int>(command_file), static_cast<int>(reply_file));
        }
        return run_test(arguments[0]);
    }
}

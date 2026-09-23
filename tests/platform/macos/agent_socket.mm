#include "platform/macos/agent_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class TestResult : int {
    success,
    initialization_failed,
    connection_failed,
    hello_write_failed,
    hello_read_failed,
    request_write_failed,
    request_read_failed,
    pending_request_failed,
    pending_disconnect_failed,
    shutdown_failed,
    stale_endpoint_failed,
    duplicate_listener_not_rejected,
    replacement_endpoint_removed,
    listener_handoff_failed,
    handoff_connection_failed,
    process_handoff_failed
};

enum class HandoffSignal : uint8_t { ready = UINT8_C(0x51), release = UINT8_C(0xa7) };

struct RequestSink {
    uint32_t attempts = 0;
    uint32_t requests = 0;
    uint32_t disconnects = 0;
    uint32_t busy_responses_remaining = 0;
    bool always_busy = false;
    bool action_pending = false;
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult process_request(void* context, SaccadeSpanU8 input, SaccadeAgentCapabilityBits capabilities, uint64_t now_ns,
                              SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (input.size != sizeof(SaccadeAgentObserveRequest) || output.size < sizeof(SaccadeAgentObserveCompletion) ||
        capabilities != SACCADE_AGENT_CAPABILITY_OBSERVE || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    auto* sink = static_cast<RequestSink*>(context);
    ++sink->attempts;
    if (sink->always_busy || sink->busy_responses_remaining != 0) {
        sink->action_pending = sink->always_busy;
        if (sink->busy_responses_remaining != 0)
            --sink->busy_responses_remaining;
        *output_size = 0;
        return SACCADE_ERROR_BUSY;
    }
    SaccadeAgentObserveRequest request{};
    std::memcpy(&request, input.data, sizeof(request));
    SaccadeAgentObserveCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_COMPLETION;
    completion.request_id = request.request_id;
    completion.total_size = static_cast<uint32_t>(sizeof(completion));
    std::memcpy(output.data, &completion, sizeof(completion));
    *output_size = sizeof(completion);
    ++sink->requests;
    return SACCADE_OK;
}

SaccadeResult disconnect(void* context) noexcept {
    auto* sink = static_cast<RequestSink*>(context);
    ++sink->disconnects;
    sink->action_pending = false;
    return SACCADE_OK;
}

bool write_frame(int client, const void* message, uint32_t size) noexcept {
    return send(client, &size, sizeof(size), 0) == sizeof(size) && send(client, message, size, 0) == size;
}

bool leave_stale_endpoint(const char* endpoint) noexcept {
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint, std::strlen(endpoint) + 1U);
    (void)unlink(endpoint);
    const bool bound = bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    close(listener);
    return bound;
}

bool transmit_signal(int descriptor, HandoffSignal signal) noexcept {
    return write(descriptor, &signal, sizeof(signal)) == sizeof(signal);
}

bool receive_signal(int descriptor, HandoffSignal expected) noexcept {
    HandoffSignal signal{};
    return read(descriptor, &signal, sizeof(signal)) == sizeof(signal) && signal == expected;
}

bool cross_process_handoff(const char* endpoint) noexcept {
    int ready[2]{};
    int release[2]{};
    if (pipe(ready) != 0 || pipe(release) != 0)
        return false;

    const pid_t owner = fork();
    if (owner < 0)
        return false;
    if (owner == 0) {
        close(ready[0]);
        close(release[1]);

        const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpoint, std::strlen(endpoint) + 1U);
        (void)unlink(endpoint);
        const bool listening =
            listener >= 0 && bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0;
        const bool synchronized =
            listening && transmit_signal(ready[1], HandoffSignal::ready) && receive_signal(release[0], HandoffSignal::release);
        if (listener >= 0)
            close(listener);
        (void)unlink(endpoint);
        close(ready[1]);
        close(release[0]);
        _exit(synchronized ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    close(ready[1]);
    close(release[0]);
    const bool owner_ready = receive_signal(ready[0], HandoffSignal::ready);

    RequestSink sink{};
    saccade::platform::macos::AgentSocket replacement;
    static saccade::platform::macos::AgentSocketStorage replacement_storage;
    const SaccadeResult collision =
        replacement.initialize({&sink, process_request, disconnect, endpoint, SACCADE_AGENT_CAPABILITY_OBSERVE}, &replacement_storage);
    const bool released = owner_ready && transmit_signal(release[1], HandoffSignal::release);
    close(ready[0]);
    close(release[1]);

    int owner_status = 0;
    const bool owner_stopped =
        waitpid(owner, &owner_status, 0) == owner && WIFEXITED(owner_status) && WEXITSTATUS(owner_status) == EXIT_SUCCESS;
    if (!owner_ready || collision != SACCADE_ERROR_ALREADY_EXISTS || !released || !owner_stopped)
        return false;

    if (replacement.initialize({&sink, process_request, disconnect, endpoint, SACCADE_AGENT_CAPABILITY_OBSERVE}, &replacement_storage) !=
        SACCADE_OK)
        return false;

    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint, std::strlen(endpoint) + 1U);
    constexpr uint64_t handoff_time_ns = 1;
    const bool connected = client >= 0 && connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                           replacement.advance(handoff_time_ns) == SACCADE_OK && replacement.stats().connections == 1;
    if (client >= 0)
        close(client);
    return connected && replacement.shutdown() == SACCADE_OK;
}

template <typename T> bool read_response(saccade::platform::macos::AgentSocket* server, int client, uint64_t* now_ns, T* output) noexcept {
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
        if (server->advance((*now_ns)++) != SACCADE_OK)
            return false;
        int available = 0;
        if (ioctl(client, FIONREAD, &available) != 0)
            return false;
        if (available < static_cast<int>(sizeof(uint32_t) + sizeof(T))) {
            usleep(1000);
            continue;
        }
        uint32_t size = 0;
        if (recv(client, &size, sizeof(size), MSG_WAITALL) != sizeof(size) || size != sizeof(T))
            return false;
        return recv(client, output, sizeof(T), MSG_WAITALL) == sizeof(T);
    }
    return false;
}

} // namespace

int main() {
    std::array<char, 104> endpoint{};
    std::array<char, 104> retired_endpoint{};
    std::array<char, 104> process_endpoint{};
    if (snprintf(endpoint.data(), endpoint.size(), "/tmp/saccade-agent-test-%u-%d.sock", geteuid(), getpid()) <= 0)
        return result(TestResult::initialization_failed);
    if (snprintf(retired_endpoint.data(), retired_endpoint.size(), "/tmp/saccade-agent-old-%u-%d.sock", geteuid(), getpid()) <= 0)
        return result(TestResult::initialization_failed);
    if (snprintf(process_endpoint.data(), process_endpoint.size(), "/tmp/saccade-agent-process-%u-%d.sock", geteuid(), getpid()) <= 0)
        return result(TestResult::initialization_failed);
    if (!cross_process_handoff(process_endpoint.data()))
        return result(TestResult::process_handoff_failed);
    if (!leave_stale_endpoint(endpoint.data()))
        return result(TestResult::stale_endpoint_failed);

    RequestSink sink{};
    saccade::platform::macos::AgentSocket server;
    saccade::platform::macos::AgentSocket contender;
    static saccade::platform::macos::AgentSocketStorage storage;
    static saccade::platform::macos::AgentSocketStorage contender_storage;
    if (server.initialize({&sink, process_request, disconnect, endpoint.data(), SACCADE_AGENT_CAPABILITY_OBSERVE}, &storage) != SACCADE_OK)
        return result(TestResult::initialization_failed);
    if (contender.initialize({&sink, process_request, disconnect, endpoint.data(), SACCADE_AGENT_CAPABILITY_OBSERVE}, &contender_storage) !=
        SACCADE_ERROR_ALREADY_EXISTS)
        return result(TestResult::duplicate_listener_not_rejected);
    uint64_t now_ns = 1;
    if (server.advance(now_ns++) != SACCADE_OK || server.advance(now_ns++) != SACCADE_OK)
        return result(TestResult::connection_failed);

    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0)
        return result(TestResult::connection_failed);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.data(), std::strlen(endpoint.data()) + 1U);
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return result(TestResult::connection_failed);
    if (server.advance(now_ns++) != SACCADE_OK)
        return result(TestResult::connection_failed);

    SaccadeAgentHelloRequest hello{};
    hello.header.struct_size = static_cast<uint32_t>(sizeof(hello));
    hello.header.api_version = SACCADE_AGENT_API_VERSION;
    hello.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_REQUEST;
    hello.request_id = 7;
    hello.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (!write_frame(client, &hello, sizeof(hello)))
        return result(TestResult::hello_write_failed);
    SaccadeAgentHelloCompletion hello_completion{};
    if (!read_response(&server, client, &now_ns, &hello_completion) || hello_completion.request_id != hello.request_id ||
        hello_completion.result != SACCADE_AGENT_OK || hello_completion.granted_capability_bits != SACCADE_AGENT_CAPABILITY_OBSERVE)
        return result(TestResult::hello_read_failed);

    SaccadeAgentObserveRequest request{};
    request.header.struct_size = static_cast<uint32_t>(sizeof(request));
    request.header.api_version = SACCADE_AGENT_API_VERSION;
    request.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request.request_id = 9;
    request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (!write_frame(client, &request, sizeof(request)))
        return result(TestResult::request_write_failed);
    SaccadeAgentObserveCompletion completion{};
    if (!read_response(&server, client, &now_ns, &completion) || completion.request_id != request.request_id ||
        completion.result != SACCADE_AGENT_OK || sink.requests != 1 || sink.attempts != 1)
        return result(TestResult::request_read_failed);

    sink.busy_responses_remaining = 3;
    request.request_id = 10;
    if (!write_frame(client, &request, sizeof(request)) || !read_response(&server, client, &now_ns, &completion) ||
        completion.request_id != request.request_id || completion.result != SACCADE_AGENT_OK || sink.requests != 2 || sink.attempts != 5)
        return result(TestResult::pending_request_failed);

    sink.always_busy = true;
    request.request_id = 12;
    if (!write_frame(client, &request, sizeof(request)))
        return result(TestResult::pending_disconnect_failed);
    const uint32_t attempts_before_pending = sink.attempts;
    for (uint32_t attempt = 0; attempt < 16 && sink.attempts == attempts_before_pending; ++attempt) {
        if (server.advance(now_ns++) != SACCADE_OK)
            return result(TestResult::pending_disconnect_failed);
    }
    if (sink.attempts == attempts_before_pending)
        return result(TestResult::pending_disconnect_failed);
    close(client);
    for (uint32_t attempt = 0; attempt < 16 && sink.disconnects == 0; ++attempt) {
        if (server.advance(now_ns++) != SACCADE_OK)
            return result(TestResult::pending_disconnect_failed);
    }
    if (sink.disconnects != 1 || sink.action_pending)
        return result(TestResult::pending_disconnect_failed);

    (void)unlink(retired_endpoint.data());
    if (rename(endpoint.data(), retired_endpoint.data()) != 0)
        return result(TestResult::replacement_endpoint_removed);
    const int replacement = socket(AF_UNIX, SOCK_STREAM, 0);
    if (replacement < 0 || bind(replacement, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(replacement, 1) != 0)
        return result(TestResult::replacement_endpoint_removed);
    if (server.shutdown() != SACCADE_OK || sink.disconnects != 1)
        return result(TestResult::shutdown_failed);
    if (access(endpoint.data(), F_OK) != 0)
        return result(TestResult::replacement_endpoint_removed);
    close(replacement);
    (void)unlink(endpoint.data());
    (void)unlink(retired_endpoint.data());
    if (contender.initialize({&sink, process_request, disconnect, endpoint.data(), SACCADE_AGENT_CAPABILITY_OBSERVE}, &contender_storage) !=
        SACCADE_OK)
        return result(TestResult::listener_handoff_failed);

    const int handoff_client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (handoff_client < 0 || connect(handoff_client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        contender.advance(now_ns++) != SACCADE_OK)
        return result(TestResult::handoff_connection_failed);
    hello.request_id = 11;
    if (!write_frame(handoff_client, &hello, sizeof(hello)) || !read_response(&contender, handoff_client, &now_ns, &hello_completion) ||
        hello_completion.request_id != hello.request_id || hello_completion.result != SACCADE_AGENT_OK)
        return result(TestResult::handoff_connection_failed);
    close(handoff_client);
    if (contender.shutdown() != SACCADE_OK)
        return result(TestResult::shutdown_failed);
    return result(TestResult::success);
}

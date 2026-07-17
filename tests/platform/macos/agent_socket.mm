#include "platform/macos/agent_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    shutdown_failed
};

struct RequestSink {
    uint32_t requests = 0;
    uint32_t disconnects = 0;
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult process_request(void* context, SaccadeSpanU8 input, SaccadeAgentCapabilityBits capabilities,
                              uint64_t now_ns, SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (input.size != sizeof(SaccadeAgentObserveRequest) || output.size < sizeof(SaccadeAgentObserveCompletion) ||
        capabilities != SACCADE_AGENT_CAPABILITY_OBSERVE || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
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
    ++static_cast<RequestSink*>(context)->requests;
    return SACCADE_OK;
}

SaccadeResult disconnect(void* context) noexcept {
    ++static_cast<RequestSink*>(context)->disconnects;
    return SACCADE_OK;
}

bool write_frame(int client, const void* message, uint32_t size) noexcept {
    return send(client, &size, sizeof(size), 0) == sizeof(size) && send(client, message, size, 0) == size;
}

template <typename T>
bool read_response(saccade::platform::macos::AgentSocket* server, int client, uint64_t* now_ns, T* output) noexcept {
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
        if (server->advance((*now_ns)++) != SACCADE_OK) return false;
        int available = 0;
        if (ioctl(client, FIONREAD, &available) != 0) return false;
        if (available < static_cast<int>(sizeof(uint32_t) + sizeof(T))) {
            usleep(1000);
            continue;
        }
        uint32_t size = 0;
        if (recv(client, &size, sizeof(size), MSG_WAITALL) != sizeof(size) || size != sizeof(T)) return false;
        return recv(client, output, sizeof(T), MSG_WAITALL) == sizeof(T);
    }
    return false;
}

} // namespace

int main() {
    std::array<char, 104> endpoint{};
    if (snprintf(endpoint.data(), endpoint.size(), "/tmp/saccade-agent-test-%u-%d.sock", geteuid(), getpid()) <= 0)
        return result(TestResult::initialization_failed);
    RequestSink sink{};
    saccade::platform::macos::AgentSocket server;
    static saccade::platform::macos::AgentSocketStorage storage;
    if (server.initialize({&sink, process_request, disconnect, endpoint.data(), SACCADE_AGENT_CAPABILITY_OBSERVE},
                          &storage) != SACCADE_OK)
        return result(TestResult::initialization_failed);
    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0) return result(TestResult::connection_failed);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.data(), std::strlen(endpoint.data()) + 1U);
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return result(TestResult::connection_failed);
    uint64_t now_ns = 1;
    if (server.advance(now_ns++) != SACCADE_OK) return result(TestResult::connection_failed);

    SaccadeAgentHelloRequest hello{};
    hello.header.struct_size = static_cast<uint32_t>(sizeof(hello));
    hello.header.api_version = SACCADE_AGENT_API_VERSION;
    hello.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_REQUEST;
    hello.request_id = 7;
    hello.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (!write_frame(client, &hello, sizeof(hello))) return result(TestResult::hello_write_failed);
    SaccadeAgentHelloCompletion hello_completion{};
    if (!read_response(&server, client, &now_ns, &hello_completion) ||
        hello_completion.request_id != hello.request_id || hello_completion.result != SACCADE_AGENT_OK ||
        hello_completion.granted_capability_bits != SACCADE_AGENT_CAPABILITY_OBSERVE)
        return result(TestResult::hello_read_failed);

    SaccadeAgentObserveRequest request{};
    request.header.struct_size = static_cast<uint32_t>(sizeof(request));
    request.header.api_version = SACCADE_AGENT_API_VERSION;
    request.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request.request_id = 9;
    request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (!write_frame(client, &request, sizeof(request))) return result(TestResult::request_write_failed);
    SaccadeAgentObserveCompletion completion{};
    if (!read_response(&server, client, &now_ns, &completion) || completion.request_id != request.request_id ||
        completion.result != SACCADE_AGENT_OK || sink.requests != 1)
        return result(TestResult::request_read_failed);

    close(client);
    if (server.shutdown() != SACCADE_OK || sink.disconnects != 1) return result(TestResult::shutdown_failed);
    return result(TestResult::success);
}

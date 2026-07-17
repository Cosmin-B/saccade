#include "platform/windows/agent_pipe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
    if (request.header.message_kind != SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST) return SACCADE_ERROR_INVALID_ARGUMENT;
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

template <typename T>
bool read_response(saccade::platform::windows::AgentPipe* pipe, HANDLE client, uint64_t* now_ns, T* output) noexcept {
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
        if (pipe->advance((*now_ns)++) != SACCADE_OK) return false;
        DWORD available = 0;
        if (!PeekNamedPipe(client, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            Sleep(1);
            continue;
        }
        DWORD read = 0;
        return ReadFile(client, output, sizeof(*output), &read, nullptr) != 0 && read == sizeof(*output);
    }
    return false;
}

} // namespace

int main() {
    std::array<wchar_t, 128> endpoint{};
    if (swprintf_s(endpoint.data(), endpoint.size(), L"\\\\.\\pipe\\Saccade.Agent.Test.%lu",
                   static_cast<unsigned long>(GetCurrentProcessId())) <= 0)
        return result(TestResult::initialization_failed);
    RequestSink sink{};
    saccade::platform::windows::AgentPipe pipe;
    static saccade::platform::windows::AgentPipeStorage storage;
    if (pipe.initialize({&sink, process_request, disconnect, endpoint.data(), SACCADE_AGENT_CAPABILITY_OBSERVE},
                        &storage) != SACCADE_OK)
        return result(TestResult::initialization_failed);
    HANDLE client = CreateFileW(endpoint.data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) return result(TestResult::connection_failed);
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(client, &mode, nullptr, nullptr)) return result(TestResult::connection_failed);
    uint64_t now_ns = 1;
    if (pipe.advance(now_ns++) != SACCADE_OK) return result(TestResult::connection_failed);

    SaccadeAgentHelloRequest hello{};
    hello.header.struct_size = static_cast<uint32_t>(sizeof(hello));
    hello.header.api_version = SACCADE_AGENT_API_VERSION;
    hello.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_REQUEST;
    hello.request_id = 7;
    hello.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    DWORD written = 0;
    if (!WriteFile(client, &hello, sizeof(hello), &written, nullptr) || written != sizeof(hello))
        return result(TestResult::hello_write_failed);
    SaccadeAgentHelloCompletion hello_completion{};
    if (!read_response(&pipe, client, &now_ns, &hello_completion) || hello_completion.request_id != hello.request_id ||
        hello_completion.result != SACCADE_AGENT_OK ||
        hello_completion.granted_capability_bits != SACCADE_AGENT_CAPABILITY_OBSERVE)
        return result(TestResult::hello_read_failed);

    if (pipe.advance(now_ns++) != SACCADE_OK) return result(TestResult::request_write_failed);
    SaccadeAgentObserveRequest request{};
    request.header.struct_size = static_cast<uint32_t>(sizeof(request));
    request.header.api_version = SACCADE_AGENT_API_VERSION;
    request.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request.request_id = 9;
    request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    if (!WriteFile(client, &request, sizeof(request), &written, nullptr) || written != sizeof(request))
        return result(TestResult::request_write_failed);
    SaccadeAgentObserveCompletion completion{};
    if (!read_response(&pipe, client, &now_ns, &completion) || completion.request_id != request.request_id ||
        completion.result != SACCADE_AGENT_OK || sink.requests != 1)
        return result(TestResult::request_read_failed);

    CloseHandle(client);
    if (pipe.shutdown() != SACCADE_OK || sink.disconnects != 1) return result(TestResult::shutdown_failed);
    return result(TestResult::success);
}

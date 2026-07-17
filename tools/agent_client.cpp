#include "agent_client.hpp"

#include <saccade/saccade.h>

#include <cstring>
#include <ctime>
#include <limits>

#if defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace saccade::tools {

namespace {

uint64_t deadline_after(uint64_t timeout_ns) noexcept {
    const uint64_t now_ns = monotonic_time_ns();
    return timeout_ns > std::numeric_limits<uint64_t>::max() - now_ns ? std::numeric_limits<uint64_t>::max()
                                                                      : now_ns + timeout_ns;
}

} // namespace

bool AgentClient::connect() noexcept {
    close();
#if defined(_WIN32)
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return false;
    std::array<wchar_t, 128> endpoint{};
    if (swprintf_s(endpoint.data(), endpoint.size(), L"\\\\.\\pipe\\Saccade.Agent.v1.%lu",
                   static_cast<unsigned long>(session_id)) <= 0)
        return false;
    if (!WaitNamedPipeW(endpoint.data(), 2000)) return false;
    pipe_ = CreateFileW(endpoint.data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr) != 0) return true;
    close();
    return false;
#elif defined(__APPLE__)
    std::array<char, 104> root{};
    const size_t root_size = confstr(_CS_DARWIN_USER_TEMP_DIR, root.data(), root.size());
    constexpr char leaf[] = "saccade-agent-v1.sock";
    if (root_size == 0 || root_size > root.size()) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const size_t prefix_size = std::strlen(root.data());
    if (prefix_size + sizeof(leaf) + 1U > sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, root.data(), prefix_size);
    size_t leaf_offset = prefix_size;
    if (leaf_offset != 0 && address.sun_path[leaf_offset - 1U] != '/') address.sun_path[leaf_offset++] = '/';
    std::memcpy(address.sun_path + leaf_offset, leaf, sizeof(leaf));
    socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_ >= 0 && ::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
        return true;
    close();
    return false;
#endif
}

bool AgentClient::transact(const void* request, size_t request_size, AgentClientStorage* storage,
                           size_t* response_size) noexcept {
    if (request == nullptr || storage == nullptr || response_size == nullptr || request_size == 0 ||
        request_size > storage->request.size())
        return false;
    SaccadeAgentMessageHeader header{};
    if (request_size < sizeof(header)) return transact_once(request, request_size, storage, response_size);
    std::memcpy(&header, request, sizeof(header));
    if (header.message_kind == SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST &&
        request_size >= sizeof(SaccadeAgentObserveRequest)) {
        SaccadeAgentObserveRequest observe{};
        std::memcpy(&observe, request, sizeof(observe));
        return transact_fresh(request, request_size, observe.freshness, storage, response_size);
    }
    if (header.message_kind == SACCADE_AGENT_MESSAGE_QUERY_REQUEST &&
        request_size >= sizeof(SaccadeAgentQueryRequest)) {
        SaccadeAgentQueryRequest query{};
        std::memcpy(&query, request, sizeof(query));
        return transact_fresh(request, request_size, query.freshness, storage, response_size);
    }
    if (header.message_kind == SACCADE_AGENT_MESSAGE_ACTION_BATCH && request_size >= sizeof(SaccadeAgentActionBatch)) {
        SaccadeAgentActionBatch batch{};
        std::memcpy(&batch, request, sizeof(batch));
        return transact_once(request, request_size, storage, response_size) &&
               ((batch.header.flags & SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION) == 0 ||
                verify_action(batch, storage, response_size));
    }
    return transact_once(request, request_size, storage, response_size);
}

bool AgentClient::transact_once(const void* request, size_t request_size, AgentClientStorage* storage,
                                size_t* response_size) noexcept {
#if defined(_WIN32)
    DWORD written = 0;
    if (!WriteFile(pipe_, request, static_cast<DWORD>(request_size), &written, nullptr) ||
        written != static_cast<DWORD>(request_size))
        return false;
    DWORD read = 0;
    if (!ReadFile(pipe_, storage->response.data(), static_cast<DWORD>(storage->response.size()), &read, nullptr))
        return false;
    *response_size = read;
    return true;
#elif defined(__APPLE__)
    const uint32_t frame_size = static_cast<uint32_t>(request_size);
    if (!send_all(reinterpret_cast<const uint8_t*>(&frame_size), sizeof(frame_size)) ||
        !send_all(static_cast<const uint8_t*>(request), request_size))
        return false;
    uint32_t reply_size = 0;
    if (!receive_all(reinterpret_cast<uint8_t*>(&reply_size), sizeof(reply_size)) || reply_size == 0 ||
        reply_size > storage->response.size() || !receive_all(storage->response.data(), reply_size))
        return false;
    *response_size = reply_size;
    return true;
#endif
}

bool AgentClient::transact_fresh(const void* request, size_t request_size, const SaccadeAgentFreshness& freshness,
                                 AgentClientStorage* storage, size_t* response_size) noexcept {
    const bool waits = freshness.policy == SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    const uint64_t deadline_ns = waits ? deadline_after(freshness.timeout_ns) : 0;
    for (;;) {
        if (!transact_once(request, request_size, storage, response_size)) return false;
        if (!waits) return true;

        SaccadeAgentMessageHeader header{};
        if (*response_size < sizeof(header)) return true;
        std::memcpy(&header, storage->response.data(), sizeof(header));
        SaccadeAgentResult result = SACCADE_AGENT_ERROR_INVALID_MESSAGE;
        if (header.message_kind == SACCADE_AGENT_MESSAGE_OBSERVE_COMPLETION &&
            *response_size >= sizeof(SaccadeAgentObserveCompletion)) {
            SaccadeAgentObserveCompletion completion{};
            std::memcpy(&completion, storage->response.data(), sizeof(completion));
            result = completion.result;
        } else if (header.message_kind == SACCADE_AGENT_MESSAGE_QUERY_COMPLETION &&
                   *response_size >= sizeof(SaccadeAgentQueryCompletion)) {
            SaccadeAgentQueryCompletion completion{};
            std::memcpy(&completion, storage->response.data(), sizeof(completion));
            result = completion.result;
        }
        if (result != SACCADE_AGENT_ERROR_TIMEOUT || monotonic_time_ns() >= deadline_ns) return true;
    }
}

bool AgentClient::verify_action(const SaccadeAgentActionBatch& batch, AgentClientStorage* storage,
                                size_t* response_size) noexcept {
    if (*response_size < sizeof(SaccadeAgentActionCompletion)) return true;
    SaccadeAgentActionCompletion action_completion{};
    std::memcpy(&action_completion, storage->response.data(), sizeof(action_completion));
    if ((action_completion.header.flags & SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE) != 0 ||
        (action_completion.result != SACCADE_AGENT_OK && action_completion.result != SACCADE_AGENT_ERROR_TIMEOUT) ||
        action_completion.completed_action_count != batch.action_count ||
        action_completion.failed_action_index != UINT32_MAX)
        return true;

    const size_t action_response_size = *response_size;
    std::memcpy(storage->request.data(), storage->response.data(), action_response_size);
    SaccadeAgentObserveRequest observe{};
    observe.header = {sizeof(observe), SACCADE_AGENT_API_VERSION, SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST, 0};
    observe.request_id = batch.request_id;
    observe.scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    observe.scope.source_mode = SACCADE_AGENT_SOURCE_FUSED;
    observe.freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    observe.freshness.after_generation = action_completion.validated_generation;
    observe.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    observe.maximum_targets = 1;
    observe.target_stride = sizeof(SaccadeAgentTarget);
    observe.total_capacity = SACCADE_AGENT_MAX_MESSAGE_BYTES;

    SaccadeAgentResult verification_result = SACCADE_AGENT_ERROR_TIMEOUT;
    int32_t platform_error = SACCADE_ERROR_TIMEOUT;
    SaccadeAgentGeneration next_generation{};
    bool transport_ok = true;
    while (monotonic_time_ns() < batch.deadline_ns) {
        const uint64_t now_ns = monotonic_time_ns();
        observe.freshness.timeout_ns = batch.deadline_ns - now_ns;
        size_t verification_size = 0;
        if (!transact_once(&observe, sizeof(observe), storage, &verification_size)) {
            transport_ok = false;
            break;
        }
        if (verification_size < sizeof(SaccadeAgentObserveCompletion)) {
            transport_ok = false;
            break;
        }
        SaccadeAgentObserveCompletion completion{};
        std::memcpy(&completion, storage->response.data(), sizeof(completion));
        verification_result = completion.result;
        platform_error = completion.platform_error;
        if (completion.result == SACCADE_AGENT_OK) {
            next_generation = completion.generation;
            break;
        }
        if (completion.result != SACCADE_AGENT_ERROR_TIMEOUT) break;
    }

    std::memcpy(storage->response.data(), storage->request.data(), action_response_size);
    *response_size = action_response_size;
    if (!transport_ok) return false;
    auto* completion = reinterpret_cast<SaccadeAgentActionCompletion*>(storage->response.data());
    completion->result = verification_result;
    completion->platform_error = platform_error;
    if (verification_result == SACCADE_AGENT_OK) {
        completion->header.flags |= SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE;
        completion->next_generation = next_generation;
    }
    return true;
}

bool AgentClient::hello(SaccadeAgentCapabilityBits requested, AgentClientStorage* storage,
                        SaccadeAgentCapabilityBits* granted) noexcept {
    SaccadeAgentHelloRequest hello{};
    hello.header.struct_size = static_cast<uint32_t>(sizeof(hello));
    hello.header.api_version = SACCADE_AGENT_API_VERSION;
    hello.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_REQUEST;
    hello.request_id = 1;
    hello.requested_capability_bits = requested;
    size_t response_size = 0;
    if (!transact(&hello, sizeof(hello), storage, &response_size) ||
        response_size != sizeof(SaccadeAgentHelloCompletion))
        return false;
    SaccadeAgentHelloCompletion completion{};
    std::memcpy(&completion, storage->response.data(), sizeof(completion));
    if (completion.header.api_version != SACCADE_AGENT_API_VERSION || completion.result != SACCADE_AGENT_OK)
        return false;
    *granted = completion.granted_capability_bits;
    return true;
}

void AgentClient::close() noexcept {
#if defined(_WIN32)
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
#elif defined(__APPLE__)
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
#endif
}

AgentClient::~AgentClient() {
    close();
}

#if defined(__APPLE__)
bool AgentClient::send_all(const uint8_t* bytes, size_t size) noexcept {
    size_t offset = 0;
    while (offset != size) {
        const ssize_t written = send(socket_, bytes + offset, size - offset, 0);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool AgentClient::receive_all(uint8_t* bytes, size_t size) noexcept {
    size_t offset = 0;
    while (offset != size) {
        const ssize_t read = recv(socket_, bytes + offset, size - offset, 0);
        if (read <= 0) return false;
        offset += static_cast<size_t>(read);
    }
    return true;
}
#endif

uint64_t monotonic_time_ns() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 1;
    const uint64_t whole = static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart);
    const uint64_t remainder = static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart);
    return whole * UINT64_C(1'000'000'000) +
           remainder * UINT64_C(1'000'000'000) / static_cast<uint64_t>(frequency.QuadPart);
#elif defined(__APPLE__)
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) return 1;
    return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1'000'000'000) + static_cast<uint64_t>(value.tv_nsec);
#endif
}

} // namespace saccade::tools

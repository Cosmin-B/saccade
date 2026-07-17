#include "platform/windows/agent_pipe.hpp"

#include <bcrypt.h>

#include <cstring>
#include <cwchar>

namespace saccade::platform::windows {
namespace {

bool disconnected_error(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED;
}

} // namespace

AgentPipe::~AgentPipe() {
    if (initialized_) (void)shutdown();
}

SaccadeResult AgentPipe::initialize(AgentPipeConfig config, AgentPipeStorage* storage) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.context == nullptr || config.request == nullptr || config.disconnect == nullptr ||
        config.allowed_capability_bits == 0 || storage == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return SACCADE_ERROR_PERMISSION;
    DWORD token_size = 0;
    const BOOL token_read =
        GetTokenInformation(token, TokenUser, token_user_.data(), static_cast<DWORD>(token_user_.size()), &token_size);
    CloseHandle(token);
    if (!token_read) return token_size > token_user_.size() ? SACCADE_ERROR_CAPACITY : SACCADE_ERROR_PERMISSION;
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_user_.data());
    if (!IsValidSid(token_user->User.Sid)) return SACCADE_ERROR_PERMISSION;
    if (!InitializeAcl(reinterpret_cast<PACL>(acl_.data()), static_cast<DWORD>(acl_.size()), ACL_REVISION) ||
        !AddAccessAllowedAce(reinterpret_cast<PACL>(acl_.data()), ACL_REVISION, GENERIC_READ | GENERIC_WRITE,
                             token_user->User.Sid) ||
        !InitializeSecurityDescriptor(&security_descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&security_descriptor_, TRUE, reinterpret_cast<PACL>(acl_.data()), FALSE))
        return SACCADE_ERROR_BACKEND;

    security_attributes_.nLength = sizeof(security_attributes_);
    security_attributes_.lpSecurityDescriptor = &security_descriptor_;
    security_attributes_.bInheritHandle = FALSE;
    if (config.endpoint != nullptr) {
        const size_t size = std::wcslen(config.endpoint);
        if (size == 0 || size >= endpoint_.size()) return SACCADE_ERROR_CAPACITY;
        std::wmemcpy(endpoint_.data(), config.endpoint, size + 1U);
    } else {
        DWORD session_id = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return SACCADE_ERROR_BACKEND;
        const int written = swprintf_s(endpoint_.data(), endpoint_.size(), L"\\\\.\\pipe\\Saccade.Agent.v1.%lu",
                                       static_cast<unsigned long>(session_id));
        if (written <= 0 || static_cast<size_t>(written) >= endpoint_.size()) return SACCADE_ERROR_CAPACITY;
    }

    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event_ == nullptr) return SACCADE_ERROR_BACKEND;
    config_ = config;
    storage_ = storage;
    SaccadeResult result = create_pipe();
    if (result == SACCADE_OK) {
        initialized_ = true;
        result = begin_connect();
    }
    if (result != SACCADE_OK) {
        close_pipe();
        CloseHandle(event_);
        event_ = nullptr;
        config_ = {};
        storage_ = nullptr;
        initialized_ = false;
    }
    return result;
}

SaccadeResult AgentPipe::create_pipe() noexcept {
    pipe_ = CreateNamedPipeW(endpoint_.data(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                             64U * 1024U, 64U * 1024U, 0, &security_attributes_);
    return pipe_ == INVALID_HANDLE_VALUE ? SACCADE_ERROR_BACKEND : SACCADE_OK;
}

SaccadeResult AgentPipe::begin_connect() noexcept {
    operation_ = {};
    operation_.hEvent = event_;
    ResetEvent(event_);
    state_ = State::connecting;
    if (ConnectNamedPipe(pipe_, &operation_)) return SACCADE_OK;
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) return SACCADE_OK;
    if (error == ERROR_PIPE_CONNECTED) return complete_connect();
    return SACCADE_ERROR_BACKEND;
}

SaccadeResult AgentPipe::begin_read() noexcept {
    operation_ = {};
    operation_.hEvent = event_;
    ResetEvent(event_);
    state_ = State::reading;
    DWORD ignored = 0;
    if (ReadFile(pipe_, storage_->request.data(), static_cast<DWORD>(storage_->request.size()), &ignored, &operation_))
        return SACCADE_OK;
    const DWORD error = GetLastError();
    return error == ERROR_IO_PENDING ? SACCADE_OK : disconnected_error(error) ? reconnect() : SACCADE_ERROR_BACKEND;
}

SaccadeResult AgentPipe::begin_write(size_t byte_size) noexcept {
    if (byte_size == 0 || byte_size > storage_->response.size()) return SACCADE_ERROR_CAPACITY;
    operation_ = {};
    operation_.hEvent = event_;
    ResetEvent(event_);
    state_ = State::writing;
    write_size_ = byte_size;
    DWORD ignored = 0;
    if (WriteFile(pipe_, storage_->response.data(), static_cast<DWORD>(byte_size), &ignored, &operation_))
        return SACCADE_OK;
    const DWORD error = GetLastError();
    return error == ERROR_IO_PENDING ? SACCADE_OK : disconnected_error(error) ? reconnect() : SACCADE_ERROR_BACKEND;
}

SaccadeResult AgentPipe::complete_connect() noexcept {
    authenticated_ = false;
    client_capability_bits_ = 0;
    ++stats_.connections;
    return begin_read();
}

SaccadeResult AgentPipe::hello(uint32_t byte_size, size_t* output_size) noexcept {
    SaccadeAgentHelloRequest request{};
    if (byte_size == sizeof(request)) std::memcpy(&request, storage_->request.data(), sizeof(request));
    SaccadeAgentHelloCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_COMPLETION;
    completion.request_id = request.request_id;
    const bool valid = byte_size == sizeof(request) && request.header.struct_size == sizeof(request) &&
                       request.header.api_version == SACCADE_AGENT_API_VERSION &&
                       request.header.message_kind == SACCADE_AGENT_MESSAGE_HELLO_REQUEST &&
                       request.requested_capability_bits != 0;
    if (!valid) {
        completion.result = SACCADE_AGENT_ERROR_INVALID_MESSAGE;
        completion.platform_error = SACCADE_ERROR_INVALID_ARGUMENT;
        ++stats_.rejected_messages;
    } else {
        client_capability_bits_ = request.requested_capability_bits & config_.allowed_capability_bits;
        if (client_capability_bits_ == 0) {
            completion.result = SACCADE_AGENT_ERROR_CAPABILITY_DENIED;
            completion.platform_error = SACCADE_ERROR_PERMISSION;
            ++stats_.rejected_messages;
        } else {
            completion.granted_capability_bits = client_capability_bits_;
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(completion.service_nonce),
                                static_cast<ULONG>(sizeof(completion.service_nonce)),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                return SACCADE_ERROR_BACKEND;
            authenticated_ = true;
        }
    }
    std::memcpy(storage_->response.data(), &completion, sizeof(completion));
    *output_size = sizeof(completion);
    return SACCADE_OK;
}

SaccadeResult AgentPipe::complete_read(uint32_t byte_size, uint64_t now_ns) noexcept {
    if (byte_size == 0) return reconnect();
    ++stats_.requests;
    stats_.bytes_read += byte_size;
    size_t output_size = 0;
    SaccadeResult result =
        authenticated_
            ? config_.request(config_.context, {storage_->request.data(), byte_size}, client_capability_bits_, now_ns,
                              {storage_->response.data(), storage_->response.size()}, &output_size)
            : hello(byte_size, &output_size);
    if (result != SACCADE_OK) {
        ++stats_.rejected_messages;
        return reconnect();
    }
    return begin_write(output_size);
}

SaccadeResult AgentPipe::complete_write(uint32_t byte_size) noexcept {
    if (byte_size != write_size_) return reconnect();
    ++stats_.responses;
    stats_.bytes_written += byte_size;
    write_size_ = 0;
    return begin_read();
}

SaccadeResult AgentPipe::reconnect() noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) return SACCADE_ERROR_STATE;
    const SaccadeResult neutralized = authenticated_ ? config_.disconnect(config_.context) : SACCADE_OK;
    (void)CancelIoEx(pipe_, &operation_);
    (void)DisconnectNamedPipe(pipe_);
    authenticated_ = false;
    client_capability_bits_ = 0;
    write_size_ = 0;
    ++stats_.disconnects;
    const SaccadeResult connected = begin_connect();
    return neutralized != SACCADE_OK ? neutralized : connected;
}

SaccadeResult AgentPipe::poll(uint64_t now_ns) noexcept {
    DWORD byte_size = 0;
    if (!GetOverlappedResult(pipe_, &operation_, &byte_size, FALSE)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return SACCADE_OK;
        if (error == ERROR_MORE_DATA) {
            ++stats_.rejected_messages;
            return reconnect();
        }
        return disconnected_error(error) || error == ERROR_OPERATION_ABORTED ? reconnect() : SACCADE_ERROR_BACKEND;
    }
    switch (state_) {
    case State::connecting:
        return complete_connect();
    case State::reading:
        return complete_read(byte_size, now_ns);
    case State::writing:
        return complete_write(byte_size);
    case State::stopped:
        return SACCADE_ERROR_STATE;
    }
    return SACCADE_ERROR_STATE;
}

SaccadeResult AgentPipe::advance(uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0) return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult result = poll(now_ns);
    if (result != SACCADE_OK) ++stats_.failures;
    return result;
}

void AgentPipe::close_pipe() noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    (void)CancelIoEx(pipe_, nullptr);
    (void)DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
}

SaccadeResult AgentPipe::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SaccadeResult neutralized = authenticated_ ? config_.disconnect(config_.context) : SACCADE_OK;
    close_pipe();
    CloseHandle(event_);
    event_ = nullptr;
    config_ = {};
    storage_ = nullptr;
    state_ = State::stopped;
    client_capability_bits_ = 0;
    authenticated_ = false;
    initialized_ = false;
    return neutralized;
}

} // namespace saccade::platform::windows

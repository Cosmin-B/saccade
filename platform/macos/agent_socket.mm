#include "platform/macos/agent_socket.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace saccade::platform::macos {
namespace {

bool would_block() noexcept {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool set_nonblocking(int socket) noexcept {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool embedded_helper(CFDictionaryRef own_info, CFDictionaryRef peer_info) noexcept {
    const auto own_url = static_cast<CFURLRef>(CFDictionaryGetValue(own_info, kSecCodeInfoMainExecutable));
    const auto peer_url = static_cast<CFURLRef>(CFDictionaryGetValue(peer_info, kSecCodeInfoMainExecutable));
    if (own_url == nullptr || peer_url == nullptr || CFGetTypeID(own_url) != CFURLGetTypeID() ||
        CFGetTypeID(peer_url) != CFURLGetTypeID()) {
        return false;
    }

    std::array<char, PATH_MAX> own_path{};
    std::array<char, PATH_MAX> peer_path{};
    if (!CFURLGetFileSystemRepresentation(own_url, true, reinterpret_cast<UInt8*>(own_path.data()), own_path.size()) ||
        !CFURLGetFileSystemRepresentation(peer_url, true, reinterpret_cast<UInt8*>(peer_path.data()), peer_path.size())) {
        return false;
    }
    if (std::strcmp(own_path.data(), peer_path.data()) == 0)
        return true;

    constexpr char contents_marker[] = ".app/Contents/";
    const char* contents = std::strstr(own_path.data(), contents_marker);
    if (contents == nullptr)
        return false;
    const size_t root_size = static_cast<size_t>(contents - own_path.data()) + sizeof(contents_marker) - 1U;
    if (std::strncmp(own_path.data(), peer_path.data(), root_size) != 0)
        return false;

    const char* relative = peer_path.data() + root_size;
    return std::strcmp(relative, "Helpers/saccade") == 0 || std::strcmp(relative, "Helpers/saccade-mcp") == 0;
}

bool same_signing_team(pid_t pid) noexcept {
    SecCodeRef own_code = nullptr;
    SecCodeRef peer_code = nullptr;
    CFDictionaryRef own_info = nullptr;
    CFDictionaryRef peer_info = nullptr;
    NSDictionary* attributes = @{(__bridge NSString*)kSecGuestAttributePid : @(pid)};
    bool allowed =
        SecCodeCopySelf(kSecCSDefaultFlags, &own_code) == errSecSuccess &&
        SecCodeCopyGuestWithAttributes(nullptr, (__bridge CFDictionaryRef)attributes, kSecCSDefaultFlags, &peer_code) == errSecSuccess &&
        SecCodeCheckValidity(peer_code, kSecCSStrictValidate, nullptr) == errSecSuccess &&
        SecCodeCopySigningInformation(own_code, kSecCSSigningInformation, &own_info) == errSecSuccess &&
        SecCodeCopySigningInformation(peer_code, kSecCSSigningInformation, &peer_info) == errSecSuccess;
    if (allowed) {
        const auto own_team = static_cast<CFStringRef>(CFDictionaryGetValue(own_info, kSecCodeInfoTeamIdentifier));
        const auto peer_team = static_cast<CFStringRef>(CFDictionaryGetValue(peer_info, kSecCodeInfoTeamIdentifier));
        allowed = own_team != nullptr && peer_team != nullptr && CFEqual(own_team, peer_team);
        if (!allowed && own_team == nullptr && peer_team == nullptr)
            allowed = embedded_helper(own_info, peer_info);
    }
    if (peer_info != nullptr)
        CFRelease(peer_info);
    if (own_info != nullptr)
        CFRelease(own_info);
    if (peer_code != nullptr)
        CFRelease(peer_code);
    if (own_code != nullptr)
        CFRelease(own_code);
    return allowed;
}

} // namespace

AgentSocket::~AgentSocket() {
    if (initialized_)
        (void)shutdown();
}

SaccadeResult AgentSocket::initialize(AgentSocketConfig config, AgentSocketStorage* storage) noexcept {
    if (initialized_)
        return SACCADE_ERROR_ALREADY_EXISTS;
    if (config.context == nullptr || config.request == nullptr || config.disconnect == nullptr || config.allowed_capability_bits == 0 ||
        storage == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (config.endpoint != nullptr) {
        const size_t size = std::strlen(config.endpoint);
        if (size == 0 || size >= endpoint_.size())
            return SACCADE_ERROR_CAPACITY;
        std::memcpy(endpoint_.data(), config.endpoint, size + 1U);
    } else {
        std::array<char, endpoint_capacity_> root{};
        const size_t root_size = confstr(_CS_DARWIN_USER_TEMP_DIR, root.data(), root.size());
        constexpr char leaf[] = "saccade-agent-v1.sock";
        if (root_size == 0 || root_size > root.size() || root_size + sizeof(leaf) > endpoint_.size())
            return SACCADE_ERROR_CAPACITY;
        const size_t prefix_size = std::strlen(root.data());
        std::memcpy(endpoint_.data(), root.data(), prefix_size);
        if (prefix_size != 0 && endpoint_[prefix_size - 1U] != '/')
            endpoint_[prefix_size] = '/';
        const size_t leaf_offset = prefix_size + (prefix_size != 0 && endpoint_[prefix_size - 1U] != '/' ? 1U : 0U);
        std::memcpy(endpoint_.data() + leaf_offset, leaf, sizeof(leaf));
    }

    listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0)
        return SACCADE_ERROR_BACKEND;
    int no_sigpipe = 1;
    if (setsockopt(listener_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0 || !set_nonblocking(listener_)) {
        close(listener_);
        listener_ = -1;
        return SACCADE_ERROR_BACKEND;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint_.data(), std::strlen(endpoint_.data()) + 1U);
    if (access(endpoint_.data(), F_OK) == 0) {
        const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        const bool active = probe >= 0 && connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        if (probe >= 0)
            close(probe);
        if (active) {
            close(listener_);
            listener_ = -1;
            return SACCADE_ERROR_ALREADY_EXISTS;
        }
        (void)unlink(endpoint_.data());
    }
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(listener_);
        listener_ = -1;
        return SACCADE_ERROR_BACKEND;
    }

    struct stat endpoint_status{};
    if (lstat(endpoint_.data(), &endpoint_status) != 0 || !S_ISSOCK(endpoint_status.st_mode)) {
        close(listener_);
        listener_ = -1;
        return SACCADE_ERROR_BACKEND;
    }
    endpoint_device_ = static_cast<uint64_t>(endpoint_status.st_dev);
    endpoint_inode_ = static_cast<uint64_t>(endpoint_status.st_ino);
    if (chmod(endpoint_.data(), S_IRUSR | S_IWUSR) != 0 || listen(listener_, 1) != 0) {
        close(listener_);
        listener_ = -1;
        unlink_owned_endpoint();
        return SACCADE_ERROR_BACKEND;
    }
    config_ = config;
    storage_ = storage;
    state_ = State::accepting;
    initialized_ = true;
    return SACCADE_OK;
}

bool AgentSocket::peer_allowed(int client) noexcept {
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(client, &uid, &gid) != 0 || uid != geteuid())
        return false;
    pid_t pid = 0;
    socklen_t pid_size = sizeof(pid);
    return getsockopt(client, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size) == 0 && pid > 0 && same_signing_team(pid);
}

SaccadeResult AgentSocket::accept_client() noexcept {
    const int client = accept(listener_, nullptr, nullptr);
    if (client < 0)
        return would_block() ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    int no_sigpipe = 1;
    if (!peer_allowed(client) || setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0 ||
        !set_nonblocking(client)) {
        ++stats_.rejected_peers;
        close(client);
        return SACCADE_OK;
    }
    client_ = client;
    authenticated_ = false;
    client_capability_bits_ = 0;
    request_size_ = 0;
    frame_offset_ = 0;
    message_offset_ = 0;
    state_ = State::reading_size;
    ++stats_.connections;
    return SACCADE_OK;
}

SaccadeResult AgentSocket::read_size() noexcept {
    auto* bytes = reinterpret_cast<uint8_t*>(&request_size_);
    const ssize_t read = recv(client_, bytes + frame_offset_, sizeof(request_size_) - frame_offset_, 0);
    if (read == 0)
        return disconnect();
    if (read < 0)
        return would_block() ? SACCADE_OK : disconnect();
    frame_offset_ += static_cast<size_t>(read);
    if (frame_offset_ != sizeof(request_size_))
        return SACCADE_OK;
    if (request_size_ < sizeof(SaccadeAgentMessageHeader) || request_size_ > storage_->request.size()) {
        ++stats_.rejected_messages;
        return disconnect();
    }
    frame_offset_ = 0;
    message_offset_ = 0;
    state_ = State::reading_message;
    return SACCADE_OK;
}

SaccadeResult AgentSocket::read_message(uint64_t now_ns) noexcept {
    const ssize_t read = recv(client_, storage_->request.data() + message_offset_, request_size_ - message_offset_, 0);
    if (read == 0)
        return disconnect();
    if (read < 0)
        return would_block() ? SACCADE_OK : disconnect();
    message_offset_ += static_cast<size_t>(read);
    if (message_offset_ != request_size_)
        return SACCADE_OK;
    stats_.bytes_read += request_size_ + sizeof(request_size_);
    ++stats_.requests;
    state_ = State::processing;
    return process(now_ns);
}

SaccadeResult AgentSocket::hello(size_t* output_size) noexcept {
    SaccadeAgentHelloRequest request{};
    if (request_size_ == sizeof(request))
        std::memcpy(&request, storage_->request.data(), sizeof(request));
    SaccadeAgentHelloCompletion completion{};
    completion.header.struct_size = static_cast<uint32_t>(sizeof(completion));
    completion.header.api_version = SACCADE_AGENT_API_VERSION;
    completion.header.message_kind = SACCADE_AGENT_MESSAGE_HELLO_COMPLETION;
    completion.request_id = request.request_id;
    const bool valid = request_size_ == sizeof(request) && request.header.struct_size == sizeof(request) &&
                       request.header.api_version == SACCADE_AGENT_API_VERSION &&
                       request.header.message_kind == SACCADE_AGENT_MESSAGE_HELLO_REQUEST && request.requested_capability_bits != 0;
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
            if (SecRandomCopyBytes(kSecRandomDefault, sizeof(completion.service_nonce),
                                   reinterpret_cast<uint8_t*>(completion.service_nonce)) != errSecSuccess)
                return SACCADE_ERROR_BACKEND;
            authenticated_ = true;
        }
    }
    std::memcpy(storage_->response.data(), &completion, sizeof(completion));
    *output_size = sizeof(completion);
    return SACCADE_OK;
}

SaccadeResult AgentSocket::process(uint64_t now_ns) noexcept {
    size_t output_size = 0;
    const SaccadeResult result = authenticated_
                                     ? config_.request(config_.context, {storage_->request.data(), request_size_}, client_capability_bits_,
                                                       now_ns, {storage_->response.data(), storage_->response.size()}, &output_size)
                                     : hello(&output_size);
    if (result == SACCADE_ERROR_BUSY && authenticated_ && output_size == 0) {
        state_ = State::processing;
        return SACCADE_OK;
    }
    if (result != SACCADE_OK || output_size == 0 || output_size > storage_->response.size()) {
        ++stats_.rejected_messages;
        return disconnect();
    }
    response_size_ = static_cast<uint32_t>(output_size);
    frame_offset_ = 0;
    message_offset_ = 0;
    state_ = State::writing_size;
    return SACCADE_OK;
}

SaccadeResult AgentSocket::process_pending(uint64_t now_ns) noexcept {
    uint8_t ignored = 0;
    const ssize_t available = recv(client_, &ignored, sizeof(ignored), MSG_PEEK | MSG_DONTWAIT);
    if (available == 0)
        return disconnect();
    if (available < 0 && !would_block())
        return disconnect();
    return process(now_ns);
}

SaccadeResult AgentSocket::write_size() noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&response_size_);
    const ssize_t written = send(client_, bytes + frame_offset_, sizeof(response_size_) - frame_offset_, 0);
    if (written < 0)
        return would_block() ? SACCADE_OK : disconnect();
    frame_offset_ += static_cast<size_t>(written);
    if (frame_offset_ != sizeof(response_size_))
        return SACCADE_OK;
    state_ = State::writing_message;
    return SACCADE_OK;
}

SaccadeResult AgentSocket::write_message() noexcept {
    const ssize_t written = send(client_, storage_->response.data() + message_offset_, response_size_ - message_offset_, 0);
    if (written < 0)
        return would_block() ? SACCADE_OK : disconnect();
    message_offset_ += static_cast<size_t>(written);
    if (message_offset_ != response_size_)
        return SACCADE_OK;
    stats_.bytes_written += response_size_ + sizeof(response_size_);
    ++stats_.responses;
    request_size_ = 0;
    response_size_ = 0;
    frame_offset_ = 0;
    message_offset_ = 0;
    state_ = State::reading_size;
    return SACCADE_OK;
}

SaccadeResult AgentSocket::disconnect() noexcept {
    const SaccadeResult neutralized = authenticated_ ? config_.disconnect(config_.context) : SACCADE_OK;
    if (client_ >= 0) {
        close(client_);
        client_ = -1;
        ++stats_.disconnects;
    }
    authenticated_ = false;
    client_capability_bits_ = 0;
    request_size_ = 0;
    response_size_ = 0;
    frame_offset_ = 0;
    message_offset_ = 0;
    state_ = State::accepting;
    return neutralized;
}

void AgentSocket::unlink_owned_endpoint() noexcept {
    if (endpoint_inode_ == 0)
        return;

    struct stat endpoint_status{};
    if (lstat(endpoint_.data(), &endpoint_status) == 0 && S_ISSOCK(endpoint_status.st_mode) &&
        static_cast<uint64_t>(endpoint_status.st_dev) == endpoint_device_ &&
        static_cast<uint64_t>(endpoint_status.st_ino) == endpoint_inode_) {
        (void)unlink(endpoint_.data());
    }
    endpoint_device_ = 0;
    endpoint_inode_ = 0;
}

SaccadeResult AgentSocket::advance(uint64_t now_ns) noexcept {
    if (!initialized_ || now_ns == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result = SACCADE_OK;
    switch (state_) {
    case State::accepting:
        result = accept_client();
        break;
    case State::reading_size:
        result = read_size();
        break;
    case State::reading_message:
        result = read_message(now_ns);
        break;
    case State::processing:
        result = process_pending(now_ns);
        break;
    case State::writing_size:
        result = write_size();
        break;
    case State::writing_message:
        result = write_message();
        break;
    }
    if (result != SACCADE_OK)
        ++stats_.failures;
    return result;
}

SaccadeResult AgentSocket::shutdown() noexcept {
    if (!initialized_)
        return SACCADE_ERROR_STATE;
    const SaccadeResult neutralized = disconnect();
    close(listener_);
    listener_ = -1;
    unlink_owned_endpoint();
    config_ = {};
    storage_ = nullptr;
    initialized_ = false;
    return neutralized;
}

} // namespace saccade::platform::macos

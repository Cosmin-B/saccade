#ifndef SACCADE_PLATFORM_MACOS_AGENT_SOCKET_HPP
#define SACCADE_PLATFORM_MACOS_AGENT_SOCKET_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_agent.h>
#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::macos {

using AgentRequestFn = SaccadeResult (*)(void*, SaccadeSpanU8, SaccadeAgentCapabilityBits, uint64_t,
                                         SaccadeMutableSpanU8, size_t*) noexcept;
using AgentDisconnectFn = SaccadeResult (*)(void*) noexcept;

struct AgentSocketConfig {
    void* context = nullptr;
    AgentRequestFn request = nullptr;
    AgentDisconnectFn disconnect = nullptr;
    const char* endpoint = nullptr;
    SaccadeAgentCapabilityBits allowed_capability_bits = 0;
};

struct AgentSocketStats {
    uint64_t connections = 0;
    uint64_t disconnects = 0;
    uint64_t requests = 0;
    uint64_t responses = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t rejected_peers = 0;
    uint64_t rejected_messages = 0;
    uint64_t failures = 0;
};

struct AgentSocketStorage {
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> request{};
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> response{};
};

class AgentSocket final {
  public:
    AgentSocket() noexcept = default;
    ~AgentSocket();

    AgentSocket(const AgentSocket&) = delete;
    AgentSocket& operator=(const AgentSocket&) = delete;
    AgentSocket(AgentSocket&&) = delete;
    AgentSocket& operator=(AgentSocket&&) = delete;

    SaccadeResult initialize(AgentSocketConfig, AgentSocketStorage*) noexcept;
    SaccadeResult advance(uint64_t now_ns) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] const char* endpoint() const noexcept { return endpoint_.data(); }

    [[nodiscard]] AgentSocketStats stats() const noexcept { return stats_; }

  private:
    enum class State : uint8_t { accepting, reading_size, reading_message, writing_size, writing_message };

    SaccadeResult accept_client() noexcept;
    SaccadeResult read_size() noexcept;
    SaccadeResult read_message(uint64_t now_ns) noexcept;
    SaccadeResult write_size() noexcept;
    SaccadeResult write_message() noexcept;
    SaccadeResult process(uint64_t now_ns) noexcept;
    SaccadeResult hello(size_t* output_size) noexcept;
    SaccadeResult disconnect() noexcept;
    void unlink_owned_endpoint() noexcept;
    bool peer_allowed(int client) noexcept;

    static constexpr size_t endpoint_capacity_ = 104;
    std::array<char, endpoint_capacity_> endpoint_{};
    AgentSocketConfig config_{};
    AgentSocketStorage* storage_ = nullptr;
    AgentSocketStats stats_{};
    uint32_t request_size_ = 0;
    uint32_t response_size_ = 0;
    size_t frame_offset_ = 0;
    size_t message_offset_ = 0;
    uint64_t endpoint_device_ = 0;
    uint64_t endpoint_inode_ = 0;
    SaccadeAgentCapabilityBits client_capability_bits_ = 0;
    int listener_ = -1;
    int client_ = -1;
    State state_ = State::accepting;
    bool authenticated_ = false;
    bool initialized_ = false;
};

static_assert(sizeof(AgentSocketStats) == 72);
static_assert(sizeof(AgentSocketStorage) == 2U * SACCADE_AGENT_MAX_MESSAGE_BYTES);
static_assert(sizeof(AgentSocket) < 1024);

} // namespace saccade::platform::macos

#endif

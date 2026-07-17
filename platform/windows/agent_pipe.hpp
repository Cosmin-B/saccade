#ifndef SACCADE_PLATFORM_WINDOWS_AGENT_PIPE_HPP
#define SACCADE_PLATFORM_WINDOWS_AGENT_PIPE_HPP

#include <saccade/saccade.h>
#include <saccade/saccade_agent.h>
#include <saccade/saccade_backend.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::platform::windows {

using AgentRequestFn = SaccadeResult (*)(void*, SaccadeSpanU8, SaccadeAgentCapabilityBits, uint64_t,
                                         SaccadeMutableSpanU8, size_t*) noexcept;
using AgentDisconnectFn = SaccadeResult (*)(void*) noexcept;

struct AgentPipeConfig {
    void* context = nullptr;
    AgentRequestFn request = nullptr;
    AgentDisconnectFn disconnect = nullptr;
    const wchar_t* endpoint = nullptr;
    SaccadeAgentCapabilityBits allowed_capability_bits = 0;
};

struct AgentPipeStats {
    uint64_t connections = 0;
    uint64_t disconnects = 0;
    uint64_t requests = 0;
    uint64_t responses = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t rejected_messages = 0;
    uint64_t failures = 0;
};

struct AgentPipeStorage {
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> request{};
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> response{};
};

class AgentPipe final {
  public:
    AgentPipe() noexcept = default;
    ~AgentPipe();

    AgentPipe(const AgentPipe&) = delete;
    AgentPipe& operator=(const AgentPipe&) = delete;
    AgentPipe(AgentPipe&&) = delete;
    AgentPipe& operator=(AgentPipe&&) = delete;

    SaccadeResult initialize(AgentPipeConfig, AgentPipeStorage*) noexcept;
    SaccadeResult advance(uint64_t now_ns) noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] const wchar_t* endpoint() const noexcept { return endpoint_.data(); }

    [[nodiscard]] AgentPipeStats stats() const noexcept { return stats_; }

  private:
    enum class State : uint8_t { stopped, connecting, reading, writing };

    SaccadeResult create_pipe() noexcept;
    SaccadeResult begin_connect() noexcept;
    SaccadeResult begin_read() noexcept;
    SaccadeResult begin_write(size_t byte_size) noexcept;
    SaccadeResult complete_connect() noexcept;
    SaccadeResult complete_read(uint32_t byte_size, uint64_t now_ns) noexcept;
    SaccadeResult complete_write(uint32_t byte_size) noexcept;
    SaccadeResult reconnect() noexcept;
    SaccadeResult poll(uint64_t now_ns) noexcept;
    SaccadeResult hello(uint32_t byte_size, size_t* output_size) noexcept;
    void close_pipe() noexcept;

    static constexpr size_t security_storage_bytes_ = 512;
    static constexpr size_t endpoint_capacity_ = 128;
    std::array<uint8_t, security_storage_bytes_> token_user_{};
    std::array<uint8_t, security_storage_bytes_> acl_{};
    std::array<wchar_t, endpoint_capacity_> endpoint_{};
    SECURITY_DESCRIPTOR security_descriptor_{};
    SECURITY_ATTRIBUTES security_attributes_{};
    OVERLAPPED operation_{};
    AgentPipeConfig config_{};
    AgentPipeStorage* storage_ = nullptr;
    AgentPipeStats stats_{};
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    size_t write_size_ = 0;
    SaccadeAgentCapabilityBits client_capability_bits_ = 0;
    State state_ = State::stopped;
    bool authenticated_ = false;
    bool initialized_ = false;
};

static_assert(sizeof(AgentPipeStats) == 64);
static_assert(sizeof(AgentPipeStorage) == 2U * SACCADE_AGENT_MAX_MESSAGE_BYTES);
static_assert(sizeof(AgentPipe) < 4096);

} // namespace saccade::platform::windows

#endif

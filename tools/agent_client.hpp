#ifndef SACCADE_TOOLS_AGENT_CLIENT_HPP
#define SACCADE_TOOLS_AGENT_CLIENT_HPP

#include <saccade/saccade_agent.h>

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace saccade::tools {

struct AgentClientStorage {
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> request{};
    alignas(8) std::array<uint8_t, SACCADE_AGENT_MAX_MESSAGE_BYTES> response{};
};

class AgentClient final {
  public:
    bool connect() noexcept;
    bool transact(const void* request, size_t request_size, AgentClientStorage*, size_t* response_size) noexcept;
    bool hello(SaccadeAgentCapabilityBits requested, AgentClientStorage*, SaccadeAgentCapabilityBits* granted) noexcept;
    void close() noexcept;
    ~AgentClient();

  private:
    bool transact_once(const void*, size_t, AgentClientStorage*, size_t*) noexcept;
    bool transact_fresh(const void*, size_t, const SaccadeAgentFreshness&, AgentClientStorage*, size_t*) noexcept;
    bool verify_action(const SaccadeAgentActionBatch&, bool explicit_window, AgentClientStorage*, size_t*) noexcept;

#if defined(__APPLE__)
    bool send_all(const uint8_t*, size_t) noexcept;
    bool receive_all(uint8_t*, size_t) noexcept;
    int socket_ = -1;
#elif defined(_WIN32)
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#endif
};

uint64_t monotonic_time_ns() noexcept;

} // namespace saccade::tools

#endif

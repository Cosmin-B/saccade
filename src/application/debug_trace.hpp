#ifndef SACCADE_APPLICATION_DEBUG_TRACE_HPP
#define SACCADE_APPLICATION_DEBUG_TRACE_HPP

#include <array>
#include <cstdint>

namespace saccade::application {

constexpr uint32_t debug_trace_capacity = 32;

enum class DebugTraceCode : uint16_t {
    runtime_initialized = 1,
    frame_offered,
    semantic_requested,
    scene_published,
    command_dispatched,
    symbol_entered,
    overlay_composed,
    runtime_failure,
    runtime_shutdown
};

struct DebugTraceEvent {
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    uint64_t argument = 0;
    DebugTraceCode code = DebugTraceCode::runtime_initialized;
    uint16_t flags = 0;
    int32_t result = 0;
};

struct DebugTraceSnapshot {
    std::array<DebugTraceEvent, debug_trace_capacity> events{};
    uint64_t next_sequence = 1;
    uint64_t overwritten = 0;
    uint32_t count = 0;
    uint32_t reserved = 0;
};

class DebugTrace final {
  public:
    void reset() noexcept { *this = {}; }

    void record(DebugTraceCode code, uint64_t timestamp_ns, uint64_t argument = 0, int32_t result = 0,
                uint16_t flags = 0) noexcept {
        DebugTraceEvent& event = events_[write_index_];
        event = {next_sequence_++, timestamp_ns, argument, code, flags, result};
        write_index_ = (write_index_ + 1U) % debug_trace_capacity;
        if (count_ != debug_trace_capacity)
            ++count_;
        else
            ++overwritten_;
    }

    [[nodiscard]] DebugTraceSnapshot snapshot() const noexcept {
        DebugTraceSnapshot output{};
        output.next_sequence = next_sequence_;
        output.overwritten = overwritten_;
        output.count = count_;
        const uint32_t oldest = count_ == debug_trace_capacity ? write_index_ : 0;
        for (uint32_t index = 0; index < count_; ++index)
            output.events[index] = events_[(oldest + index) % debug_trace_capacity];
        return output;
    }

  private:
    std::array<DebugTraceEvent, debug_trace_capacity> events_{};
    uint64_t next_sequence_ = 1;
    uint64_t overwritten_ = 0;
    uint32_t write_index_ = 0;
    uint32_t count_ = 0;
};

static_assert(sizeof(DebugTraceEvent) == 32);
static_assert(sizeof(DebugTraceSnapshot) == 1048);
static_assert(sizeof(DebugTrace) == 1048);

} // namespace saccade::application

#endif

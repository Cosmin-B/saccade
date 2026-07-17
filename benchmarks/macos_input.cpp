#include "platform/macos/input_executor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class BenchmarkResult : int { success, initialization_failed, execution_failed, shutdown_failed };

constexpr uint32_t command_count = SACCADE_INPUT_PLAN_MAX_COMMANDS;
constexpr uint32_t warmup_count = 100;
constexpr uint32_t sample_count = 1000;
constexpr uint32_t coordinate_fraction_bits = 8;
constexpr int32_t coordinate_scale = INT32_C(1) << coordinate_fraction_bits;
constexpr uint32_t coordinate_row_width = 32;
constexpr uint32_t desktop_width = 4096;
constexpr uint32_t desktop_height = 2160;
constexpr uint64_t plan_id = 1;
constexpr uint64_t scene_epoch = 2;
constexpr uint64_t frame_id = 3;
constexpr uint64_t model_epoch = 4;
constexpr uint64_t session_epoch = 5;
constexpr uint64_t transform_epoch = 6;
constexpr uint64_t topology_epoch = 7;
constexpr uint64_t permission_epoch = 8;
constexpr uint64_t source_id = 9;
constexpr uint64_t execution_time_ns = 1;
constexpr uint64_t interaction_budget_ns = UINT64_C(8333333);
constexpr size_t packet_size =
    sizeof(SaccadeInputPlanHeader) + static_cast<size_t>(command_count) * sizeof(SaccadeInputCommand);

struct alignas(SaccadeInputPlanHeader) PlanStorage {
    std::array<uint8_t, packet_size> bytes{};
};

struct Sink {
    uint64_t events = 0;
};

int exit_code(BenchmarkResult value) noexcept {
    return static_cast<int>(value);
}

bool consume_event(void* context, CGEventRef) noexcept {
    ++static_cast<Sink*>(context)->events;
    return true;
}

SaccadeSpanU8 make_plan(PlanStorage* storage) noexcept {
    SaccadeInputPlanHeader header{};
    header.struct_size = sizeof(header);
    header.plan_version = SACCADE_INPUT_PLAN_VERSION;
    header.command_count = command_count;
    header.command_stride = sizeof(SaccadeInputCommand);
    header.flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    header.required_permissions = SACCADE_INPUT_PERMISSION_POINTER;
    header.plan_id = plan_id;
    header.scene_epoch = scene_epoch;
    header.frame_id = frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.permission_epoch = permission_epoch;
    header.source_id = source_id;
    header.deadline_ns = UINT64_MAX;
    header.commands_offset = sizeof(header);
    header.total_size = packet_size;
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    auto* commands = reinterpret_cast<SaccadeInputCommand*>(storage->bytes.data() + sizeof(header));
    for (uint32_t index = 0; index < command_count; ++index) {
        SaccadeInputCommand& command = commands[index];
        command.kind = SACCADE_INPUT_COMMAND_POINTER_MOVE;
        command.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
        command.target_id = index + 1U;
        command.x_q8 = static_cast<int32_t>(index) * coordinate_scale;
        command.y_q8 = static_cast<int32_t>(index % coordinate_row_width) * coordinate_scale;
    }
    return {storage->bytes.data(), storage->bytes.size()};
}

} // namespace

int main() {
    static PlanStorage storage;
    const SaccadeSpanU8 plan = make_plan(&storage);
    Sink sink{};
    saccade::platform::macos::InputExecutor executor;
    const saccade::platform::macos::Desktop desktop{0, 0, desktop_width, desktop_height, topology_epoch};
    const saccade::platform::macos::InputSink input_sink{&sink, consume_event, nullptr};
    if (executor.initialize(desktop, input_sink, permission_epoch, 0, 0) != SACCADE_OK)
        return exit_code(BenchmarkResult::initialization_failed);
    std::array<uint64_t, sample_count> samples{};
    for (uint32_t iteration = 0; iteration < warmup_count + sample_count; ++iteration) {
        saccade::platform::macos::InputExecutionResult execution{};
        const auto start = std::chrono::steady_clock::now();
        const SaccadeResult executed =
            executor.execute(plan, SACCADE_INPUT_PERMISSION_POINTER, execution_time_ns, &execution);
        const auto finish = std::chrono::steady_clock::now();
        if (executed != SACCADE_OK || execution.commands_completed != command_count ||
            execution.native_events != command_count)
            return exit_code(BenchmarkResult::execution_failed);
        if (iteration >= warmup_count) {
            samples[iteration - warmup_count] =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        }
    }
    std::sort(samples.begin(), samples.end());
    const uint64_t median = samples[sample_count / 2U];
    const uint64_t p95 = samples[sample_count * 95U / 100U];
    std::printf("macos_input commands=%u median_ns=%llu p95_ns=%llu "
                "per_event_ns=%.1f interaction_budget_pct=%.3f\n",
                command_count, static_cast<unsigned long long>(median), static_cast<unsigned long long>(p95),
                static_cast<double>(p95) / command_count, static_cast<double>(p95) / interaction_budget_ns * 100.0);
    return executor.shutdown() == SACCADE_OK ? exit_code(BenchmarkResult::success)
                                             : exit_code(BenchmarkResult::shutdown_failed);
}

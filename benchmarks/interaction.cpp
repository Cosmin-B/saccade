#include "interaction/action_planner.hpp"
#include "interaction/hints.hpp"
#include "interaction/selection_reducer.hpp"
#include "scene/packet.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    packet_failure = 1,
    hint_failure = 2,
    benchmark_failure = 3,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t target_count = SACCADE_TARGET_PACKET_MAX_TARGETS;
constexpr size_t scene_bytes = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord);
constexpr uint64_t interaction_budget_ns = UINT64_C(8333333);

struct alignas(8) SceneStorage {
    std::array<uint8_t, scene_bytes> bytes{};
};

uint64_t benchmark_sink = 0;

void make_scene(SceneStorage* storage) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = 1;
    header.frame_id = 2;
    header.model_epoch = 3;
    header.session_epoch = 4;
    header.transform_epoch = 5;
    header.topology_epoch = 6;
    header.source_id = 7;
    header.targets_offset = sizeof(header);
    header.total_size = storage->bytes.size();
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(storage->bytes.data() + sizeof(header));
    for (uint32_t index = 0; index < target_count; ++index) {
        SaccadeTargetRecord& target = targets[index];
        target.target_id = static_cast<uint64_t>(index) + 1U;
        target.window_id = 1;
        target.display_id = 1;
        target.x_q8 = static_cast<int32_t>((index % 100U) * 256U);
        target.y_q8 = static_cast<int32_t>((index / 100U) * 256U);
        target.width_q8 = 192;
        target.height_q8 = 192;
        target.safe_x_q8 = target.x_q8 + 96;
        target.safe_y_q8 = target.y_q8 + 96;
        target.confidence_q16 = UINT16_MAX;
        target.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON |
                                 SACCADE_TARGET_CAPABILITY_SCROLL;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }
}

template <typename Operation> uint64_t measure(uint32_t iterations, Operation operation) noexcept {
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < iterations; ++index) {
        benchmark_sink += operation();
    }
    const auto end = std::chrono::steady_clock::now();
    const uint64_t elapsed =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    return elapsed / iterations;
}

void report(const char* name, uint64_t ns) noexcept {
    const double budget = static_cast<double>(ns) * 100.0 / static_cast<double>(interaction_budget_ns);
    std::printf("%-28s %10llu ns  %6.3f%% of 120 Hz\n", name, static_cast<unsigned long long>(ns), budget);
}

} // namespace

int main() {
    static SceneStorage scene_storage;
    static saccade::interaction::HintSessionStorage hint_storage;
    static saccade::interaction::SelectionStorage selection_storage;
    static saccade::interaction::ActionPlanStorage plan_storage;
    make_scene(&scene_storage);
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet({scene_storage.bytes.data(), scene_storage.bytes.size()}, &scene) !=
        SACCADE_OK) {
        return to_process_exit_code(ExitCode::packet_failure);
    }

    constexpr std::array<uint16_t, 10> alphabet{'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ';'};
    saccade::interaction::HintConfig hint_config{};
    std::copy(alphabet.begin(), alphabet.end(), hint_config.alphabet.begin());
    hint_config.alphabet_count = static_cast<uint32_t>(alphabet.size());
    hint_config.priority = saccade::interaction::HintPriority::pointer;
    hint_config.pointer_x_q8 = 12800;
    hint_config.pointer_y_q8 = 12800;
    saccade::interaction::HintSession hints;
    const uint64_t freeze_ns = measure(200, [&]() noexcept -> uint64_t {
        if (hints.freeze(scene, hint_config, &hint_storage) != SACCADE_OK) {
            return 0;
        }
        const uint64_t count = hints.label_count();
        (void)hints.cancel();
        return count;
    });

    if (hints.freeze(scene, hint_config, &hint_storage) != SACCADE_OK) {
        return to_process_exit_code(ExitCode::hint_failure);
    }
    const saccade::interaction::HintLabel query = hints.labels()[0];
    const uint64_t prefix_ns = measure(1000, [&]() noexcept -> uint64_t {
        saccade::interaction::HintMatch match{};
        if (hints.resolve_prefix(query.symbols.data(), query.symbol_count, &match) != SACCADE_OK) {
            return 0;
        }
        return match.target_id;
    });
    (void)hints.cancel();

    saccade::interaction::SelectionContext selection_context{1, 5, 6, 1, 1000};
    saccade::interaction::SelectionReducer selection;
    const uint64_t path_ns = measure(10000, [&]() noexcept -> uint64_t {
        if (selection.begin(scene, saccade::interaction::SelectionMode::path, selection_context, &selection_storage) !=
                SACCADE_OK ||
            selection.select(1) != SACCADE_OK || selection.select(64) != SACCADE_OK) {
            return 0;
        }
        const uint64_t count = selection.view().target_count;
        (void)selection.reset();
        return count;
    });

    std::array<uint64_t, saccade::interaction::maximum_action_targets> target_ids{};
    for (uint32_t index = 0; index < target_ids.size(); ++index) {
        target_ids[index] = target_count - index;
    }
    saccade::interaction::ActionContext action_context{};
    action_context.plan_id = 1;
    action_context.scene_epoch = 1;
    action_context.transform_epoch = 5;
    action_context.topology_epoch = 6;
    action_context.permission_epoch = 1;
    action_context.focus_id = 1;
    action_context.now_ns = 1;
    action_context.deadline_ns = 1000;
    action_context.permissions = SACCADE_INPUT_PERMISSION_POINTER;
    saccade::interaction::ActionRequest request{};
    request.kind = saccade::interaction::ActionKind::click;
    request.target_ids = target_ids.data();
    request.target_count = static_cast<uint32_t>(target_ids.size());
    saccade::interaction::ActionPlanner planner;
    const uint64_t batch_ns = measure(1000, [&]() noexcept -> uint64_t {
        SaccadeSpanU8 output{};
        if (planner.build(scene, action_context, request, &plan_storage, &output) != SACCADE_OK) {
            return 0;
        }
        return output.size;
    });

    report("hint freeze (10,000)", freeze_ns);
    report("exact prefix (10,000)", prefix_ns);
    report("path 64 of 10,000", path_ns);
    report("batch plan 64 of 10,000", batch_ns);
    return to_process_exit_code(benchmark_sink == 0 ? ExitCode::benchmark_failure : ExitCode::success);
}

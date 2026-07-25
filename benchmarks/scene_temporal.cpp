#include "scene/temporal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

enum class ExitCode : int { success = 0, initialize_failed = 1, compile_failed = 2 };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

struct PacketStorage {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, SACCADE_TARGET_PACKET_MAX_TARGETS> targets{};
};

void initialize(PacketStorage* packet, uint32_t count, uint64_t scene_epoch, int32_t changed_offset) noexcept {
    packet->header = {};
    packet->header.struct_size = sizeof(packet->header);
    packet->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    packet->header.target_count = count;
    packet->header.target_stride = sizeof(SaccadeTargetRecord);
    packet->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    packet->header.scene_epoch = scene_epoch;
    packet->header.frame_id = scene_epoch;
    packet->header.capture_time_ns = scene_epoch * 1000;
    packet->header.model_epoch = 2;
    packet->header.session_epoch = 3;
    packet->header.transform_epoch = 4;
    packet->header.topology_epoch = 5;
    packet->header.source_id = 6;
    packet->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    packet->header.total_size =
        sizeof(SaccadeTargetPacketHeader) + static_cast<size_t>(count) * sizeof(SaccadeTargetRecord);
    for (uint32_t index = 0; index < count; ++index) {
        SaccadeTargetRecord& target = packet->targets[index];
        target = {};
        target.target_id = index + 1U;
        target.window_id = index / 1000U + 1U;
        target.display_id = 7;
        target.x_q8 = static_cast<int32_t>(index % 100U) * 8192;
        target.y_q8 = static_cast<int32_t>(index / 100U) * 8192;
        target.width_q8 = 5120;
        target.height_q8 = 4096;
        target.safe_x_q8 = target.x_q8 + 2560;
        target.safe_y_q8 = target.y_q8 + 2048;
        target.confidence_q16 = 50000U + index % 1000U;
        target.role = SACCADE_TARGET_ROLE_BUTTON;
        target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
        if (index % 20U == 0) {
            target.x_q8 += changed_offset;
            target.safe_x_q8 += changed_offset;
            target.confidence_q16 += static_cast<uint32_t>(changed_offset != 0);
        }
    }
}

saccade::scene::PacketView view(PacketStorage* packet) noexcept {
    return {&packet->header, packet->targets.data(), static_cast<size_t>(packet->header.total_size), nullptr, 0};
}

bool run_case(uint32_t target_count) {
    static PacketStorage packets[2];
    static saccade::scene::TemporalStorage storage;
    alignas(SaccadeSceneDeltaHeader) static std::array<uint8_t, saccade::scene::temporal_delta_max_bytes> output{};

    initialize(&packets[0], target_count, 1, 0);
    initialize(&packets[1], target_count, 2, 64);
    saccade::scene::TemporalCompiler compiler;
    if (compiler.initialize(&storage) != SACCADE_OK) {
        return false;
    }

    size_t required = 0;
    saccade::scene::TemporalStats stats{};
    if (compiler.compile(view(&packets[0]), {output.data(), output.size()}, &required, &stats) != SACCADE_OK) {
        return false;
    }

    constexpr uint32_t warmups = 30;
    constexpr uint32_t samples = 400;
    std::array<uint64_t, samples> times{};
    uint64_t output_bytes = 0;
    for (uint32_t iteration = 0; iteration < warmups + samples; ++iteration) {
        PacketStorage& packet = packets[(iteration + 1U) & 1U];
        packet.header.scene_epoch = iteration + 2U;
        packet.header.frame_id = iteration + 2U;
        const auto start = std::chrono::steady_clock::now();
        const SaccadeResult result = compiler.compile(view(&packet), {output.data(), output.size()}, &required, &stats);
        const auto finish = std::chrono::steady_clock::now();
        const uint32_t expected_updates = (target_count + 19U) / 20U;
        if (result != SACCADE_OK || stats.updates != expected_updates || stats.additions != 0 || stats.removals != 0) {
            return false;
        }
        if (iteration >= warmups) {
            times[iteration - warmups] =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
            output_bytes += required;
        }
    }

    std::sort(times.begin(), times.end());
    const uint64_t median = times[samples / 2U];
    const uint64_t p95 = times[samples * 95U / 100U];
    std::printf("scene_temporal targets=%u changed=%u median_ns=%llu p95_ns=%llu output_bytes=%llu "
                "budget_120hz_pct=%.3f budget_30hz_pct=%.3f\n",
                target_count, (target_count + 19U) / 20U, static_cast<unsigned long long>(median),
                static_cast<unsigned long long>(p95), static_cast<unsigned long long>(output_bytes / samples),
                static_cast<double>(p95) / 8333333.0 * 100.0, static_cast<double>(p95) / 33333333.0 * 100.0);
    return true;
}

void initialize_window_move(PacketStorage* packet, uint32_t count, uint64_t scene_epoch, uint64_t transform_epoch,
                            bool moved) noexcept {
    initialize(packet, count, scene_epoch, 0);
    packet->header.transform_epoch = transform_epoch;
    for (uint32_t index = 0; index < count; ++index) {
        SaccadeTargetRecord& target = packet->targets[index];
        target.window_id = index % 10U + 1U;
        if (moved) {
            const int32_t translation = static_cast<int32_t>(target.window_id) * 16;
            target.x_q8 += translation;
            target.safe_x_q8 += translation;
        }
    }
}

bool run_window_case(uint32_t target_count) {
    static PacketStorage packets[2];
    static saccade::scene::TemporalStorage storage;
    alignas(SaccadeSceneDeltaHeader) static std::array<uint8_t, saccade::scene::temporal_delta_max_bytes> output{};

    initialize_window_move(&packets[0], target_count, 1, 1, false);
    initialize_window_move(&packets[1], target_count, 2, 2, true);
    saccade::scene::TemporalCompiler compiler;
    if (compiler.initialize(&storage) != SACCADE_OK) {
        return false;
    }

    size_t required = 0;
    saccade::scene::TemporalStats stats{};
    if (compiler.compile(view(&packets[0]), {output.data(), output.size()}, &required, &stats) != SACCADE_OK) {
        return false;
    }

    constexpr uint32_t warmups = 30;
    constexpr uint32_t samples = 400;
    std::array<uint64_t, samples> times{};
    uint64_t output_bytes = 0;
    for (uint32_t iteration = 0; iteration < warmups + samples; ++iteration) {
        PacketStorage& packet = packets[(iteration + 1U) & 1U];
        packet.header.scene_epoch = iteration + 2U;
        packet.header.frame_id = iteration + 2U;
        packet.header.transform_epoch = iteration + 2U;
        const auto start = std::chrono::steady_clock::now();
        const SaccadeResult result = compiler.compile(view(&packet), {output.data(), output.size()}, &required, &stats);
        const auto finish = std::chrono::steady_clock::now();
        if (result != SACCADE_OK || stats.window_transforms != 10 || stats.updates != 0 || stats.additions != 0 ||
            stats.removals != 0) {
            return false;
        }
        if (iteration >= warmups) {
            times[iteration - warmups] =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
            output_bytes += required;
        }
    }

    std::sort(times.begin(), times.end());
    const uint64_t median = times[samples / 2U];
    const uint64_t p95 = times[samples * 95U / 100U];
    std::printf("scene_temporal_window_move targets=%u windows=10 target_updates=0 median_ns=%llu p95_ns=%llu "
                "output_bytes=%llu budget_120hz_pct=%.3f budget_30hz_pct=%.3f\n",
                target_count, static_cast<unsigned long long>(median), static_cast<unsigned long long>(p95),
                static_cast<unsigned long long>(output_bytes / samples), static_cast<double>(p95) / 8333333.0 * 100.0,
                static_cast<double>(p95) / 33333333.0 * 100.0);
    return true;
}

} // namespace

int main() {
    for (const uint32_t target_count : {100U, 1000U, 10000U}) {
        if (!run_case(target_count) || !run_window_case(target_count)) {
            return exit_code(ExitCode::compile_failed);
        }
    }
    return exit_code(ExitCode::success);
}

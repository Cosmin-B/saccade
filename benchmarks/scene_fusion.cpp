#include "scene/fusion.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

enum class ExitCode : int { success = 0, fusion_failed = 1 };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

struct PacketStorage {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, SACCADE_TARGET_PACKET_MAX_TARGETS> targets{};
};

constexpr size_t packet_bytes = sizeof(SaccadeTargetPacketHeader) +
                                static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
constexpr size_t output_bytes = packet_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;

void initialize(PacketStorage* packet, uint16_t source, int32_t offset, uint64_t model_epoch) {
    packet->header.struct_size = sizeof(packet->header);
    packet->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    packet->header.target_count = SACCADE_TARGET_PACKET_MAX_TARGETS;
    packet->header.target_stride = sizeof(SaccadeTargetRecord);
    packet->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    packet->header.scene_epoch = 1;
    packet->header.frame_id = 2;
    packet->header.model_epoch = model_epoch;
    packet->header.session_epoch = 3;
    packet->header.transform_epoch = 4;
    packet->header.topology_epoch = 5;
    packet->header.source_id = 6;
    packet->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    packet->header.total_size = packet_bytes;
    for (uint32_t index = 0; index < SACCADE_TARGET_PACKET_MAX_TARGETS; ++index) {
        SaccadeTargetRecord& target = packet->targets[index];
        target.target_id = static_cast<uint64_t>(source) << 48U | index + 1U;
        target.window_id = index / 1000U + 1U;
        target.display_id = 7;
        target.x_q8 = (static_cast<int32_t>(index % 100U) * 32 - 1600 + offset) * 256;
        target.y_q8 = (static_cast<int32_t>(index / 100U) * 32 - 800 + offset) * 256;
        target.width_q8 = 20 * 256;
        target.height_q8 = 16 * 256;
        target.safe_x_q8 = target.x_q8 + 10 * 256;
        target.safe_y_q8 = target.y_q8 + 8 * 256;
        target.confidence_q16 = static_cast<uint32_t>(50000U + index % 1000U);
        target.role = static_cast<SaccadeTargetRole>(
            source == SACCADE_TARGET_SOURCE_ACCESSIBILITY ? SACCADE_TARGET_ROLE_BUTTON : SACCADE_TARGET_ROLE_UNKNOWN);
        target.source_bits = source;
        target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
        target.flags = SACCADE_TARGET_ACTIONABLE;
        target.order = index;
    }
}

} // namespace

int main() {
    static PacketStorage semantic;
    static PacketStorage neural;
    static saccade::scene::FusionWorkspace workspace;
    alignas(SaccadeTargetPacketHeader) static std::array<uint8_t, output_bytes> output{};
    initialize(&semantic, SACCADE_TARGET_SOURCE_ACCESSIBILITY, 0, 100);
    initialize(&neural, SACCADE_TARGET_SOURCE_NEURAL, 1, 200);
    const std::array<saccade::scene::PacketView, 2> packets = {
        {{&semantic.header, semantic.targets.data(), packet_bytes},
         {&neural.header, neural.targets.data(), packet_bytes}}};
    saccade::scene::FusionConfig config{};
    saccade::scene::FusionEpochs epochs{};
    epochs.scene_epoch = 10;
    epochs.frame_id = 2;
    epochs.model_epoch = 200;
    epochs.session_epoch = 3;
    epochs.transform_epoch = 4;
    epochs.topology_epoch = 5;
    epochs.source_id = 6;

    constexpr uint32_t warmups = 20;
    constexpr uint32_t samples = 200;
    std::array<uint64_t, samples> times{};
    for (uint32_t iteration = 0; iteration < warmups + samples; ++iteration) {
        size_t required = 0;
        saccade::scene::FusionStats stats{};
        const auto start = std::chrono::steady_clock::now();
        const SaccadeResult result =
            saccade::scene::fuse(packets.data(), static_cast<uint32_t>(packets.size()), config, epochs, &workspace,
                                 {output.data(), output.size()}, &required, &stats);
        const auto finish = std::chrono::steady_clock::now();
        if (result != SACCADE_OK || stats.targets_written != 10000 || stats.duplicates_merged != 10000 ||
            required != packet_bytes) {
            return exit_code(ExitCode::fusion_failed);
        }
        if (iteration >= warmups) {
            times[iteration - warmups] =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        }
    }
    std::sort(times.begin(), times.end());
    const uint64_t median = times[samples / 2];
    const uint64_t p95 = times[samples * 95 / 100];
    std::printf("scene_fusion candidates=20000 targets=10000 median_ns=%llu p95_ns=%llu "
                "interaction_budget_pct=%.3f scene_budget_pct=%.3f\n",
                static_cast<unsigned long long>(median), static_cast<unsigned long long>(p95),
                static_cast<double>(p95) / 8333333.0 * 100.0, static_cast<double>(p95) / 33333333.0 * 100.0);
    return exit_code(ExitCode::success);
}

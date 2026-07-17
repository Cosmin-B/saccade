#include "scene/fusion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using saccade::scene::FusionConfig;
using saccade::scene::FusionEpochs;
using saccade::scene::FusionStats;
using saccade::scene::FusionWorkspace;
using saccade::scene::PacketView;

struct PacketStorage {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, SACCADE_TARGET_PACKET_MAX_TARGETS> targets{};
};

struct TextPacketStorage {
    SaccadeTargetPacketHeader header{};
    SaccadeTargetRecord target{};
    std::array<uint8_t, 6> text{'B', 'u', 't', 't', 'o', 'n'};
};

constexpr size_t maximum_packet_size =
    sizeof(SaccadeTargetPacketHeader) +
    static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord) +
    SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;

SaccadeTargetRecord target(uint64_t id, int32_t x, int32_t y, SaccadeTargetRole role, uint16_t source) {
    SaccadeTargetRecord value{};
    value.target_id = id;
    value.window_id = 50;
    value.display_id = 60;
    value.x_q8 = x * 256;
    value.y_q8 = y * 256;
    value.width_q8 = 20 * 256;
    value.height_q8 = 16 * 256;
    value.safe_x_q8 = value.x_q8 + 10 * 256;
    value.safe_y_q8 = value.y_q8 + 8 * 256;
    value.confidence_q16 = 50000;
    value.role = role;
    value.source_bits = source;
    value.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    value.flags = SACCADE_TARGET_ACTIONABLE;
    return value;
}

PacketView packet(PacketStorage* storage, uint32_t count, uint64_t model_epoch = 70) {
    storage->header = {};
    storage->header.struct_size = sizeof(storage->header);
    storage->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    storage->header.target_count = count;
    storage->header.target_stride = sizeof(SaccadeTargetRecord);
    storage->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    storage->header.scene_epoch = 2;
    storage->header.frame_id = 3;
    storage->header.model_epoch = model_epoch;
    storage->header.session_epoch = 4;
    storage->header.transform_epoch = 5;
    storage->header.topology_epoch = 6;
    storage->header.source_id = 7;
    storage->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    storage->header.total_size =
        sizeof(SaccadeTargetPacketHeader) + static_cast<uint64_t>(count) * sizeof(SaccadeTargetRecord);
    return {&storage->header, storage->targets.data(), static_cast<size_t>(storage->header.total_size)};
}

PacketView packet(TextPacketStorage* storage, SaccadeTargetRecord value, uint64_t model_epoch) {
    storage->header = {};
    storage->header.struct_size = sizeof(storage->header);
    storage->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    storage->header.target_count = 1;
    storage->header.target_stride = sizeof(SaccadeTargetRecord);
    storage->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    storage->header.scene_epoch = 2;
    storage->header.frame_id = 3;
    storage->header.model_epoch = model_epoch;
    storage->header.session_epoch = 4;
    storage->header.transform_epoch = 5;
    storage->header.topology_epoch = 6;
    storage->header.source_id = 7;
    storage->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    storage->header.total_size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord) + storage->text.size();
    value.text = {0, static_cast<uint16_t>(storage->text.size())};
    storage->target = value;
    return {&storage->header, &storage->target, static_cast<size_t>(storage->header.total_size), storage->text.data(),
            static_cast<uint32_t>(storage->text.size())};
}

FusionEpochs epochs() {
    FusionEpochs value{};
    value.scene_epoch = 100;
    value.frame_id = 3;
    value.model_epoch = 70;
    value.session_epoch = 4;
    value.transform_epoch = 5;
    value.topology_epoch = 6;
    value.source_id = 7;
    return value;
}

} // namespace

int main() {
    static PacketStorage semantic;
    static PacketStorage neural;
    static PacketStorage grid;
    static FusionWorkspace workspace;
    alignas(SaccadeTargetPacketHeader) static std::array<uint8_t, maximum_packet_size> output{};

    semantic.targets[0] = target(10, -100, 20, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_ACCESSIBILITY);
    semantic.targets[0].flags = SACCADE_TARGET_DISABLED;
    semantic.targets[0].capability_bits = 0;
    neural.targets[0] = target(20, -99, 21, SACCADE_TARGET_ROLE_UNKNOWN, SACCADE_TARGET_SOURCE_NEURAL);
    grid.targets[0] = target(30, 400, 300, SACCADE_TARGET_ROLE_UNKNOWN, SACCADE_TARGET_SOURCE_GRID);
    const std::array<PacketView, 3> inputs = {packet(&semantic, 1, 900), packet(&neural, 1), packet(&grid, 1, 901)};
    grid.header.flags = SACCADE_TARGET_PACKET_INCOMPLETE;

    FusionStats stats{};
    size_t required = 0;
    FusionConfig config{};
    config.maximum_targets = 8;
    if (saccade::scene::fuse(inputs.data(), static_cast<uint32_t>(inputs.size()), config, epochs(), &workspace,
                             {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        stats.targets_written != 2 || stats.duplicates_merged != 1 || stats.safety_merges != 1) {
        return 1;
    }
    saccade::scene::PacketView fused{};
    if (saccade::scene::validate_packet({output.data(), required}, &fused) != SACCADE_OK ||
        fused.header->scene_epoch != 100 || fused.header->target_count != 2 ||
        fused.header->flags != SACCADE_TARGET_PACKET_INCOMPLETE || fused.targets[0].target_id != 10 ||
        fused.targets[0].source_bits != (SACCADE_TARGET_SOURCE_ACCESSIBILITY | SACCADE_TARGET_SOURCE_NEURAL) ||
        fused.targets[0].flags != SACCADE_TARGET_DISABLED || fused.targets[0].capability_bits != 0 ||
        fused.targets[1].target_id != 30) {
        return 2;
    }

    config.merge_duplicates = false;
    if (saccade::scene::fuse(inputs.data(), static_cast<uint32_t>(inputs.size()), config, epochs(), &workspace,
                             {output.data(), output.size()}, &required, &stats) != SACCADE_OK ||
        stats.targets_written != 3 || stats.duplicates_merged != 0 || stats.safety_merges != 0 ||
        saccade::scene::validate_packet({output.data(), required}, &fused) != SACCADE_OK ||
        fused.header->target_count != 3 || fused.targets[0].source_bits != SACCADE_TARGET_SOURCE_ACCESSIBILITY ||
        fused.targets[1].source_bits != SACCADE_TARGET_SOURCE_NEURAL) {
        return 7;
    }
    config.merge_duplicates = true;

    static TextPacketStorage labeled;
    SaccadeTargetRecord semantic_label =
        target(40, 20, 20, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_ACCESSIBILITY);
    SaccadeTargetRecord neural_label = target(41, 21, 21, SACCADE_TARGET_ROLE_UNKNOWN, SACCADE_TARGET_SOURCE_NEURAL);
    neural_label.capability_bits |= SACCADE_TARGET_CAPABILITY_SCROLL | SACCADE_TARGET_CAPABILITY_DRAG_SOURCE |
                                    SACCADE_TARGET_CAPABILITY_DROP_TARGET | SACCADE_TARGET_CAPABILITY_TEXT;
    neural.targets[0] = neural_label;
    const PacketView label_inputs[] = {packet(&neural, 1), packet(&labeled, semantic_label, 900)};
    config.maximum_targets = 2;
    if (saccade::scene::fuse(label_inputs, 2, config, epochs(), &workspace, {output.data(), output.size()}, &required,
                             &stats) != SACCADE_OK ||
        saccade::scene::validate_packet({output.data(), required}, &fused) != SACCADE_OK ||
        fused.header->target_count != 1 || fused.target_text(0).size != 6 ||
        fused.targets[0].capability_bits !=
            (SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON) ||
        std::memcmp(fused.target_text(0).data, "Button", 6) != 0) {
        return 3;
    }

    FusionEpochs stale = epochs();
    stale.topology_epoch = 99;
    if (saccade::scene::fuse(inputs.data(), 3, config, stale, &workspace, {output.data(), output.size()}, &required,
                             &stats) != SACCADE_ERROR_STALE_HANDLE) {
        return 4;
    }
    config.maximum_targets = 8;
    if (saccade::scene::fuse(inputs.data(), 3, config, epochs(), &workspace, {output.data(), 64}, &required, &stats) !=
            SACCADE_ERROR_CAPACITY ||
        required != sizeof(SaccadeTargetPacketHeader) + 8 * sizeof(SaccadeTargetRecord) +
                        SACCADE_TARGET_PACKET_MAX_TEXT_BYTES) {
        return 5;
    }

    for (uint32_t index = 0; index < SACCADE_TARGET_PACKET_MAX_TARGETS; ++index) {
        const int32_t x = static_cast<int32_t>(index % 100U) * 32 - 1600;
        const int32_t y = static_cast<int32_t>(index / 100U) * 32 - 800;
        neural.targets[index] = target(1000 + index, x, y, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL);
    }
    const PacketView full = packet(&neural, SACCADE_TARGET_PACKET_MAX_TARGETS);
    config.maximum_targets = SACCADE_TARGET_PACKET_MAX_TARGETS;
    if (saccade::scene::fuse(&full, 1, config, epochs(), &workspace, {output.data(), output.size()}, &required,
                             &stats) != SACCADE_OK ||
        stats.targets_written != SACCADE_TARGET_PACKET_MAX_TARGETS || stats.duplicates_merged != 0 ||
        stats.capacity_drops != 0 || saccade::scene::validate_packet({output.data(), required}, &fused) != SACCADE_OK) {
        return 6;
    }
    return 0;
}

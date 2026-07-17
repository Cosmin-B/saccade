#include "interaction/hints.hpp"
#include "scene/packet.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

enum class ExitCode : int {
    success = 0,
    freeze = 1,
    resolve = 2,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t target_count = 137;
constexpr size_t packet_size = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord);

struct alignas(8) PacketStorage {
    std::array<uint8_t, packet_size> bytes{};
};

void make_scene(PacketStorage* storage) noexcept {
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
        targets[index].target_id = index + 1U;
        targets[index].x_q8 = static_cast<int32_t>(index * 512U);
        targets[index].width_q8 = 256;
        targets[index].height_q8 = 256;
        targets[index].safe_x_q8 = targets[index].x_q8 + 128;
        targets[index].safe_y_q8 = 128;
        targets[index].confidence_q16 = UINT16_MAX;
        targets[index].source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        targets[index].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
        targets[index].flags = SACCADE_TARGET_ACTIONABLE;
        targets[index].order = index;
    }
}

bool prefix_free(const saccade::interaction::HintLabel* labels, uint32_t count) noexcept {
    for (uint32_t left = 0; left < count; ++left) {
        for (uint32_t right = left + 1; right < count; ++right) {
            const uint32_t common = std::min<uint32_t>(labels[left].symbol_count, labels[right].symbol_count);
            bool equal = true;
            for (uint32_t index = 0; index < common; ++index) {
                equal &= labels[left].symbols[index] == labels[right].symbols[index];
            }
            if (equal) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    static PacketStorage packet;
    static saccade::interaction::HintSessionStorage storage;
    make_scene(&packet);
    saccade::scene::PacketView scene{};
    const std::array<uint16_t, 5> alphabet{'A', 'S', 'D', 'F', 'G'};
    saccade::interaction::HintConfig config{};
    std::copy(alphabet.begin(), alphabet.end(), config.alphabet.begin());
    config.alphabet_count = static_cast<uint32_t>(alphabet.size());
    config.priority = saccade::interaction::HintPriority::pointer;
    config.pointer_x_q8 = static_cast<int32_t>(80U * 512U + 128U);
    saccade::interaction::HintSession session;
    saccade::test::begin_allocation_tracking();
    const SaccadeResult result =
        saccade::scene::validate_packet({packet.bytes.data(), packet.bytes.size()}, &scene) == SACCADE_OK
            ? session.freeze(scene, config, &storage)
            : SACCADE_ERROR_INVALID_ARGUMENT;
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (result != SACCADE_OK || allocations != 0 || session.label_count() != target_count ||
        session.labels()[0].target_id != 81 || !prefix_free(session.labels(), session.label_count())) {
        return to_process_exit_code(ExitCode::freeze);
    }
    const auto& label = session.labels()[37];
    saccade::interaction::HintMatch match{};
    if (session.resolve_prefix(label.symbols.data(), 1, &match) != SACCADE_OK || match.candidate_count == 0 ||
        session.resolve_prefix(label.symbols.data(), label.symbol_count, &match) != SACCADE_OK || !match.exact ||
        match.target_id != label.target_id || session.cancel() != SACCADE_OK ||
        session.cancel() != SACCADE_ERROR_STATE) {
        return to_process_exit_code(ExitCode::resolve);
    }
    return to_process_exit_code(ExitCode::success);
}

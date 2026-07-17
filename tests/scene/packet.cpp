#include "scene/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::array<uint8_t, 8> text{'O', 'K', 'C', 'a', 'f', 0xC3, 0xA9, '!'};
constexpr size_t packet_size = sizeof(SaccadeTargetPacketHeader) + 2U * sizeof(SaccadeTargetRecord) + text.size();

struct alignas(8) PacketBytes {
    std::array<uint8_t, packet_size> bytes{};
};

PacketBytes make_packet() noexcept {
    PacketBytes storage{};
    auto& bytes = storage.bytes;
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = 2;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_SOURCE_Q8;
    header.frame_id = 1;
    header.model_epoch = 2;
    header.session_epoch = 3;
    header.transform_epoch = 4;
    header.topology_epoch = 5;
    header.source_id = 6;
    header.targets_offset = sizeof(header);
    header.total_size = bytes.size();
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::array<SaccadeTargetRecord, 2> targets{};
    targets[0].target_id = 1;
    targets[0].width_q8 = 256;
    targets[0].height_q8 = 256;
    targets[0].safe_x_q8 = 128;
    targets[0].safe_y_q8 = 128;
    targets[0].confidence_q16 = 60000;
    targets[0].source_bits = SACCADE_TARGET_SOURCE_NEURAL;
    targets[0].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    targets[0].flags = SACCADE_TARGET_ACTIONABLE;
    targets[0].text = {0, 2};
    targets[1].target_id = 9;
    targets[1].x_q8 = -256;
    targets[1].y_q8 = 512;
    targets[1].width_q8 = 512;
    targets[1].height_q8 = 256;
    targets[1].safe_x_q8 = 0;
    targets[1].safe_y_q8 = 640;
    targets[1].confidence_q16 = 50000;
    targets[1].role = SACCADE_TARGET_ROLE_BUTTON;
    targets[1].source_bits = SACCADE_TARGET_SOURCE_NEURAL | SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    targets[1].capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    targets[1].flags = SACCADE_TARGET_ACTIONABLE;
    targets[1].text = {2, 6};
    std::memcpy(bytes.data() + sizeof(header), targets.data(), sizeof(targets));
    std::memcpy(bytes.data() + sizeof(header) + sizeof(targets), text.data(), text.size());
    return storage;
}

} // namespace

int main() {
    auto storage = make_packet();
    auto& bytes = storage.bytes;
    saccade::scene::PacketView view{};
    if (saccade::scene::validate_packet({bytes.data(), bytes.size()}, &view) != SACCADE_OK || view.header == nullptr ||
        view.targets == nullptr || view.header->target_count != 2 || view.targets[1].target_id != 9 ||
        view.byte_size != bytes.size() || view.text_size != text.size() || view.target_text(0).size != 2 ||
        std::memcmp(view.target_text(1).data, text.data() + 2, 6) != 0) {
        return 1;
    }

    auto invalid_storage = storage;
    auto& invalid = invalid_storage.bytes;
    auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(invalid.data());
    header->coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 2;
    }
    header->scene_epoch = 7;
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(invalid.data() + sizeof(SaccadeTargetPacketHeader));
    targets[0].target_id = 8;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_OK) {
        return 3;
    }
    targets[0].safe_x_q8 = targets[0].x_q8 + targets[0].width_q8;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 4;
    }

    invalid_storage = storage;
    header = reinterpret_cast<SaccadeTargetPacketHeader*>(invalid.data());
    targets = reinterpret_cast<SaccadeTargetRecord*>(invalid.data() + sizeof(SaccadeTargetPacketHeader));
    targets[1].text.offset = 3;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 5;
    }
    targets[1].text.offset = 2;
    invalid[invalid.size() - 1] = 0;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 6;
    }
    invalid[invalid.size() - 1] = '!';
    targets[0].flags = SACCADE_TARGET_SECURE;
    targets[0].capability_bits = 0;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 7;
    }

    invalid_storage = storage;
    header = reinterpret_cast<SaccadeTargetPacketHeader*>(invalid.data());
    header->target_stride = sizeof(SaccadeTargetRecord) + 8U;
    if (saccade::scene::validate_packet({invalid.data(), invalid.size()}, &view) != SACCADE_ERROR_INVALID_ARGUMENT ||
        saccade::scene::validate_packet({bytes.data() + 1, bytes.size() - 1}, &view) !=
            SACCADE_ERROR_INVALID_ARGUMENT ||
        saccade::scene::validate_packet({bytes.data(), bytes.size()}, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 8;
    }
    return 0;
}

#ifndef SACCADE_SCENE_PACKET_HPP
#define SACCADE_SCENE_PACKET_HPP

#include <saccade/saccade_backend.h>
#include <saccade/saccade_scene.h>

#include <cstddef>
#include <cstdint>

namespace saccade::scene {

struct PacketView {
    const SaccadeTargetPacketHeader* header = nullptr;
    const SaccadeTargetRecord* targets = nullptr;
    size_t byte_size = 0;
    const uint8_t* text = nullptr;
    uint32_t text_size = 0;

    [[nodiscard]] SaccadeSpanU8 target_text(uint32_t index) const noexcept {
        const SaccadeTargetTextRef ref = targets[index].text;
        return ref.size == 0 ? SaccadeSpanU8{} : SaccadeSpanU8{text + ref.offset, ref.size};
    }
};

bool valid_utf8(SaccadeSpanU8) noexcept;
SaccadeResult validate_packet(SaccadeSpanU8, PacketView*) noexcept;

} // namespace saccade::scene

#endif

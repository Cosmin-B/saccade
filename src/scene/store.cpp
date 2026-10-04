#include "scene/store.hpp"

#include <bit>
#include <cstring>

namespace saccade::scene {

SaccadeResult SceneStore::initialize(SceneStoreStorage* storage) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    if (storage == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    storage_ = storage;
    for (ScenePacketSlot& slot : storage_->slots) {
        slot.byte_size = 0;
    }
    free_mask_ = UINT32_C(0x7);
    initialized_ = true;
    return SACCADE_OK;
}

void SceneStore::drain_returned() noexcept {
    const uint32_t slot = returned_.slot.exchange(0, std::memory_order_acq_rel);
    if (slot != 0) {
        free_mask_ |= UINT32_C(1) << (slot - 1U);
    }
}

SaccadeResult SceneStore::begin_write(MutableScenePacket* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (writing_slot_ != 0) {
        ++producer_stats_.producer_busy;
        return SACCADE_ERROR_BUSY;
    }
    drain_returned();
    if (free_mask_ == 0) {
        ++producer_stats_.producer_busy;
        return SACCADE_ERROR_BUSY;
    }
    const uint32_t slot_index = static_cast<uint32_t>(std::countr_zero(free_mask_));
    writing_slot_ = slot_index + 1U;
    free_mask_ &= ~(UINT32_C(1) << slot_index);
    ++producer_stats_.write_begins;
    *output = {storage_->slots[slot_index].bytes.data(), target_packet_max_bytes, writing_slot_, 0};
    return SACCADE_OK;
}

bool SceneStore::owns_write(const MutableScenePacket& packet) const noexcept {
    return initialized_ && writing_slot_ != 0 && packet.slot == writing_slot_ &&
           packet.data == storage_->slots[writing_slot_ - 1U].bytes.data() &&
           packet.capacity == target_packet_max_bytes && packet.reserved == 0;
}

SaccadeResult SceneStore::abort_write(const MutableScenePacket& packet) noexcept {
    if (!owns_write(packet)) {
        return SACCADE_ERROR_STATE;
    }
    free_mask_ |= UINT32_C(1) << (writing_slot_ - 1U);
    writing_slot_ = 0;
    return SACCADE_OK;
}

SaccadeResult SceneStore::commit(const MutableScenePacket& packet, size_t byte_size, bool check_packet) noexcept {
    if (!owns_write(packet) || byte_size < sizeof(SaccadeTargetPacketHeader) || byte_size > target_packet_max_bytes) {
        return SACCADE_ERROR_STATE;
    }

    ScenePacketSlot& slot = storage_->slots[writing_slot_ - 1U];
    if (check_packet) {
        PacketView view{};
        if (validate_packet({slot.bytes.data(), byte_size}, &view) != SACCADE_OK ||
            view.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
    }

    slot.byte_size = byte_size;
    const uint32_t replaced_slot = pending_.slot.exchange(writing_slot_, std::memory_order_acq_rel);
    if (replaced_slot != 0) {
        free_mask_ |= UINT32_C(1) << (replaced_slot - 1U);
        ++producer_stats_.replaced;
    }
    writing_slot_ = 0;
    ++producer_stats_.published;
    return SACCADE_OK;
}

SaccadeResult SceneStore::commit_checked(const MutableScenePacket& packet, size_t byte_size) noexcept {
    return commit(packet, byte_size, true);
}

SaccadeResult SceneStore::commit_trusted(const MutableScenePacket& packet, size_t byte_size) noexcept {
    return commit(packet, byte_size, false);
}

SaccadeResult SceneStore::publish_copy(SaccadeSpanU8 bytes) noexcept {
    if (bytes.data == nullptr || bytes.size > target_packet_max_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    MutableScenePacket packet{};
    const SaccadeResult begun = begin_write(&packet);
    if (begun != SACCADE_OK) {
        return begun;
    }
    std::memcpy(packet.data, bytes.data, bytes.size);
    producer_stats_.copied_bytes += bytes.size;
    const SaccadeResult committed = commit_checked(packet, bytes.size);
    if (committed != SACCADE_OK) {
        (void)abort_write(packet);
    }
    return committed;
}

SaccadeResult SceneStore::acquire_latest(PacketView* output) noexcept {
    if (!initialized_ || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (retired_slot_ != 0) {
        if (returned_.slot.load(std::memory_order_acquire) != 0) {
            return SACCADE_ERROR_STATE;
        }
        returned_.slot.store(retired_slot_, std::memory_order_release);
        retired_slot_ = 0;
        ++consumer_stats_.returned;
    }
    const uint32_t latest_slot = pending_.slot.exchange(0, std::memory_order_acq_rel);
    if (latest_slot != 0) {
        retired_slot_ = active_slot_;
        active_slot_ = latest_slot;
        ++consumer_stats_.acquired;
    } else if (active_slot_ != 0) {
        ++consumer_stats_.reused_active;
    }
    if (active_slot_ == 0) {
        *output = {};
        return SACCADE_ERROR_NOT_FOUND;
    }
    const ScenePacketSlot& slot = storage_->slots[active_slot_ - 1U];
    const auto* header = reinterpret_cast<const SaccadeTargetPacketHeader*>(slot.bytes.data());
    const auto* targets = reinterpret_cast<const SaccadeTargetRecord*>(slot.bytes.data() + header->targets_offset);
    const size_t text_offset =
        header->targets_offset + static_cast<size_t>(header->target_count) * header->target_stride;
    *output = {header, targets, slot.byte_size, slot.bytes.data() + text_offset,
               static_cast<uint32_t>(header->total_size - text_offset)};
    return SACCADE_OK;
}

SceneStoreStats SceneStore::stats() const noexcept {
    SceneStoreStats result = producer_stats_;
    result.acquired = consumer_stats_.acquired;
    result.reused_active = consumer_stats_.reused_active;
    result.returned = consumer_stats_.returned;
    return result;
}

} // namespace saccade::scene

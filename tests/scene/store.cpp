#include "scene/store.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

enum class ExitCode : int {
    success = 0,
    initialize = 1,
    acquire_empty = 2,
    begin_write = 3,
    commit = 4,
    concurrent_access = 5,
    write_identity = 6,
    text_view = 7,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

void write_scene(const saccade::scene::MutableScenePacket& packet, uint64_t epoch) noexcept {
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = 1;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header.scene_epoch = epoch;
    header.frame_id = epoch;
    header.model_epoch = 1;
    header.session_epoch = 1;
    header.transform_epoch = 1;
    header.topology_epoch = 1;
    header.source_id = 1;
    header.targets_offset = sizeof(header);
    header.total_size = sizeof(header) + sizeof(SaccadeTargetRecord);
    SaccadeTargetRecord target{};
    target.target_id = epoch;
    target.width_q8 = 256;
    target.height_q8 = 256;
    target.safe_x_q8 = 128;
    target.safe_y_q8 = 128;
    target.confidence_q16 = UINT16_MAX;
    target.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
    target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    target.flags = SACCADE_TARGET_ACTIONABLE;
    std::memcpy(packet.data, &header, sizeof(header));
    std::memcpy(packet.data + sizeof(header), &target, sizeof(target));
}

bool preserves_text_view() noexcept {
    static saccade::scene::SceneStoreStorage storage;
    saccade::scene::SceneStore store;
    saccade::scene::MutableScenePacket packet{};
    if (store.initialize(&storage) != SACCADE_OK || store.begin_write(&packet) != SACCADE_OK) {
        return false;
    }
    write_scene(packet, 1);
    auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(packet.data);
    auto* target = reinterpret_cast<SaccadeTargetRecord*>(packet.data + sizeof(*header));
    constexpr std::array<uint8_t, 4> text{'O', 'p', 'e', 'n'};
    target->text = {0, static_cast<uint32_t>(text.size())};
    std::memcpy(packet.data + header->total_size, text.data(), text.size());
    header->total_size += text.size();
    if (store.commit_checked(packet, static_cast<size_t>(header->total_size)) != SACCADE_OK) {
        return false;
    }

    saccade::scene::PacketView view{};
    for (uint32_t acquisition = 0; acquisition < 2; ++acquisition) {
        if (store.acquire_latest(&view) != SACCADE_OK || view.text == nullptr || view.text_size != text.size()) {
            return false;
        }
        const SaccadeSpanU8 target_text = view.target_text(0);
        if (target_text.size != text.size() || std::memcmp(target_text.data, text.data(), text.size()) != 0) {
            return false;
        }
    }

    if (store.begin_write(&packet) != SACCADE_OK) {
        return false;
    }
    write_scene(packet, 2);
    constexpr size_t one_target_size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord);
    return store.commit_checked(packet, one_target_size) == SACCADE_OK && store.acquire_latest(&view) == SACCADE_OK &&
           view.header->scene_epoch == 2 && view.text_size == 0 && view.target_text(0).size == 0;
}

bool rejects_foreign_write() noexcept {
    static saccade::scene::SceneStoreStorage storage;
    saccade::scene::SceneStore store;
    saccade::scene::MutableScenePacket packet{};
    if (store.initialize(&storage) != SACCADE_OK || store.begin_write(&packet) != SACCADE_OK) {
        return false;
    }
    write_scene(packet, 1);
    constexpr size_t one_target_size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord);
    alignas(SaccadeTargetPacketHeader) std::array<uint8_t, one_target_size> foreign_bytes{};
    std::memcpy(foreign_bytes.data(), packet.data, one_target_size);
    // A valid foreign buffer must not authorize publication of malformed slot bytes.
    reinterpret_cast<SaccadeTargetPacketHeader*>(packet.data)->struct_size = 0;
    if (store.commit_checked(packet, one_target_size) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return false;
    }
    for (uint32_t alteration = 0; alteration < 5; ++alteration) {
        saccade::scene::MutableScenePacket foreign = packet;
        switch (alteration) {
        case 0:
            foreign.data = foreign_bytes.data();
            break;
        case 1:
            foreign.data = nullptr;
            break;
        case 2:
            --foreign.capacity;
            break;
        case 3:
            foreign.reserved = 1;
            break;
        case 4:
            foreign.slot = UINT32_MAX;
            break;
        }
        if (store.commit_checked(foreign, one_target_size) != SACCADE_ERROR_STATE ||
            store.commit_trusted(foreign, one_target_size) != SACCADE_ERROR_STATE ||
            store.abort_write(foreign) != SACCADE_ERROR_STATE) {
            return false;
        }
        saccade::scene::PacketView view{};
        if (store.acquire_latest(&view) != SACCADE_ERROR_NOT_FOUND || store.stats().published != 0) {
            return false;
        }
    }
    if (store.abort_write(packet) != SACCADE_OK || store.begin_write(&packet) != SACCADE_OK) {
        return false;
    }
    write_scene(packet, 2);
    saccade::scene::PacketView view{};
    return store.commit_checked(packet, one_target_size) == SACCADE_OK && store.acquire_latest(&view) == SACCADE_OK &&
           view.header->scene_epoch == 2;
}

} // namespace

int main() {
    if (!preserves_text_view()) {
        return to_process_exit_code(ExitCode::text_view);
    }
    if (!rejects_foreign_write()) {
        return to_process_exit_code(ExitCode::write_identity);
    }
    static saccade::scene::SceneStoreStorage storage;
    saccade::scene::SceneStore store;
    if (store.initialize(nullptr) != SACCADE_ERROR_INVALID_ARGUMENT || store.initialize(&storage) != SACCADE_OK ||
        store.initialize(&storage) != SACCADE_ERROR_ALREADY_EXISTS) {
        return to_process_exit_code(ExitCode::initialize);
    }
    saccade::scene::PacketView view{};
    if (store.acquire_latest(&view) != SACCADE_ERROR_NOT_FOUND) {
        return to_process_exit_code(ExitCode::acquire_empty);
    }
    saccade::scene::MutableScenePacket packet{};
    if (store.begin_write(&packet) != SACCADE_OK) {
        return to_process_exit_code(ExitCode::begin_write);
    }
    write_scene(packet, 1);
    constexpr size_t one_target_size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord);
    if (store.commit_checked(packet, one_target_size) != SACCADE_OK || store.acquire_latest(&view) != SACCADE_OK ||
        view.header->scene_epoch != 1 || view.targets[0].target_id != 1 || store.acquire_latest(&view) != SACCADE_OK ||
        view.header->scene_epoch != 1) {
        return to_process_exit_code(ExitCode::commit);
    }

    constexpr uint64_t iteration_count = 10000;
    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> observed_epoch{0};
    std::atomic<size_t> hot_allocations{SIZE_MAX};
    std::thread producer([&]() noexcept {
        saccade::test::begin_allocation_tracking();
        for (uint64_t epoch = 2; epoch <= iteration_count; ++epoch) {
            saccade::scene::MutableScenePacket output{};
            while (store.begin_write(&output) == SACCADE_ERROR_BUSY) {
                std::this_thread::yield();
            }
            write_scene(output, epoch);
            if (store.commit_trusted(output, one_target_size) != SACCADE_OK) {
                producer_done.store(true, std::memory_order_release);
                return;
            }
        }
        hot_allocations.store(saccade::test::end_allocation_tracking(), std::memory_order_relaxed);
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&]() noexcept {
        uint64_t last = 1;
        while (!producer_done.load(std::memory_order_acquire) || last != iteration_count) {
            saccade::scene::PacketView current{};
            if (store.acquire_latest(&current) == SACCADE_OK) {
                if (current.header->scene_epoch < last) {
                    observed_epoch.store(UINT64_MAX, std::memory_order_relaxed);
                    return;
                }
                last = current.header->scene_epoch;
                observed_epoch.store(last, std::memory_order_relaxed);
            }
        }
    });
    producer.join();
    consumer.join();
    const auto stats = store.stats();
    if (observed_epoch.load(std::memory_order_relaxed) != iteration_count || stats.published != iteration_count ||
        stats.published != stats.acquired + stats.replaced || hot_allocations.load(std::memory_order_relaxed) != 0) {
        return to_process_exit_code(ExitCode::concurrent_access);
    }
    return to_process_exit_code(ExitCode::success);
}

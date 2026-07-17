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

} // namespace

int main() {
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

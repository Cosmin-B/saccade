#include "backends/reference_cpu/reference_cpu.hpp"
#include "scheduler/desktop_neural_coordinator.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstdint>

namespace {

template <typename T> T output_structure() noexcept {
    T value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    return value;
}

saccade::geometry::CoordinateTransform transform(uint64_t source_id, int32_t x_q8) noexcept {
    saccade::geometry::CoordinateTransform value;
    saccade::geometry::TransformDesc desc{};
    desc.source = {0, 0, 256, 256};
    desc.destination = {x_q8, 0, 256, 256};
    desc.epoch = source_id;
    desc.source_space = saccade::geometry::CoordinateSpace::capture;
    desc.destination_space = saccade::geometry::CoordinateSpace::desktop;
    (void)value.initialize(desc);
    return value;
}

SaccadeFrameHandle import_frame(SaccadeRuntimeHandle runtime, const std::array<uint8_t, 4>& pixel, uint64_t frame_id,
                                uint64_t transform_epoch) noexcept {
    SaccadeHostFrameDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.data = {pixel.data(), pixel.size()};
    desc.width = 1;
    desc.height = 1;
    desc.row_stride_bytes = 4;
    desc.pixel_format = SACCADE_FORMAT_BGRA8;
    desc.frame_id = frame_id;
    desc.transform_epoch = transform_epoch;
    SaccadeFrameHandle frame = 0;
    return saccade_frame_import(runtime, &desc, &frame) == SACCADE_OK ? frame : 0;
}

struct Retirement {
    uint32_t count = 0;
};

void retire(void* context, SaccadeFrameHandle) noexcept {
    ++static_cast<Retirement*>(context)->count;
}

saccade::scheduler::DesktopNeuralFrame desktop_frame(SaccadeFrameHandle frame, uint64_t frame_id, uint64_t source_id,
                                                     int32_t x_q8, Retirement* retirement) noexcept {
    saccade::scheduler::DesktopNeuralFrame value{};
    value.frame = frame;
    value.source_id = source_id;
    value.topology_epoch = 20;
    value.transform_epoch = source_id;
    value.capture_time_ns = 1;
    value.scene_transform_epoch = 30;
    value.source_count = 2;
    value.width = 1;
    value.height = 1;
    value.source_to_desktop = transform(source_id, x_q8);
    value.retire_context = retirement;
    value.retire = retire;
    (void)frame_id;
    return value;
}

} // namespace

int main() {
    const std::array<uint8_t, 4> white{255, 255, 255, 255};
    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK) return 1;
    saccade::backend::reference_cpu::Backend backend;
    const SaccadeInferenceProviderDesc provider = backend.provider();
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_OK)
        return 2;
    const auto model = saccade::backend::reference_cpu::encode_model({200, 1});
    SaccadeInferenceSessionDesc session_desc{};
    session_desc.struct_size = sizeof(session_desc);
    session_desc.api_version = SACCADE_API_VERSION;
    session_desc.model_bytes = {model.data(), model.size()};
    session_desc.model_stable_id = 300;
    session_desc.provider_stable_id = provider.info.stable_id;
    session_desc.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT;
    session_desc.required_format_bits = SACCADE_FORMAT_BGRA8;
    session_desc.required_precision_bits = SACCADE_PRECISION_FP32;
    session_desc.required_import_bits = SACCADE_IMPORT_HOST;
    session_desc.queue_capacity = 1;
    session_desc.max_in_flight = 1;
    SaccadeInferenceSessionInfo session_info = output_structure<SaccadeInferenceSessionInfo>();
    SaccadeExecutionContextHandle session = 0;
    if (saccade_inference_session_create(runtime, &session_desc, &session, &session_info) != SACCADE_OK) return 3;

    static saccade::scene::SceneStoreStorage scene_storage;
    static saccade::scheduler::DesktopNeuralCoordinatorStorage coordinator_storage;
    saccade::scene::SceneStore scenes;
    saccade::scheduler::DesktopNeuralCoordinator coordinator;
    saccade::scheduler::DesktopNeuralCoordinatorConfig config{};
    config.runtime = runtime;
    config.session = session;
    config.model_epoch = 400;
    config.session_epoch = 500;
    config.desktop_source_id = 600;
    config.maximum_output_bytes = session_info.max_output_bytes;
    config.maximum_targets = 16;
    config.rates.scene_period_ns = 1;
    if (scenes.initialize(&scene_storage) != SACCADE_OK ||
        coordinator.initialize(config, &coordinator_storage, &scenes) != SACCADE_OK)
        return 4;

    Retirement retirement{};
    const SaccadeFrameHandle first = import_frame(runtime, white, 1, 100);
    const SaccadeFrameHandle second = import_frame(runtime, white, 2, 200);
    saccade::test::begin_allocation_tracking();
    if (first == 0 || second == 0 || coordinator.offer(desktop_frame(first, 1, 100, 0, &retirement)) != SACCADE_OK ||
        coordinator.offer(desktop_frame(second, 2, 200, 256, &retirement)) != SACCADE_OK)
        return 5;
    saccade::scheduler::DesktopNeuralAdvance advance{};
    if (coordinator.advance(0, &advance) != SACCADE_OK || !advance.interaction_due || advance.scene_published ||
        coordinator.advance(1, &advance) != SACCADE_OK || advance.scene_published || advance.sources_completed != 1 ||
        coordinator.advance(2, &advance) != SACCADE_OK || !advance.scene_published || !advance.scope_complete ||
        advance.sources_expected != 2 || advance.sources_completed != 1 || advance.target_count != 2 ||
        advance.batch_latency_ns != 2 || advance.full_scope_latency_ns != 1 || retirement.count != 2)
        return 6;
    saccade::scene::PacketView scene{};
    if (scenes.acquire_latest(&scene) != SACCADE_OK || scene.header->scene_epoch != 1 || scene.header->frame_id != 2 ||
        scene.header->transform_epoch != 30 || scene.header->topology_epoch != 20 || scene.header->target_count != 2 ||
        scene.targets[0].x_q8 != 0 || scene.targets[1].x_q8 != 256)
        return 7;
    const auto stats = coordinator.stats();
    if (stats.frames_offered != 2 || stats.batches_started != 1 || stats.batches_published != 1 ||
        stats.sources_submitted != 2 || stats.sources_completed != 2 || stats.targets_published != 2 ||
        stats.batch_latency_total_ns != 2 || stats.batch_latency_max_ns != 2 || stats.batch_deadlines_missed != 1 ||
        stats.full_scope_latency_total_ns != 1 || stats.full_scope_latency_max_ns != 1 ||
        stats.full_scope_deadlines_missed != 0 || stats.failures != 0 || coordinator.shutdown() != SACCADE_OK ||
        saccade::test::end_allocation_tracking() != 0)
        return 8;

    static saccade::scene::SceneStoreStorage missing_scene_storage;
    static saccade::scheduler::DesktopNeuralCoordinatorStorage missing_coordinator_storage;
    saccade::scene::SceneStore missing_scenes;
    saccade::scheduler::DesktopNeuralCoordinator missing_coordinator;
    Retirement missing_retirement{};
    const SaccadeFrameHandle missing = import_frame(runtime, white, 3, 100);
    if (missing_scenes.initialize(&missing_scene_storage) != SACCADE_OK ||
        missing_coordinator.initialize(config, &missing_coordinator_storage, &missing_scenes) != SACCADE_OK ||
        missing == 0 ||
        missing_coordinator.offer(desktop_frame(missing, 3, 100, 0, &missing_retirement)) != SACCADE_OK ||
        missing_coordinator.advance(0, &advance) != SACCADE_OK || advance.scene_published ||
        advance.sources_expected != 2 || missing_scenes.acquire_latest(&scene) != SACCADE_ERROR_NOT_FOUND ||
        missing_coordinator.stats().batches_incomplete != 1 || missing_coordinator.shutdown() != SACCADE_OK ||
        missing_retirement.count != 1) {
        return 10;
    }

    static saccade::scene::SceneStoreStorage failed_scene_storage;
    static saccade::scheduler::DesktopNeuralCoordinatorStorage failed_coordinator_storage;
    saccade::scene::SceneStore failed_scenes;
    saccade::scheduler::DesktopNeuralCoordinator failed_coordinator;
    auto failed_config = config;
    --failed_config.maximum_output_bytes;
    Retirement failed_retirement{};
    const SaccadeFrameHandle failed_first = import_frame(runtime, white, 4, 100);
    const SaccadeFrameHandle failed_second = import_frame(runtime, white, 5, 200);
    if (failed_scenes.initialize(&failed_scene_storage) != SACCADE_OK ||
        failed_coordinator.initialize(failed_config, &failed_coordinator_storage, &failed_scenes) != SACCADE_OK ||
        failed_first == 0 || failed_second == 0 ||
        failed_coordinator.offer(desktop_frame(failed_first, 4, 100, 0, &failed_retirement)) != SACCADE_OK ||
        failed_coordinator.offer(desktop_frame(failed_second, 5, 200, 256, &failed_retirement)) != SACCADE_OK ||
        failed_coordinator.advance(0, &advance) != SACCADE_OK || advance.scene_published ||
        advance.sources_expected != 2 || advance.sources_failed != 2 ||
        failed_scenes.acquire_latest(&scene) != SACCADE_ERROR_NOT_FOUND ||
        failed_coordinator.stats().batches_incomplete != 1 || failed_coordinator.stats().sources_failed != 2 ||
        failed_coordinator.shutdown() != SACCADE_OK || failed_retirement.count != 2) {
        return 11;
    }
    if (saccade_inference_session_destroy(runtime, session) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK)
        return 9;
    return 0;
}

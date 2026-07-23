#include "backends/reference_cpu/reference_cpu.hpp"
#include "scheduler/neural_coordinator.hpp"
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

saccade::geometry::CoordinateTransform transform(uint64_t epoch) noexcept {
    saccade::geometry::CoordinateTransform value;
    saccade::geometry::TransformDesc desc{};
    desc.source = {0, 0, 256, 256};
    desc.destination = {2560, 5120, 512, 768};
    desc.epoch = epoch;
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

struct RetirementCapture {
    SaccadeFrameHandle frame = 0;
    uint32_t calls = 0;
};

void retire_frame(void* context, SaccadeFrameHandle frame) noexcept {
    auto* capture = static_cast<RetirementCapture*>(context);
    capture->frame = frame;
    ++capture->calls;
}

saccade::scheduler::NeuralFrame neural_frame(SaccadeFrameHandle frame, uint64_t epoch,
                                             RetirementCapture* retirement = nullptr) noexcept {
    saccade::scheduler::NeuralFrame value{};
    value.frame = frame;
    value.source_id = 100;
    value.topology_epoch = 200;
    value.transform_epoch = epoch;
    value.capture_time_ns = epoch + 1000;
    value.width = 1;
    value.height = 1;
    value.source_to_desktop = transform(epoch);
    value.retire_context = retirement;
    value.retire = retirement != nullptr ? retire_frame : nullptr;
    return value;
}

} // namespace

int main() {
    const std::array<uint8_t, 4> white{255, 255, 255, 255};
    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK) {
        return 1;
    }
    saccade::backend::reference_cpu::Backend backend;
    const SaccadeInferenceProviderDesc provider = backend.provider();
    if (saccade_register_inference_provider(runtime, &provider) != SACCADE_OK ||
        saccade_runtime_freeze(runtime) != SACCADE_OK) {
        return 2;
    }

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
    if (saccade_inference_session_create(runtime, &session_desc, &session, &session_info) != SACCADE_OK) {
        return 3;
    }

    static saccade::scene::SceneStoreStorage scene_storage;
    static saccade::scheduler::NeuralCoordinatorStorage coordinator_storage;
    saccade::scene::SceneStore scenes;
    saccade::scheduler::NeuralCoordinator coordinator;
    saccade::scheduler::NeuralCoordinatorConfig config{};
    config.runtime = runtime;
    config.session = session;
    config.model_epoch = 400;
    config.session_epoch = 500;
    config.maximum_output_bytes = session_info.max_output_bytes;
    if (scenes.initialize(&scene_storage) != SACCADE_OK ||
        coordinator.initialize(config, &coordinator_storage, &scenes) != SACCADE_OK) {
        return 4;
    }

    saccade::test::begin_allocation_tracking();
    const SaccadeFrameHandle first = import_frame(runtime, white, 1, 1);
    RetirementCapture retirement{};
    saccade::scheduler::NeuralAdvance advance{};
    if (first == 0 || coordinator.offer(neural_frame(first, 1, &retirement)) != SACCADE_OK ||
        coordinator.advance(0, &advance) != SACCADE_OK || !advance.interaction_due || advance.scene_published ||
        coordinator.advance(1, &advance) != SACCADE_OK || !advance.scene_published || advance.scene_epoch != 1 ||
        advance.target_count != 1 || retirement.calls != 1 || retirement.frame != first ||
        saccade_frame_release(runtime, first) != SACCADE_ERROR_STALE_HANDLE) {
        return 5;
    }
    saccade::scene::PacketView scene{};
    if (scenes.acquire_latest(&scene) != SACCADE_OK ||
        scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || scene.header->frame_id != 1 ||
        scene.header->scene_epoch != 1 || scene.header->capture_time_ns != 1001 || scene.targets[0].x_q8 != 2560 ||
        scene.targets[0].y_q8 != 5120 || scene.targets[0].width_q8 != 512 || scene.targets[0].height_q8 != 768) {
        return 6;
    }

    const SaccadeFrameHandle second = import_frame(runtime, white, 2, 2);
    if (second == 0 || coordinator.offer(neural_frame(second, 2)) != SACCADE_OK ||
        coordinator.advance(saccade::scheduler::scene_period_30hz_ns, &advance) != SACCADE_OK) {
        return 7;
    }
    const SaccadeFrameHandle third = import_frame(runtime, white, 3, 3);
    const SaccadeFrameHandle fourth = import_frame(runtime, white, 4, 4);
    if (third == 0 || fourth == 0 || coordinator.offer(neural_frame(third, 3)) != SACCADE_OK ||
        coordinator.offer(neural_frame(fourth, 4)) != SACCADE_OK ||
        saccade_frame_release(runtime, third) != SACCADE_ERROR_STALE_HANDLE ||
        coordinator.advance(saccade::scheduler::scene_period_30hz_ns + 1, &advance) != SACCADE_OK ||
        !advance.scene_published || advance.scene_epoch != 2 ||
        coordinator.advance(saccade::scheduler::scene_period_30hz_ns * 2, &advance) != SACCADE_OK ||
        coordinator.advance(saccade::scheduler::scene_period_30hz_ns * 2 + 1, &advance) != SACCADE_OK ||
        !advance.scene_published || advance.scene_epoch != 3) {
        return 8;
    }
    if (scenes.acquire_latest(&scene) != SACCADE_OK || scene.header->frame_id != 4 || scene.header->scene_epoch != 3 ||
        scene.header->capture_time_ns != 1004) {
        return 9;
    }
    const auto stats = coordinator.stats();
    if (stats.frames_offered != 4 || stats.frames_replaced != 1 || stats.frames_submitted != 3 ||
        stats.tickets_completed != 3 || stats.scenes_published != 3 || stats.targets_published != 3 ||
        stats.failures != 0 || coordinator.shutdown() != SACCADE_OK || saccade::test::end_allocation_tracking() != 0) {
        return 10;
    }
    if (saccade_inference_session_destroy(runtime, session) != SACCADE_OK ||
        saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return 11;
    }
    return 0;
}

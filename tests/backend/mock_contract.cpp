#include "backend/registry.hpp"
#include "backends/callback_guard.hpp"
#include "backends/mock/mock_backend.hpp"
#include "scene/packet.hpp"
#include "tests/support/input_plan.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

SaccadeResult SACCADE_CALL successful_callback(void*) {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL throwing_callback(void*) {
    throw 1;
}

template <typename Structure> Structure output_structure() {
    Structure value{};
    value.struct_size = static_cast<uint32_t>(sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    return value;
}

SaccadeInferenceDispatchDesc inference_dispatch(uint64_t frame_id, uint32_t width, uint32_t height) noexcept {
    static constexpr uint8_t pixel = 0;
    SaccadeInferenceDispatchDesc value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.frame.struct_size = sizeof(value.frame);
    value.frame.api_version = SACCADE_API_VERSION;
    value.frame.storage = SACCADE_FRAME_STORAGE_HOST;
    value.frame.pixel_format = SACCADE_FORMAT_R8;
    value.frame.host_data = {&pixel, 1};
    value.frame.width = width;
    value.frame.height = height;
    value.frame.row_stride_bytes = width;
    value.frame.frame_id = frame_id;
    value.frame.transform_epoch = 1;
    value.scope = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    value.output_capacity = 32;
    value.model_epoch = 1;
    value.session_epoch = 1;
    value.transform_epoch = 1;
    value.topology_epoch = 1;
    value.source_id = 1;
    return value;
}

template <class Record, size_t Size>
void store_record(std::array<uint8_t, Size>* bytes, size_t offset, const Record& record) noexcept {
    std::memcpy(bytes->data() + offset, &record, sizeof(record));
}

constexpr size_t overlay_packet_size =
    sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);
using OverlayPacket = std::array<uint8_t, overlay_packet_size>;

OverlayPacket overlay_packet(uint64_t scene_epoch, uint64_t transform_epoch) noexcept {
    OverlayPacket bytes{};
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = 1;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene_epoch;
    header.transform_epoch = transform_epoch;
    header.targets_offset = sizeof(SaccadeOverlayPacketHeader);
    header.styles_offset = sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayTarget);

    SaccadeOverlayTarget target{};
    target.target_id = 1;
    target.x_q3 = 80;
    target.y_q3 = 80;
    target.width_q3 = 160;
    target.height_q3 = 80;
    target.label_x_q3 = 80;
    target.label_y_q3 = 40;
    target.confidence_q16 = UINT16_MAX;
    target.glyphs[0] = 1;
    target.glyphs[1] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
    target.glyph_count = 1;

    SaccadeOverlayStyle style{};
    style.target_stroke_q3 = 8;
    style.label_height_q3 = 80;
    style.label_padding_x_q3 = 8;
    style.glyph_width_q3 = 40;
    style.glyph_height_q3 = 56;
    style.glyph_advance_q3 = 48;
    style.active_stroke_q3 = 8;

    store_record(&bytes, 0, header);
    store_record(&bytes, static_cast<size_t>(header.targets_offset), target);
    store_record(&bytes, static_cast<size_t>(header.styles_offset), style);
    return bytes;
}

uint64_t read_u64_le(const uint8_t* bytes) {
    uint64_t value = 0;
    for (uint32_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

bool output_prefixes_are_bounded(SaccadeEnumerateDevicesFn enumerate, void* context) {
    constexpr size_t prefix_size = offsetof(SaccadeDeviceInfo, reserved);
    alignas(SaccadeDeviceInfo) std::array<uint8_t, prefix_size + 16> short_output{};
    short_output.fill(UINT8_C(0xA5));
    const uint32_t short_size = static_cast<uint32_t>(prefix_size);
    const uint32_t api_version = SACCADE_API_VERSION;
    std::memcpy(short_output.data(), &short_size, sizeof(short_size));
    std::memcpy(short_output.data() + offsetof(SaccadeDeviceInfo, api_version), &api_version, sizeof(api_version));
    if (enumerate(context, 0, reinterpret_cast<SaccadeDeviceInfo*>(short_output.data())) != SACCADE_OK) {
        return false;
    }
    uint32_t returned_size = 0;
    std::memcpy(&returned_size, short_output.data(), sizeof(returned_size));
    if (returned_size != short_size) {
        return false;
    }
    for (size_t index = prefix_size; index < short_output.size(); ++index) {
        if (short_output[index] != UINT8_C(0xA5)) {
            return false;
        }
    }

    struct ExtendedDeviceInfo {
        SaccadeDeviceInfo current;
        std::array<uint64_t, 2> future;
    };

    ExtendedDeviceInfo extended{};
    extended.current.struct_size = static_cast<uint32_t>(sizeof(extended));
    extended.current.api_version = SACCADE_API_VERSION;
    extended.future.fill(UINT64_C(0xA5A5A5A5A5A5A5A5));
    if (enumerate(context, 0, &extended.current) != SACCADE_OK ||
        extended.current.struct_size != sizeof(SaccadeDeviceInfo) ||
        extended.future[0] != UINT64_C(0xA5A5A5A5A5A5A5A5) || extended.future[1] != UINT64_C(0xA5A5A5A5A5A5A5A5)) {
        return false;
    }
    return true;
}

bool max_in_flight_is_enforced() {
    using saccade::backend::mock::Backend;
    using saccade::backend::mock::Config;

    Config config{};
    config.completion_polls = 2;
    config.queue_capacity = 2;
    Backend backend(config);
    SaccadeInferenceProviderDesc provider = backend.inference_provider();
    const SaccadeDeviceInfo device = backend.device_info();

    const uint8_t model_bytes[] = {1};
    SaccadeModelDesc model_desc{};
    model_desc.struct_size = static_cast<uint32_t>(sizeof(model_desc));
    model_desc.api_version = SACCADE_API_VERSION;
    model_desc.bytes = {model_bytes, sizeof(model_bytes)};
    model_desc.stable_id = 1;
    model_desc.device_id = device.stable_id;
    SaccadeModelHandle model = 0;
    if (provider.ops.create_model(provider.context, &model_desc, &model) != SACCADE_OK) {
        return false;
    }

    SaccadeExecutionContextDesc context_desc{};
    context_desc.struct_size = static_cast<uint32_t>(sizeof(context_desc));
    context_desc.api_version = SACCADE_API_VERSION;
    context_desc.model = model;
    context_desc.device_id = device.stable_id;
    context_desc.queue_capacity = 2;
    context_desc.max_in_flight = 1;
    SaccadeExecutionContextHandle execution = 0;
    if (provider.ops.create_context(provider.context, &context_desc, &execution) != SACCADE_OK) {
        return false;
    }

    SaccadeInferenceDispatchDesc submit = inference_dispatch(1, 1, 1);
    SaccadeTicketHandle first = 0;
    SaccadeTicketHandle second = 0;
    if (provider.ops.submit(provider.context, execution, &submit, &first) != SACCADE_OK ||
        provider.ops.submit(provider.context, execution, &submit, &second) != SACCADE_OK) {
        return false;
    }

    SaccadeInferenceStatus first_status = output_structure<SaccadeInferenceStatus>();
    SaccadeInferenceStatus second_status = output_structure<SaccadeInferenceStatus>();
    if (provider.ops.poll(provider.context, execution, first, &first_status) != SACCADE_OK ||
        first_status.state != SACCADE_TICKET_RUNNING ||
        provider.ops.poll(provider.context, execution, second, &second_status) != SACCADE_OK ||
        second_status.state != SACCADE_TICKET_QUEUED ||
        provider.ops.poll(provider.context, execution, first, &first_status) != SACCADE_OK ||
        first_status.state != SACCADE_TICKET_COMPLETE ||
        provider.ops.poll(provider.context, execution, second, &second_status) != SACCADE_OK ||
        second_status.state != SACCADE_TICKET_RUNNING ||
        provider.ops.cancel(provider.context, execution, second) != SACCADE_OK) {
        return false;
    }

    std::array<uint8_t, 32> output{};
    size_t required = 0;
    if (provider.ops.collect(provider.context, execution, first, {output.data(), output.size()}, &required) !=
            SACCADE_OK ||
        provider.ops.collect(provider.context, execution, second, {output.data(), output.size()}, &required) !=
            SACCADE_ERROR_CANCELLED) {
        return false;
    }

    SaccadeTicketHandle reset_ticket = 0;
    if (provider.ops.submit(provider.context, execution, &submit, &reset_ticket) != SACCADE_OK ||
        provider.ops.poll(provider.context, execution, reset_ticket, &first_status) != SACCADE_OK ||
        first_status.state != SACCADE_TICKET_RUNNING || provider.ops.reset(provider.context, execution) != SACCADE_OK ||
        provider.ops.submit(provider.context, execution, &submit, &reset_ticket) != SACCADE_OK ||
        provider.ops.poll(provider.context, execution, reset_ticket, &first_status) != SACCADE_OK ||
        first_status.state != SACCADE_TICKET_RUNNING ||
        provider.ops.cancel(provider.context, execution, reset_ticket) != SACCADE_OK ||
        provider.ops.collect(provider.context, execution, reset_ticket, {output.data(), output.size()}, &required) !=
            SACCADE_ERROR_CANCELLED) {
        return false;
    }

    SaccadeTicketHandle queued = 0;
    SaccadeTicketHandle running = 0;
    if (provider.ops.submit(provider.context, execution, &submit, &queued) != SACCADE_OK ||
        provider.ops.submit(provider.context, execution, &submit, &running) != SACCADE_OK ||
        provider.ops.poll(provider.context, execution, running, &second_status) != SACCADE_OK ||
        second_status.state != SACCADE_TICKET_RUNNING ||
        provider.ops.synchronize(provider.context, execution, 1) != SACCADE_OK ||
        provider.ops.poll(provider.context, execution, queued, &first_status) != SACCADE_OK ||
        first_status.state != SACCADE_TICKET_COMPLETE ||
        provider.ops.poll(provider.context, execution, running, &second_status) != SACCADE_OK ||
        second_status.state != SACCADE_TICKET_COMPLETE ||
        provider.ops.collect(provider.context, execution, queued, {output.data(), output.size()}, &required) !=
            SACCADE_OK ||
        provider.ops.collect(provider.context, execution, running, {output.data(), output.size()}, &required) !=
            SACCADE_OK ||
        provider.ops.destroy_context(provider.context, execution) != SACCADE_OK ||
        provider.ops.destroy_model(provider.context, model) != SACCADE_OK) {
        return false;
    }
    return true;
}

bool input_tickets_are_reclaimed() {
    using saccade::backend::mock::Backend;
    using saccade::backend::mock::Config;

    Config config{};
    config.completion_polls = 1;
    config.queue_capacity = 1;
    Backend backend(config);
    SaccadeInputProviderDesc provider = backend.input_provider();
    const auto fixture = saccade::test::input_plan(1);
    const SaccadeInputPlanDesc plan = saccade::test::input_plan_desc(fixture);
    SaccadeInputStatus status = output_structure<SaccadeInputStatus>();

    for (uint32_t index = 0; index < 12; ++index) {
        SaccadeTicketHandle ticket = 0;
        if (provider.ops.execute(provider.context, &plan, &ticket) != SACCADE_OK ||
            provider.ops.wait(provider.context, ticket, 1, &status) != SACCADE_OK ||
            status.state != SACCADE_TICKET_COMPLETE ||
            provider.ops.poll(provider.context, ticket, &status) != SACCADE_ERROR_STALE_HANDLE) {
            return false;
        }
    }
    for (uint32_t index = 0; index < 12; ++index) {
        SaccadeTicketHandle ticket = 0;
        if (provider.ops.execute(provider.context, &plan, &ticket) != SACCADE_OK ||
            provider.ops.cancel(provider.context, ticket) != SACCADE_OK ||
            provider.ops.poll(provider.context, ticket, &status) != SACCADE_OK ||
            status.state != SACCADE_TICKET_CANCELLED ||
            provider.ops.poll(provider.context, ticket, &status) != SACCADE_ERROR_STALE_HANDLE) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    using saccade::backend::ProviderRegistry;
    using saccade::backend::mock::Backend;
    using saccade::backend::mock::Config;
    using saccade::backend::mock::FaultPoint;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 40;
    }
    if (saccade::backend::detail::guarded_callback<&successful_callback>(nullptr) != SACCADE_OK ||
        saccade::backend::detail::guarded_callback<&throwing_callback>(nullptr) != SACCADE_ERROR_BACKEND) {
        return 42;
    }

    Config config{};
    config.completion_polls = 2;
    config.queue_capacity = 1;
    config.memory.host_committed = 1234;
    config.memory.device_imported = 5678;
    config.memory.high_water_bytes = 9012;
    saccade::test::begin_allocation_tracking();
    Backend backend(config);

    SaccadeInferenceProviderDesc inference = backend.inference_provider();
    SaccadeCaptureProviderDesc capture = backend.capture_provider();
    SaccadeOverlayProviderDesc overlay = backend.overlay_provider();
    SaccadeAccessibilityProviderDesc accessibility = backend.accessibility_provider();
    SaccadeInputProviderDesc input = backend.input_provider();

    ProviderRegistry registry;
    SaccadeProviderHandle inference_handle = 0;
    if (registry.register_inference(&inference, &inference_handle) != SACCADE_OK ||
        registry.register_capture(&capture, nullptr) != SACCADE_OK ||
        registry.register_overlay(&overlay, nullptr) != SACCADE_OK ||
        registry.register_accessibility(&accessibility, nullptr) != SACCADE_OK ||
        registry.register_input(&input, nullptr) != SACCADE_OK) {
        return 1;
    }
    SaccadeDeviceInfo device = backend.device_info();
    if (registry.register_device(inference_handle, &device, nullptr) != SACCADE_OK) {
        return 2;
    }

    SaccadeDeviceInfo enumerated_device = output_structure<SaccadeDeviceInfo>();
    if (inference.ops.enumerate_devices(inference.context, 0, &enumerated_device) != SACCADE_OK ||
        enumerated_device.stable_id != device.stable_id ||
        inference.ops.enumerate_devices(inference.context, 1, &enumerated_device) != SACCADE_ERROR_NOT_FOUND ||
        !output_prefixes_are_bounded(inference.ops.enumerate_devices, inference.context)) {
        return 3;
    }

    const uint8_t model_bytes[] = {1, 2, 3, 4};
    SaccadeModelInfo model_info = output_structure<SaccadeModelInfo>();
    if (inference.ops.query_model(inference.context, {model_bytes, sizeof(model_bytes)}, &model_info) != SACCADE_OK ||
        model_info.max_output_bytes != 32) {
        return 4;
    }
    SaccadeModelDesc model_desc{};
    model_desc.struct_size = static_cast<uint32_t>(sizeof(model_desc));
    model_desc.api_version = SACCADE_API_VERSION;
    model_desc.bytes = {model_bytes, sizeof(model_bytes)};
    model_desc.stable_id = 77;
    model_desc.device_id = device.stable_id;
    SaccadeModelHandle model = 0;
    if (inference.ops.create_model(inference.context, &model_desc, &model) != SACCADE_OK || model == 0) {
        return 5;
    }

    SaccadeExecutionContextDesc context_desc{};
    context_desc.struct_size = static_cast<uint32_t>(sizeof(context_desc));
    context_desc.api_version = SACCADE_API_VERSION;
    context_desc.model = model;
    context_desc.device_id = device.stable_id;
    context_desc.queue_capacity = 1;
    context_desc.max_in_flight = 1;
    SaccadeExecutionContextHandle context = 0;
    if (inference.ops.create_context(inference.context, &context_desc, &context) != SACCADE_OK || context == 0) {
        return 6;
    }

    SaccadeInferenceDispatchDesc submit = inference_dispatch(42, 64, 48);
    submit.model_epoch = 7;
    submit.session_epoch = 8;
    submit.transform_epoch = 9;
    submit.frame.transform_epoch = 9;
    submit.topology_epoch = 10;
    submit.source_id = 11;
    SaccadeTicketHandle inference_ticket = 0;
    SaccadeTicketHandle extra_ticket = 0;
    if (inference.ops.submit(inference.context, context, &submit, &inference_ticket) != SACCADE_OK ||
        inference.ops.submit(inference.context, context, &submit, &extra_ticket) != SACCADE_ERROR_BUSY) {
        return 7;
    }

    SaccadeInferenceStatus inference_status = output_structure<SaccadeInferenceStatus>();
    if (inference.ops.poll(inference.context, context, inference_ticket, &inference_status) != SACCADE_OK ||
        inference_status.state != SACCADE_TICKET_RUNNING ||
        inference.ops.poll(inference.context, context, inference_ticket, &inference_status) != SACCADE_OK ||
        inference_status.state != SACCADE_TICKET_COMPLETE || inference_status.model_epoch != 7 ||
        inference_status.session_epoch != 8 || inference_status.transform_epoch != 9) {
        return 8;
    }

    std::array<uint8_t, 32> inference_output{};
    size_t required = 0;
    if (inference.ops.collect(inference.context, context, inference_ticket, {inference_output.data(), 8}, &required) !=
            SACCADE_ERROR_CAPACITY ||
        required != inference_output.size() ||
        inference.ops.collect(inference.context, context, inference_ticket,
                              {inference_output.data(), inference_output.size()}, &required) != SACCADE_OK ||
        read_u64_le(inference_output.data()) != 42 || read_u64_le(inference_output.data() + 8) != 7 ||
        read_u64_le(inference_output.data() + 16) != 8 || read_u64_le(inference_output.data() + 24) != 9) {
        return 9;
    }

    if (inference.ops.submit(inference.context, context, &submit, &inference_ticket) != SACCADE_OK ||
        inference.ops.cancel(inference.context, context, inference_ticket) != SACCADE_OK ||
        inference.ops.poll(inference.context, context, inference_ticket, &inference_status) != SACCADE_OK ||
        inference_status.state != SACCADE_TICKET_CANCELLED ||
        inference.ops.collect(inference.context, context, inference_ticket,
                              {inference_output.data(), inference_output.size()},
                              &required) != SACCADE_ERROR_CANCELLED) {
        return 30;
    }

    SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
    if (inference.ops.memory_stats(inference.context, context, &memory) != SACCADE_OK ||
        memory.host_committed != 1234 || memory.device_imported != 5678 || memory.high_water_bytes != 9012) {
        return 10;
    }
    backend.set_fault(FaultPoint::inference_synchronize, SACCADE_ERROR_BACKEND);
    if (inference.ops.synchronize(inference.context, context, 1) != SACCADE_ERROR_BACKEND ||
        inference.ops.synchronize(inference.context, context, 1) != SACCADE_OK) {
        return 11;
    }
    if (inference.ops.destroy_context(inference.context, context) != SACCADE_OK ||
        inference.ops.destroy_model(inference.context, model) != SACCADE_OK) {
        return 12;
    }

    SaccadeCaptureSourceInfo source = output_structure<SaccadeCaptureSourceInfo>();
    if (capture.ops.enumerate_sources(capture.context, 0, &source) != SACCADE_OK ||
        source.desktop_bounds.width != static_cast<int32_t>(config.capture_width)) {
        return 13;
    }
    SaccadeCaptureStreamDesc stream_desc{};
    stream_desc.struct_size = static_cast<uint32_t>(sizeof(stream_desc));
    stream_desc.api_version = SACCADE_API_VERSION;
    stream_desc.source_id = source.stable_id;
    stream_desc.pixel_format = config.capture_pixel_format;
    stream_desc.queue_capacity = 1;
    stream_desc.max_width = config.capture_width;
    stream_desc.max_height = config.capture_height;
    SaccadeCaptureStreamHandle stream = 0;
    if (capture.ops.create(capture.context, &stream_desc, &stream) != SACCADE_OK ||
        capture.ops.start(capture.context, stream) != SACCADE_OK) {
        return 14;
    }
    SaccadeCapturedFrame captured = output_structure<SaccadeCapturedFrame>();
    if (capture.ops.acquire(capture.context, stream, 0, &captured) != SACCADE_OK || captured.frame == 0 ||
        capture.ops.acquire(capture.context, stream, 0, &captured) != SACCADE_ERROR_BUSY) {
        return 15;
    }
    uint32_t damage_count = 0;
    if (capture.ops.copy_damage(capture.context, stream, captured.frame, nullptr, 0, &damage_count) !=
            SACCADE_ERROR_CAPACITY ||
        damage_count != 1) {
        return 16;
    }
    SaccadeRectI32 damage{};
    if (capture.ops.copy_damage(capture.context, stream, captured.frame, &damage, 1, &damage_count) != SACCADE_OK ||
        damage.width != static_cast<int32_t>(config.capture_width) ||
        capture.ops.release(capture.context, stream, captured.frame) != SACCADE_OK) {
        return 17;
    }
    memory = output_structure<SaccadeMemoryStats>();
    if (capture.ops.memory_stats(capture.context, stream, &memory) != SACCADE_OK || memory.host_committed != 1234 ||
        memory.device_imported != 5678) {
        return 31;
    }
    backend.set_fault(FaultPoint::capture_acquire, SACCADE_ERROR_BACKEND);
    if (capture.ops.acquire(capture.context, stream, 0, &captured) != SACCADE_ERROR_BACKEND) {
        return 32;
    }
    backend.set_fault(FaultPoint::capture_synchronize, SACCADE_ERROR_BACKEND);
    if (capture.ops.synchronize(capture.context, stream, 1) != SACCADE_ERROR_BACKEND ||
        capture.ops.synchronize(capture.context, stream, 1) != SACCADE_OK ||
        capture.ops.stop(capture.context, stream) != SACCADE_OK ||
        capture.ops.destroy(capture.context, stream) != SACCADE_OK) {
        return 33;
    }

    SaccadeOverlayDesc overlay_desc{};
    overlay_desc.struct_size = static_cast<uint32_t>(sizeof(overlay_desc));
    overlay_desc.api_version = SACCADE_API_VERSION;
    overlay_desc.source_id = source.stable_id;
    overlay_desc.desktop_bounds = source.desktop_bounds;
    overlay_desc.queue_capacity = 1;
    SaccadeOverlayHandle overlay_handle = 0;
    if (overlay.ops.create(overlay.context, &overlay_desc, &overlay_handle) != SACCADE_OK) {
        return 18;
    }
    const auto packet = overlay_packet(55, 66);
    SaccadeOverlayFrameDesc overlay_frame{};
    overlay_frame.struct_size = static_cast<uint32_t>(sizeof(overlay_frame));
    overlay_frame.api_version = SACCADE_API_VERSION;
    overlay_frame.scene_epoch = 55;
    overlay_frame.transform_epoch = 66;
    overlay_frame.packet = {packet.data(), packet.size()};
    overlay_frame.active_target_index = 0;
    if (overlay.ops.submit(overlay.context, overlay_handle, &overlay_frame) != SACCADE_OK ||
        overlay.ops.set_visible(overlay.context, overlay_handle, 1) != SACCADE_OK) {
        return 19;
    }
    auto overlay_observations = backend.observations();
    if (overlay_observations.overlay_submissions != 1 || overlay_observations.overlay_has_active_target != 0) {
        return 20;
    }
    overlay_frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
    if (overlay.ops.submit(overlay.context, overlay_handle, &overlay_frame) != SACCADE_OK) {
        return 20;
    }
    overlay_observations = backend.observations();
    if (overlay_observations.overlay_submissions != 2 || overlay_observations.last_scene_epoch != 55 ||
        overlay_observations.last_transform_epoch != 66 || overlay_observations.last_packet_hash == 0 ||
        overlay_observations.overlay_has_active_target != 1 || overlay_observations.last_active_target_index != 0 ||
        overlay_observations.overlay_visible != 1) {
        return 20;
    }
    SaccadeOverlayFrameDesc invalid_overlay_frame = overlay_frame;
    invalid_overlay_frame.scene_epoch = 54;
    if (overlay.ops.submit(overlay.context, overlay_handle, &invalid_overlay_frame) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 20;
    }
    invalid_overlay_frame = overlay_frame;
    invalid_overlay_frame.active_target_index = 1;
    if (overlay.ops.submit(overlay.context, overlay_handle, &invalid_overlay_frame) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 20;
    }
    memory = output_structure<SaccadeMemoryStats>();
    if (overlay.ops.memory_stats(overlay.context, overlay_handle, &memory) != SACCADE_OK ||
        memory.host_committed != 1234 || memory.device_imported != 5678) {
        return 34;
    }
    backend.set_fault(FaultPoint::overlay_submit, SACCADE_ERROR_BACKEND);
    if (overlay.ops.submit(overlay.context, overlay_handle, &overlay_frame) != SACCADE_ERROR_BACKEND ||
        overlay.ops.reset(overlay.context, overlay_handle) != SACCADE_OK ||
        overlay.ops.destroy(overlay.context, overlay_handle) != SACCADE_OK) {
        return 21;
    }

    SaccadeWindowInfo window = output_structure<SaccadeWindowInfo>();
    if (accessibility.ops.enumerate_windows(accessibility.context, 0, &window) != SACCADE_OK) {
        return 22;
    }
    SaccadeAccessibilityQueryDesc query{};
    query.struct_size = static_cast<uint32_t>(sizeof(query));
    query.api_version = SACCADE_API_VERSION;
    query.window_id = window.stable_id;
    query.scope = window.desktop_bounds;
    query.target_capacity = 4;
    query.session_epoch = 88;
    query.transform_epoch = 99;
    query.topology_epoch = 111;
    query.frame_id = 222;
    SaccadeTicketHandle accessibility_ticket = 0;
    if (accessibility.ops.request(accessibility.context, &query, &accessibility_ticket) != SACCADE_OK ||
        accessibility.ops.request(accessibility.context, &query, &extra_ticket) != SACCADE_ERROR_BUSY) {
        return 23;
    }
    SaccadeAccessibilityStatus accessibility_status = output_structure<SaccadeAccessibilityStatus>();
    if (accessibility.ops.poll(accessibility.context, accessibility_ticket, &accessibility_status) != SACCADE_OK ||
        accessibility_status.state != SACCADE_TICKET_RUNNING ||
        accessibility.ops.wait(accessibility.context, accessibility_ticket, 1, &accessibility_status) != SACCADE_OK ||
        accessibility_status.state != SACCADE_TICKET_COMPLETE || accessibility_status.snapshot == 0) {
        return 24;
    }
    alignas(SaccadeTargetPacketHeader)
        std::array<uint8_t, sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord)>
            accessibility_output{};
    saccade::scene::PacketView accessibility_packet{};
    if (accessibility.ops.collect(accessibility.context, accessibility_status.snapshot,
                                  {accessibility_output.data(), 8}, &required) != SACCADE_ERROR_CAPACITY ||
        required != accessibility_output.size() ||
        accessibility.ops.collect(accessibility.context, accessibility_status.snapshot,
                                  {accessibility_output.data(), accessibility_output.size()},
                                  &required) != SACCADE_OK ||
        reinterpret_cast<const SaccadeTargetPacketHeader*>(accessibility_output.data())->session_epoch != 88 ||
        reinterpret_cast<const SaccadeTargetPacketHeader*>(accessibility_output.data())->transform_epoch != 99 ||
        reinterpret_cast<const SaccadeTargetPacketHeader*>(accessibility_output.data())->topology_epoch != 111 ||
        reinterpret_cast<const SaccadeTargetPacketHeader*>(accessibility_output.data())->frame_id != 222 ||
        saccade::scene::validate_packet({accessibility_output.data(), accessibility_output.size()},
                                        &accessibility_packet) != SACCADE_OK ||
        accessibility.ops.release(accessibility.context, accessibility_status.snapshot) != SACCADE_OK) {
        return 25;
    }
    memory = output_structure<SaccadeMemoryStats>();
    if (accessibility.ops.memory_stats(accessibility.context, &memory) != SACCADE_OK || memory.host_committed != 1234 ||
        memory.device_imported != 5678) {
        return 35;
    }
    backend.set_fault(FaultPoint::accessibility_synchronize, SACCADE_ERROR_BACKEND);
    if (accessibility.ops.synchronize(accessibility.context, 1) != SACCADE_ERROR_BACKEND ||
        accessibility.ops.synchronize(accessibility.context, 1) != SACCADE_OK ||
        accessibility.ops.request(accessibility.context, &query, &accessibility_ticket) != SACCADE_OK ||
        accessibility.ops.cancel(accessibility.context, accessibility_ticket) != SACCADE_OK ||
        accessibility.ops.poll(accessibility.context, accessibility_ticket, &accessibility_status) != SACCADE_OK ||
        accessibility_status.state != SACCADE_TICKET_CANCELLED ||
        accessibility.ops.poll(accessibility.context, accessibility_ticket, &accessibility_status) !=
            SACCADE_ERROR_STALE_HANDLE) {
        return 36;
    }
    for (int index = 0; index < 9; ++index) {
        if (accessibility.ops.request(accessibility.context, &query, &accessibility_ticket) != SACCADE_OK ||
            accessibility.ops.cancel(accessibility.context, accessibility_ticket) != SACCADE_OK ||
            accessibility.ops.wait(accessibility.context, accessibility_ticket, 0, &accessibility_status) !=
                SACCADE_OK ||
            accessibility_status.state != SACCADE_TICKET_CANCELLED) {
            return 38;
        }
    }

    const auto input_fixture = saccade::test::input_plan(3);
    const SaccadeInputPlanDesc plan = saccade::test::input_plan_desc(input_fixture);
    SaccadeTicketHandle input_ticket = 0;
    if (input.ops.execute(input.context, &plan, &input_ticket) != SACCADE_OK ||
        input.ops.execute(input.context, &plan, &extra_ticket) != SACCADE_ERROR_BUSY) {
        return 26;
    }
    SaccadeInputStatus input_status = output_structure<SaccadeInputStatus>();
    if (input.ops.poll(input.context, input_ticket, &input_status) != SACCADE_OK ||
        input_status.state != SACCADE_TICKET_RUNNING ||
        input.ops.wait(input.context, input_ticket, 1, &input_status) != SACCADE_OK ||
        input_status.state != SACCADE_TICKET_COMPLETE || input_status.completed_actions != 3 ||
        input.ops.reset(input.context) != SACCADE_OK) {
        return 27;
    }
    backend.set_fault(FaultPoint::input_execute, SACCADE_ERROR_BACKEND);
    if (input.ops.execute(input.context, &plan, &input_ticket) != SACCADE_ERROR_BACKEND ||
        input.ops.execute(input.context, &plan, &input_ticket) != SACCADE_OK ||
        input.ops.cancel(input.context, input_ticket) != SACCADE_OK ||
        input.ops.poll(input.context, input_ticket, &input_status) != SACCADE_OK ||
        input_status.state != SACCADE_TICKET_CANCELLED ||
        input.ops.memory_stats(input.context, &memory) != SACCADE_OK || memory.host_committed != 1234 ||
        memory.device_imported != 5678) {
        return 28;
    }
    backend.set_fault(FaultPoint::input_release_all, SACCADE_ERROR_BACKEND);
    if (input.ops.release_all(input.context) != SACCADE_ERROR_BACKEND ||
        input.ops.release_all(input.context) != SACCADE_OK) {
        return 37;
    }

    const auto final_observations = backend.observations();
    const size_t allocations = saccade::test::end_allocation_tracking();
    if (final_observations.inference_submissions != 2 || final_observations.captured_frames != 1 ||
        final_observations.accessibility_requests != 11 || final_observations.input_executions != 2 ||
        final_observations.release_all_calls != 1 || allocations != 0) {
        return 29;
    }
    if (!max_in_flight_is_enforced()) {
        return 39;
    }
    if (!input_tickets_are_reclaimed()) {
        return 41;
    }

    return 0;
}

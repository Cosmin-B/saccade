#include "backend/registry.hpp"
#include "backends/reference_cpu/reference_cpu.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

template <typename Structure> Structure output_structure() {
    Structure value{};
    value.struct_size = static_cast<uint32_t>(sizeof(value));
    value.api_version = SACCADE_API_VERSION;
    return value;
}

bool output_prefixes_are_bounded(SaccadeEnumerateDevicesFn enumerate, void* context) {
    constexpr size_t prefix_size = offsetof(SaccadeDeviceInfo, reserved);
    alignas(SaccadeDeviceInfo) std::array<uint8_t, prefix_size + 16> short_output{};
    short_output.fill(UINT8_C(0xA5));
    const uint32_t short_size = static_cast<uint32_t>(prefix_size);
    const uint32_t api_version = SACCADE_API_VERSION;
    std::memcpy(short_output.data(), &short_size, sizeof(short_size));
    std::memcpy(short_output.data() + offsetof(SaccadeDeviceInfo, api_version), &api_version,
                sizeof(api_version));
    if (enumerate(context, 0, reinterpret_cast<SaccadeDeviceInfo*>(short_output.data())) !=
        SACCADE_OK) {
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
        extended.future[0] != UINT64_C(0xA5A5A5A5A5A5A5A5) ||
        extended.future[1] != UINT64_C(0xA5A5A5A5A5A5A5A5)) {
        return false;
    }
    return true;
}

void set_bgra(std::array<uint8_t, 8 * 6 * 4>& image, uint32_t x, uint32_t y, uint8_t value) {
    const size_t offset = (static_cast<size_t>(y) * 8 + x) * 4;
    image[offset] = value;
    image[offset + 1] = value;
    image[offset + 2] = value;
    image[offset + 3] = 255;
}

} // namespace

int main() {
    using saccade::backend::ProviderRegistry;
    using saccade::backend::reference_cpu::Backend;
    using saccade::backend::reference_cpu::DecodedOutput;
    using saccade::backend::reference_cpu::DetectionResult;
    using saccade::backend::reference_cpu::FrameView;
    using saccade::backend::reference_cpu::ModelParameters;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 18;
    }

    std::array<uint8_t, 8 * 6 * 4> image{};
    for (uint32_t y = 1; y <= 2; ++y) {
        for (uint32_t x = 1; x <= 2; ++x) {
            set_bgra(image, x, y, 255);
        }
    }
    for (uint32_t y = 3; y <= 4; ++y) {
        for (uint32_t x = 5; x <= 7; ++x) {
            set_bgra(image, x, y, 224);
        }
    }
    set_bgra(image, 0, 5, 199);

    FrameView frame{image.data(), image.size(), 8, 6, 8 * 4, SACCADE_FORMAT_BGRA8, 123};
    ModelParameters parameters{};
    parameters.luma_threshold = 200;
    parameters.minimum_area = 4;
    DetectionResult direct{};
    if (saccade::backend::reference_cpu::detect(frame, parameters, &direct) != SACCADE_OK ||
        direct.target_count != 2 || direct.targets[0].x != 1 || direct.targets[0].y != 1 ||
        direct.targets[0].width != 2 || direct.targets[0].height != 2 ||
        direct.targets[0].safe_x != 2 || direct.targets[0].safe_y != 2 ||
        direct.targets[0].area != 4 || direct.targets[1].x != 5 || direct.targets[1].y != 3 ||
        direct.targets[1].width != 3 || direct.targets[1].height != 2 ||
        direct.targets[1].area != 6 || direct.targets[0].stable_id == direct.targets[1].stable_id) {
        return 1;
    }

    FrameView rgba = frame;
    rgba.pixel_format = SACCADE_FORMAT_RGBA8;
    DetectionResult rgba_result{};
    if (saccade::backend::reference_cpu::detect(rgba, parameters, &rgba_result) != SACCADE_OK ||
        rgba_result.target_count != direct.target_count ||
        rgba_result.targets[0].stable_id != direct.targets[0].stable_id ||
        rgba_result.targets[1].confidence_q16 != direct.targets[1].confidence_q16) {
        return 15;
    }

    std::array<uint8_t, 5 * 3> bridge_image{};
    bridge_image[0] = 255;
    bridge_image[4] = 255;
    for (size_t index = 5; index < 10; ++index) {
        bridge_image[index] = 255;
    }
    FrameView bridge{bridge_image.data(), bridge_image.size(), 5, 3, 5, SACCADE_FORMAT_R8, 124};
    ModelParameters bridge_parameters{};
    bridge_parameters.luma_threshold = 200;
    bridge_parameters.minimum_area = 1;
    DetectionResult bridge_result{};
    if (saccade::backend::reference_cpu::detect(bridge, bridge_parameters, &bridge_result) !=
            SACCADE_OK ||
        bridge_result.target_count != 1 || bridge_result.targets[0].x != 0 ||
        bridge_result.targets[0].y != 0 || bridge_result.targets[0].width != 5 ||
        bridge_result.targets[0].height != 2 || bridge_result.targets[0].area != 7) {
        return 16;
    }

    FrameView malformed = frame;
    malformed.row_stride_bytes = 1;
    DetectionResult malformed_result{};
    if (saccade::backend::reference_cpu::detect(malformed, parameters, &malformed_result) !=
        SACCADE_ERROR_INVALID_ARGUMENT) {
        return 2;
    }

    saccade::test::begin_allocation_tracking();
    Backend backend;
    SaccadeFrameHandle frame_handle = 0;
    if (backend.register_frame(frame, &frame_handle) != SACCADE_OK || frame_handle == 0) {
        return 3;
    }
    SaccadeInferenceProviderDesc provider = backend.provider();
    SaccadeDeviceInfo device = backend.device_info();
    if ((provider.info.capability_bits & SACCADE_PROVIDER_CAPABILITY_ASYNC) != 0) {
        return 19;
    }
    ProviderRegistry registry;
    SaccadeProviderHandle provider_handle = 0;
    if (registry.register_inference(&provider, &provider_handle) != SACCADE_OK ||
        registry.register_device(provider_handle, &device, nullptr) != SACCADE_OK ||
        !output_prefixes_are_bounded(provider.ops.enumerate_devices, provider.context)) {
        return 4;
    }

    const auto model_bytes = saccade::backend::reference_cpu::encode_model(parameters);
    SaccadeModelInfo queried = output_structure<SaccadeModelInfo>();
    if (provider.ops.query_model(provider.context, {model_bytes.data(), model_bytes.size()},
                                 &queried) != SACCADE_OK ||
        queried.max_output_bytes != saccade::backend::reference_cpu::maximum_output_size) {
        return 5;
    }
    std::array<uint8_t, saccade::backend::reference_cpu::model_byte_count> bad_model{};
    if (provider.ops.query_model(provider.context, {bad_model.data(), bad_model.size()},
                                 &queried) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 6;
    }

    SaccadeModelDesc model_desc{};
    model_desc.struct_size = static_cast<uint32_t>(sizeof(model_desc));
    model_desc.api_version = SACCADE_API_VERSION;
    model_desc.bytes = {model_bytes.data(), model_bytes.size()};
    model_desc.stable_id = 44;
    model_desc.device_id = device.stable_id;
    SaccadeModelHandle model = 0;
    if (provider.ops.create_model(provider.context, &model_desc, &model) != SACCADE_OK) {
        return 7;
    }

    SaccadeExecutionContextDesc context_desc{};
    context_desc.struct_size = static_cast<uint32_t>(sizeof(context_desc));
    context_desc.api_version = SACCADE_API_VERSION;
    context_desc.model = model;
    context_desc.device_id = device.stable_id;
    context_desc.queue_capacity = 1;
    context_desc.max_in_flight = 1;
    SaccadeExecutionContextHandle context = 0;
    if (provider.ops.create_context(provider.context, &context_desc, &context) != SACCADE_OK) {
        return 8;
    }

    SaccadeInferenceSubmitDesc submit{};
    submit.struct_size = static_cast<uint32_t>(sizeof(submit));
    submit.api_version = SACCADE_API_VERSION;
    submit.frame = frame_handle;
    submit.scope = {0, 0, 8, 6};
    submit.output_capacity =
        static_cast<uint32_t>(saccade::backend::reference_cpu::maximum_output_size);
    submit.model_epoch = 5;
    submit.session_epoch = 6;
    submit.transform_epoch = 7;
    SaccadeTicketHandle ticket = 0;
    SaccadeTicketHandle extra = 0;
    if (provider.ops.submit(provider.context, context, &submit, &ticket) != SACCADE_OK ||
        provider.ops.submit(provider.context, context, &submit, &extra) != SACCADE_ERROR_BUSY ||
        backend.release_frame(frame_handle) != SACCADE_ERROR_BUSY ||
        provider.ops.cancel(provider.context, context, ticket) != SACCADE_OK) {
        return 9;
    }
    SaccadeInferenceStatus status = output_structure<SaccadeInferenceStatus>();
    if (provider.ops.poll(provider.context, context, ticket, &status) != SACCADE_OK ||
        status.state != SACCADE_TICKET_CANCELLED) {
        return 10;
    }
    size_t required = 0;
    std::array<uint8_t, saccade::backend::reference_cpu::maximum_output_size> output{};
    if (provider.ops.collect(provider.context, context, ticket, {output.data(), output.size()},
                             &required) != SACCADE_ERROR_CANCELLED ||
        provider.ops.submit(provider.context, context, &submit, &ticket) != SACCADE_OK ||
        provider.ops.poll(provider.context, context, ticket, &status) != SACCADE_OK ||
        status.state != SACCADE_TICKET_COMPLETE || status.frame_id != 123 ||
        status.model_epoch != 5 || status.session_epoch != 6 || status.transform_epoch != 7 ||
        status.produced_bytes == 0) {
        return 11;
    }

    if (provider.ops.collect(provider.context, context, ticket, {output.data(), 8}, &required) !=
            SACCADE_ERROR_CAPACITY ||
        required != status.produced_bytes ||
        provider.ops.collect(provider.context, context, ticket, {output.data(), output.size()},
                             &required) != SACCADE_OK) {
        return 12;
    }
    DecodedOutput decoded{};
    if (saccade::backend::reference_cpu::decode_output({output.data(), required}, &decoded) !=
            SACCADE_OK ||
        decoded.frame_id != 123 || decoded.model_epoch != 5 || decoded.session_epoch != 6 ||
        decoded.transform_epoch != 7 || decoded.detections.target_count != direct.target_count ||
        decoded.detections.targets[0].stable_id != direct.targets[0].stable_id ||
        decoded.detections.targets[1].confidence_q16 != direct.targets[1].confidence_q16) {
        return 13;
    }

    SaccadeMemoryStats memory = output_structure<SaccadeMemoryStats>();
    if (provider.ops.memory_stats(provider.context, context, &memory) != SACCADE_OK ||
        memory.host_reserved == 0 || memory.device_owned != 0 || memory.framework_opaque != 0 ||
        memory.copied_bytes < required ||
        provider.ops.synchronize(provider.context, context, 0) != SACCADE_OK ||
        provider.ops.destroy_context(provider.context, context) != SACCADE_OK ||
        provider.ops.destroy_model(provider.context, model) != SACCADE_OK ||
        backend.release_frame(frame_handle) != SACCADE_OK) {
        return 14;
    }
    if (saccade::test::end_allocation_tracking() != 0) {
        return 17;
    }

    return 0;
}

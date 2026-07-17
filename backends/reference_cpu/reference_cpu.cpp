#include "backends/reference_cpu/reference_cpu.hpp"

#include "backends/callback_guard.hpp"
#include "core/handle_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace saccade::backend::reference_cpu {
namespace {

constexpr uint64_t provider_id = UINT64_C(0x5245464350550001);
constexpr uint64_t device_id = UINT64_C(0x5245464350551001);
constexpr uint32_t maximum_width = 1024;
constexpr uint32_t maximum_height = 1024;
constexpr size_t maximum_components = 128;

constexpr uint32_t api_major(uint32_t version) noexcept {
    return version >> 16U;
}

void write_u32_le(uint8_t* destination, uint32_t value) noexcept {
    for (uint32_t index = 0; index < 4; ++index) {
        destination[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

uint32_t read_u32_le(const uint8_t* source) noexcept {
    uint32_t value = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(source[index]) << (index * 8U);
    }
    return value;
}

bool reserved_is_zero(const void* object, uint32_t struct_size, size_t reserved_offset, size_t current_size) noexcept {
    const size_t available = std::min(static_cast<size_t>(struct_size), current_size);
    const auto* bytes = static_cast<const uint8_t*>(object);
    for (size_t index = reserved_offset; index < available; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

template <typename Structure> SaccadeResult read_structure(const Structure* source, Structure* out_value) noexcept {
    if (source == nullptr || out_value == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t struct_size = 0;
    std::memcpy(&struct_size, static_cast<const void*>(source), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_value = {};
    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(*out_value));
    std::memcpy(out_value, static_cast<const void*>(source), copy_size);
    if (api_major(out_value->api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    if (!reserved_is_zero(out_value, struct_size, offsetof(Structure, reserved), sizeof(*out_value))) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    return SACCADE_OK;
}

template <typename Structure> SaccadeResult write_structure(Structure* destination, Structure value) noexcept {
    if (destination == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    uint32_t struct_size = 0;
    uint32_t api_version = 0;
    std::memcpy(&struct_size, static_cast<const void*>(destination), sizeof(struct_size));
    if (static_cast<size_t>(struct_size) < offsetof(Structure, reserved)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    std::memcpy(&api_version,
                static_cast<const uint8_t*>(static_cast<const void*>(destination)) + offsetof(Structure, api_version),
                sizeof(api_version));
    if (api_major(api_version) != api_major(SACCADE_API_VERSION)) {
        return SACCADE_ERROR_VERSION;
    }
    const size_t copy_size = std::min(static_cast<size_t>(struct_size), sizeof(value));
    value.struct_size = static_cast<uint32_t>(copy_size);
    value.api_version = SACCADE_API_VERSION;
    std::memcpy(static_cast<void*>(destination), &value, copy_size);
    return SACCADE_OK;
}

size_t bytes_per_pixel(uint32_t format) noexcept {
    switch (format) {
    case SACCADE_FORMAT_R8:
        return 1;
    case SACCADE_FORMAT_BGRA8:
    case SACCADE_FORMAT_RGBA8:
    case SACCADE_FORMAT_BGRX8:
        return 4;
    default:
        return 0;
    }
}

SaccadeResult validate_frame(const FrameView& frame) noexcept {
    const size_t pixel_size = bytes_per_pixel(frame.pixel_format);
    if (frame.data == nullptr || frame.size == 0 || frame.width == 0 || frame.height == 0 ||
        frame.width > maximum_width || frame.height > maximum_height || pixel_size == 0 || frame.frame_id == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (static_cast<size_t>(frame.width) > std::numeric_limits<size_t>::max() / pixel_size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t row_bytes = static_cast<size_t>(frame.width) * pixel_size;
    if (frame.row_stride_bytes < row_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t rows_before_last = static_cast<size_t>(frame.height - 1U);
    if (rows_before_last > (std::numeric_limits<size_t>::max() - row_bytes) / frame.row_stride_bytes) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const size_t required = rows_before_last * frame.row_stride_bytes + row_bytes;
    return required <= frame.size ? SACCADE_OK : SACCADE_ERROR_INVALID_ARGUMENT;
}

uint8_t pixel_luma(const FrameView& frame, uint32_t x, uint32_t y) noexcept {
    const size_t pixel_size = bytes_per_pixel(frame.pixel_format);
    const uint8_t* pixel =
        frame.data + static_cast<size_t>(y) * frame.row_stride_bytes + static_cast<size_t>(x) * pixel_size;
    if (frame.pixel_format == SACCADE_FORMAT_R8) {
        return pixel[0];
    }
    const uint32_t red = frame.pixel_format == SACCADE_FORMAT_RGBA8 ? pixel[0] : pixel[2];
    const uint32_t green = pixel[1];
    const uint32_t blue = frame.pixel_format == SACCADE_FORMAT_RGBA8 ? pixel[2] : pixel[0];
    return static_cast<uint8_t>((77U * red + 150U * green + 29U * blue + 128U) >> 8U);
}

uint64_t target_id(uint64_t frame_id, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
    uint64_t hash = UINT64_C(14695981039346656037);
    const std::array<uint64_t, 5> values{frame_id, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                         static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    for (uint64_t value : values) {
        for (uint32_t index = 0; index < 8; ++index) {
            hash ^= static_cast<uint8_t>(value >> (index * 8U));
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash == 0 ? 1 : hash;
}

SaccadeResult parse_model(SaccadeSpanU8 bytes, ModelParameters* out_parameters) noexcept {
    if (out_parameters == nullptr || bytes.data == nullptr || bytes.size != model_byte_count || bytes.data[0] != 'S' ||
        bytes.data[1] != 'C' || bytes.data[2] != 'M' || bytes.data[3] != '1' || bytes.data[5] != 0 ||
        bytes.data[6] != 0 || bytes.data[7] != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t minimum_area = read_u32_le(bytes.data + 8);
    if (minimum_area == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    out_parameters->luma_threshold = bytes.data[4];
    out_parameters->minimum_area = minimum_area;
    return SACCADE_OK;
}

size_t serialize_output(uint8_t* destination, size_t capacity, uint64_t frame_id, uint64_t model_epoch,
                        uint64_t session_epoch, uint64_t transform_epoch, uint64_t topology_epoch, uint64_t source_id,
                        const DetectionResult& result) noexcept {
    const size_t required = serialized_header_size + static_cast<size_t>(result.target_count) * serialized_target_size;
    if (destination == nullptr || capacity < required) {
        return 0;
    }
    SaccadeTargetPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    header.target_count = result.target_count;
    header.target_stride = sizeof(SaccadeTargetRecord);
    header.coordinate_space = SACCADE_COORDINATE_SPACE_SOURCE_Q8;
    header.frame_id = frame_id;
    header.model_epoch = model_epoch;
    header.session_epoch = session_epoch;
    header.transform_epoch = transform_epoch;
    header.topology_epoch = topology_epoch;
    header.source_id = source_id;
    header.targets_offset = sizeof(header);
    header.total_size = required;
    std::memcpy(destination, &header, sizeof(header));
    for (uint32_t index = 0; index < result.target_count; ++index) {
        const Target& target = result.targets[index];
        uint8_t* output = destination + serialized_header_size + static_cast<size_t>(index) * serialized_target_size;
        SaccadeTargetRecord record{};
        record.target_id = target.stable_id;
        record.x_q8 = target.x << 8;
        record.y_q8 = target.y << 8;
        record.width_q8 = target.width << 8;
        record.height_q8 = target.height << 8;
        record.safe_x_q8 = target.safe_x << 8;
        record.safe_y_q8 = target.safe_y << 8;
        record.confidence_q16 = target.confidence_q16;
        record.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        record.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
        record.flags = SACCADE_TARGET_ACTIONABLE;
        record.order = index;
        std::memcpy(output, &record, sizeof(record));
    }
    return required;
}

SaccadeSpanU8 literal_span(const char* text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text), std::strlen(text)};
}

} // namespace

std::array<uint8_t, model_byte_count> encode_model(const ModelParameters& parameters) noexcept {
    std::array<uint8_t, model_byte_count> bytes{};
    bytes[0] = 'S';
    bytes[1] = 'C';
    bytes[2] = 'M';
    bytes[3] = '1';
    bytes[4] = parameters.luma_threshold;
    write_u32_le(bytes.data() + 8, parameters.minimum_area);
    return bytes;
}

SaccadeResult detect(const FrameView& frame, const ModelParameters& parameters, DetectionResult* out_result) noexcept {
    if (out_result == nullptr || parameters.minimum_area == 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_result = {};
    const SaccadeResult frame_result = validate_frame(frame);
    if (frame_result != SACCADE_OK) {
        return frame_result;
    }

    struct Component {
        uint16_t parent = 0;
        uint32_t min_x = 0;
        uint32_t min_y = 0;
        uint32_t max_x = 0;
        uint32_t max_y = 0;
        uint32_t area = 0;
        uint64_t luma_sum = 0;
        bool used = false;
    };

    std::array<Component, maximum_components> components{};
    std::array<uint16_t, maximum_width> previous{};
    std::array<uint16_t, maximum_width> current{};
    size_t component_count = 0;

    const auto root_of = [&](uint16_t label) noexcept {
        uint16_t root = label;
        while (components[static_cast<size_t>(root - 1U)].parent != root) {
            root = components[static_cast<size_t>(root - 1U)].parent;
        }
        return root;
    };

    for (uint32_t y = 0; y < frame.height; ++y) {
        current.fill(0);
        uint32_t x = 0;
        while (x < frame.width) {
            if (pixel_luma(frame, x, y) < parameters.luma_threshold) {
                ++x;
                continue;
            }
            const uint32_t run_start = x;
            uint64_t run_luma = 0;
            while (x < frame.width) {
                const uint8_t luma = pixel_luma(frame, x, y);
                if (luma < parameters.luma_threshold) {
                    break;
                }
                run_luma += luma;
                ++x;
            }
            const uint32_t run_end = x - 1U;

            uint16_t label = 0;
            for (uint32_t column = run_start; column <= run_end; ++column) {
                if (previous[column] == 0) {
                    continue;
                }
                const uint16_t candidate = root_of(previous[column]);
                if (label == 0) {
                    label = candidate;
                    continue;
                }
                const uint16_t root = root_of(label);
                if (candidate == root) {
                    label = root;
                    continue;
                }
                Component& destination = components[static_cast<size_t>(root - 1U)];
                Component& source = components[static_cast<size_t>(candidate - 1U)];
                destination.min_x = std::min(destination.min_x, source.min_x);
                destination.min_y = std::min(destination.min_y, source.min_y);
                destination.max_x = std::max(destination.max_x, source.max_x);
                destination.max_y = std::max(destination.max_y, source.max_y);
                destination.area += source.area;
                destination.luma_sum += source.luma_sum;
                source.parent = root;
                source.used = false;
                label = root;
            }

            if (label == 0) {
                if (component_count == maximum_components) {
                    return SACCADE_ERROR_CAPACITY;
                }
                const uint16_t new_label = static_cast<uint16_t>(component_count + 1U);
                Component& component = components[component_count++];
                component.parent = new_label;
                component.min_x = run_start;
                component.min_y = y;
                component.max_x = run_end;
                component.max_y = y;
                component.used = true;
                label = new_label;
            }

            label = root_of(label);
            Component& component = components[static_cast<size_t>(label - 1U)];
            component.min_x = std::min(component.min_x, run_start);
            component.min_y = std::min(component.min_y, y);
            component.max_x = std::max(component.max_x, run_end);
            component.max_y = std::max(component.max_y, y);
            component.area += run_end - run_start + 1U;
            component.luma_sum += run_luma;
            for (uint32_t column = run_start; column <= run_end; ++column) {
                current[column] = label;
            }
        }
        for (uint32_t column = 0; column < frame.width; ++column) {
            if (current[column] != 0) {
                current[column] = root_of(current[column]);
            }
        }
        previous = current;
    }

    for (size_t index = 0; index < component_count; ++index) {
        const Component& component = components[index];
        if (!component.used || component.parent != index + 1U || component.area < parameters.minimum_area) {
            continue;
        }
        if (out_result->target_count == maximum_targets) {
            *out_result = {};
            return SACCADE_ERROR_CAPACITY;
        }
        Target target{};
        target.x = static_cast<int32_t>(component.min_x);
        target.y = static_cast<int32_t>(component.min_y);
        target.width = static_cast<int32_t>(component.max_x - component.min_x + 1U);
        target.height = static_cast<int32_t>(component.max_y - component.min_y + 1U);
        target.safe_x = target.x + target.width / 2;
        target.safe_y = target.y + target.height / 2;
        target.area = component.area;
        target.confidence_q16 = static_cast<uint32_t>(
            (component.luma_sum * UINT64_C(65535) + static_cast<uint64_t>(component.area) * UINT64_C(127)) /
            (static_cast<uint64_t>(component.area) * UINT64_C(255)));
        target.stable_id = target_id(frame.frame_id, target.x, target.y, target.width, target.height);
        out_result->targets[out_result->target_count++] = target;
    }

    for (uint32_t index = 1; index < out_result->target_count; ++index) {
        const Target candidate = out_result->targets[index];
        uint32_t position = index;
        while (position > 0) {
            const Target& previous_target = out_result->targets[position - 1U];
            if (previous_target.y < candidate.y ||
                (previous_target.y == candidate.y && previous_target.x <= candidate.x)) {
                break;
            }
            out_result->targets[position] = previous_target;
            --position;
        }
        out_result->targets[position] = candidate;
    }
    return SACCADE_OK;
}

SaccadeResult decode_output(SaccadeSpanU8 bytes, DecodedOutput* out_output) noexcept {
    if (out_output == nullptr || bytes.data == nullptr || bytes.size < serialized_header_size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_output = {};
    SaccadeTargetPacketHeader header{};
    std::memcpy(&header, bytes.data, sizeof(header));
    if (header.struct_size != sizeof(header) || header.packet_version != SACCADE_TARGET_PACKET_VERSION ||
        header.coordinate_space != SACCADE_COORDINATE_SPACE_SOURCE_Q8 || header.scene_epoch != 0 ||
        header.target_count > maximum_targets || header.target_stride != sizeof(SaccadeTargetRecord) ||
        header.targets_offset != sizeof(header) || header.total_size != bytes.size) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    out_output->frame_id = header.frame_id;
    out_output->model_epoch = header.model_epoch;
    out_output->session_epoch = header.session_epoch;
    out_output->transform_epoch = header.transform_epoch;
    out_output->topology_epoch = header.topology_epoch;
    out_output->source_id = header.source_id;
    out_output->detections.target_count = header.target_count;
    for (uint32_t index = 0; index < header.target_count; ++index) {
        const uint8_t* input =
            bytes.data + serialized_header_size + static_cast<size_t>(index) * serialized_target_size;
        SaccadeTargetRecord record{};
        std::memcpy(&record, input, sizeof(record));
        Target& target = out_output->detections.targets[index];
        target.stable_id = record.target_id;
        target.x = record.x_q8 / 256;
        target.y = record.y_q8 / 256;
        target.width = record.width_q8 / 256;
        target.height = record.height_q8 / 256;
        target.safe_x = record.safe_x_q8 / 256;
        target.safe_y = record.safe_y_q8 / 256;
        target.area = static_cast<uint32_t>(target.width * target.height);
        target.confidence_q16 = record.confidence_q16;
    }
    return SACCADE_OK;
}

struct Backend::Impl {
    struct Frame {
        FrameView view{};
        uint32_t references = 0;
    };

    struct Model {
        ModelParameters parameters{};
        uint64_t stable_id = 0;
        uint32_t context_count = 0;
    };

    struct Context {
        SaccadeModelHandle model = 0;
        uint32_t queue_capacity = 0;
        uint32_t active_tickets = 0;
    };

    struct Ticket {
        SaccadeExecutionContextHandle context = 0;
        FrameView frame{};
        uint64_t frame_id = 0;
        SaccadeRectI32 scope{};
        uint64_t model_epoch = 0;
        uint64_t session_epoch = 0;
        uint64_t transform_epoch = 0;
        uint64_t topology_epoch = 0;
        uint64_t source_id = 0;
        uint32_t state = SACCADE_TICKET_QUEUED;
        SaccadeResult result = SACCADE_OK;
        uint32_t output_size = 0;
        bool counted = true;
        std::array<uint8_t, maximum_output_size> output{};
    };

    static Impl* from(void* context) noexcept {
        if (context == nullptr) {
            return nullptr;
        }
        return &static_cast<Backend*>(context)->impl();
    }

    void release_ticket(Ticket& ticket) noexcept {
        if (!ticket.counted) {
            return;
        }
        if (Context* context = contexts.get(ticket.context)) {
            --context->active_tickets;
        }
        ticket.counted = false;
    }

    void run_ticket(SaccadeTicketHandle, Ticket& ticket) noexcept {
        if (ticket.state != SACCADE_TICKET_QUEUED) {
            return;
        }
        Context* context = contexts.get(ticket.context);
        Model* model = context == nullptr ? nullptr : models.get(context->model);
        if (context == nullptr || model == nullptr) {
            ticket.state = SACCADE_TICKET_FAILED;
            ticket.result = SACCADE_ERROR_STALE_HANDLE;
            return;
        }

        const size_t pixel_size = bytes_per_pixel(ticket.frame.pixel_format);
        FrameView scoped = ticket.frame;
        scoped.data += static_cast<size_t>(ticket.scope.y) * scoped.row_stride_bytes +
                       static_cast<size_t>(ticket.scope.x) * pixel_size;
        scoped.size -= static_cast<size_t>(ticket.scope.y) * scoped.row_stride_bytes +
                       static_cast<size_t>(ticket.scope.x) * pixel_size;
        scoped.width = static_cast<uint32_t>(ticket.scope.width);
        scoped.height = static_cast<uint32_t>(ticket.scope.height);

        DetectionResult result{};
        ticket.result = detect(scoped, model->parameters, &result);
        if (ticket.result != SACCADE_OK) {
            ticket.state = SACCADE_TICKET_FAILED;
            return;
        }
        for (uint32_t index = 0; index < result.target_count; ++index) {
            Target& target = result.targets[index];
            target.x += ticket.scope.x;
            target.y += ticket.scope.y;
            target.safe_x += ticket.scope.x;
            target.safe_y += ticket.scope.y;
            target.stable_id = target_id(ticket.frame.frame_id, target.x, target.y, target.width, target.height);
        }
        const size_t output_size = serialize_output(ticket.output.data(), ticket.output.size(), ticket.frame.frame_id,
                                                    ticket.model_epoch, ticket.session_epoch, ticket.transform_epoch,
                                                    ticket.topology_epoch, ticket.source_id, result);
        if (output_size == 0 || output_size > UINT32_MAX) {
            ticket.state = SACCADE_TICKET_FAILED;
            ticket.result = SACCADE_ERROR_CAPACITY;
            return;
        }
        ticket.output_size = static_cast<uint32_t>(output_size);
        ticket.state = SACCADE_TICKET_COMPLETE;
        copied_bytes += output_size;
    }

    static SaccadeInferenceStatus status(SaccadeTicketHandle handle, const Ticket& ticket) noexcept {
        SaccadeInferenceStatus result{};
        result.struct_size = static_cast<uint32_t>(sizeof(result));
        result.api_version = SACCADE_API_VERSION;
        result.state = ticket.state;
        result.result = ticket.result;
        result.ticket = handle;
        result.frame_id = ticket.frame_id;
        result.model_epoch = ticket.model_epoch;
        result.session_epoch = ticket.session_epoch;
        result.transform_epoch = ticket.transform_epoch;
        result.topology_epoch = ticket.topology_epoch;
        result.source_id = ticket.source_id;
        result.produced_bytes = ticket.state == SACCADE_TICKET_COMPLETE ? ticket.output_size : 0;
        result.required_bytes = ticket.output_size;
        return result;
    }

    static SaccadeResult SACCADE_CALL enumerate_devices(void*, uint32_t, SaccadeDeviceInfo*);
    static SaccadeResult SACCADE_CALL query_model(void*, SaccadeSpanU8, SaccadeModelInfo*);
    static SaccadeResult SACCADE_CALL create_model(void*, const SaccadeModelDesc*, SaccadeModelHandle*);
    static SaccadeResult SACCADE_CALL destroy_model(void*, SaccadeModelHandle);
    static SaccadeResult SACCADE_CALL create_context(void*, const SaccadeExecutionContextDesc*,
                                                     SaccadeExecutionContextHandle*);
    static SaccadeResult SACCADE_CALL destroy_context(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL submit(void*, SaccadeExecutionContextHandle, const SaccadeInferenceDispatchDesc*,
                                             SaccadeTicketHandle*);
    static SaccadeResult SACCADE_CALL poll(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                           SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL wait(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, uint64_t,
                                           SaccadeInferenceStatus*);
    static SaccadeResult SACCADE_CALL collect(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                              SaccadeMutableSpanU8, size_t*);
    static SaccadeResult SACCADE_CALL cancel(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle);
    static SaccadeResult SACCADE_CALL reset(void*, SaccadeExecutionContextHandle);
    static SaccadeResult SACCADE_CALL synchronize(void*, SaccadeExecutionContextHandle, uint64_t);
    static SaccadeResult SACCADE_CALL memory_stats(void*, SaccadeExecutionContextHandle, SaccadeMemoryStats*);
    core::HandleTable<Frame, 8> frames;
    core::HandleTable<Model, 4> models;
    core::HandleTable<Context, 4> contexts;
    core::HandleTable<Ticket, 8> tickets;
    uint64_t copied_bytes = 0;
};

Backend::Impl& Backend::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const Backend::Impl& Backend::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

Backend::Backend() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= alignof(Backend));
    ::new (static_cast<void*>(storage_.data())) Impl;
}

Backend::~Backend() {
    impl().~Impl();
}

SaccadeResult Backend::register_frame(const FrameView& frame, SaccadeFrameHandle* out_frame) noexcept {
    if (out_frame == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = 0;
    const SaccadeResult validation = validate_frame(frame);
    if (validation != SACCADE_OK) {
        return validation;
    }
    Impl& state = impl();
    return state.frames.emplace(out_frame, Impl::Frame{frame, 0});
}

SaccadeResult Backend::release_frame(SaccadeFrameHandle frame) noexcept {
    Impl& state = impl();
    Impl::Frame* value = state.frames.get(frame);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->references != 0) {
        return SACCADE_ERROR_BUSY;
    }
    return state.frames.erase(frame);
}

SaccadeResult SACCADE_CALL Backend::Impl::enumerate_devices(void* context, uint32_t index,
                                                            SaccadeDeviceInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (index != 0) {
        return SACCADE_ERROR_NOT_FOUND;
    }
    return write_structure(out_info, static_cast<Backend*>(context)->device_info());
}

SaccadeResult SACCADE_CALL Backend::Impl::query_model(void* context, SaccadeSpanU8 bytes, SaccadeModelInfo* out_info) {
    Impl* state = from(context);
    if (state == nullptr || out_info == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    ModelParameters parameters{};
    const SaccadeResult validation = parse_model(bytes, &parameters);
    if (validation != SACCADE_OK) {
        return validation;
    }
    SaccadeModelInfo info{};
    info.stable_id = UINT64_C(0x5245464350552001);
    info.required_host_bytes = sizeof(Impl);
    info.required_device_bytes = 0;
    info.capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT;
    info.max_output_bytes = static_cast<uint32_t>(maximum_output_size);
    info.name = literal_span("scalar component detector");
    return write_structure(out_info, info);
}

SaccadeResult SACCADE_CALL Backend::Impl::create_model(void* context, const SaccadeModelDesc* desc,
                                                       SaccadeModelHandle* out_model) {
    Impl* state = from(context);
    if (state == nullptr || out_model == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_model = 0;
    SaccadeModelDesc value{};
    const SaccadeResult descriptor_result = read_structure(desc, &value);
    if (descriptor_result != SACCADE_OK) {
        return descriptor_result;
    }
    if (value.stable_id == 0 || value.device_id != device_id || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    ModelParameters parameters{};
    const SaccadeResult model_result = parse_model(value.bytes, &parameters);
    if (model_result != SACCADE_OK) {
        return model_result;
    }
    return state->models.emplace(out_model, Model{parameters, value.stable_id, 0});
}

SaccadeResult SACCADE_CALL Backend::Impl::destroy_model(void* context, SaccadeModelHandle model) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Model* value = state->models.get(model);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->context_count != 0) {
        return SACCADE_ERROR_BUSY;
    }
    return state->models.erase(model);
}

SaccadeResult SACCADE_CALL Backend::Impl::create_context(void* context, const SaccadeExecutionContextDesc* desc,
                                                         SaccadeExecutionContextHandle* out_context) {
    Impl* state = from(context);
    if (state == nullptr || out_context == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_context = 0;
    SaccadeExecutionContextDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.model == 0 || value.device_id != device_id || value.queue_capacity == 0 || value.queue_capacity > 8 ||
        value.max_in_flight == 0 || value.max_in_flight > value.queue_capacity || value.flags != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Model* model = state->models.get(value.model);
    if (model == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult result = state->contexts.emplace(out_context, Context{value.model, value.queue_capacity, 0});
    if (result == SACCADE_OK) {
        ++model->context_count;
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::destroy_context(void* context,
                                                          SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Context* value = state->contexts.get(execution_context);
    if (value == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (value->active_tickets != 0) {
        return SACCADE_ERROR_BUSY;
    }
    Model* model = state->models.get(value->model);
    if (model != nullptr) {
        --model->context_count;
    }
    return state->contexts.erase(execution_context);
}

SaccadeResult SACCADE_CALL Backend::Impl::submit(void* context, SaccadeExecutionContextHandle execution_context,
                                                 const SaccadeInferenceDispatchDesc* desc,
                                                 SaccadeTicketHandle* out_ticket) {
    Impl* state = from(context);
    if (state == nullptr || out_ticket == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_ticket = 0;
    SaccadeInferenceDispatchDesc value{};
    const SaccadeResult validation = read_structure(desc, &value);
    if (validation != SACCADE_OK) {
        return validation;
    }
    if (value.frame.storage != SACCADE_FRAME_STORAGE_HOST || value.frame.host_data.data == nullptr ||
        value.frame.frame_id == 0 || value.frame.width == 0 || value.frame.height == 0 || value.scope.x < 0 ||
        value.scope.y < 0 || value.scope.width <= 0 || value.scope.height <= 0 ||
        value.output_capacity < maximum_output_size || value.model_epoch == 0 || value.session_epoch == 0 ||
        value.transform_epoch == 0 || value.topology_epoch == 0 || value.source_id == 0 || value.flags != 0 ||
        (value.priority_region_count != 0 && value.priority_regions == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Context* execution = state->contexts.get(execution_context);
    if (execution == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const int64_t scope_right = static_cast<int64_t>(value.scope.x) + value.scope.width;
    const int64_t scope_bottom = static_cast<int64_t>(value.scope.y) + value.scope.height;
    if (scope_right > value.frame.width || scope_bottom > value.frame.height) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (execution->active_tickets >= execution->queue_capacity) {
        return SACCADE_ERROR_BUSY;
    }
    Ticket ticket{};
    ticket.context = execution_context;
    ticket.frame = {value.frame.host_data.data,   value.frame.host_data.size, value.frame.width,   value.frame.height,
                    value.frame.row_stride_bytes, value.frame.pixel_format,   value.frame.frame_id};
    ticket.frame_id = value.frame.frame_id;
    ticket.scope = value.scope;
    ticket.model_epoch = value.model_epoch;
    ticket.session_epoch = value.session_epoch;
    ticket.transform_epoch = value.transform_epoch;
    ticket.topology_epoch = value.topology_epoch;
    ticket.source_id = value.source_id;
    const SaccadeResult result = state->tickets.emplace(out_ticket, ticket);
    if (result == SACCADE_OK) {
        ++execution->active_tickets;
    }
    return result;
}

SaccadeResult SACCADE_CALL Backend::Impl::poll(void* context, SaccadeExecutionContextHandle execution_context,
                                               SaccadeTicketHandle handle, SaccadeInferenceStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Ticket* ticket = state->tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    state->run_ticket(handle, *ticket);
    return write_structure(out_status, status(handle, *ticket));
}

SaccadeResult SACCADE_CALL Backend::Impl::wait(void* context, SaccadeExecutionContextHandle execution_context,
                                               SaccadeTicketHandle handle, uint64_t timeout_ns,
                                               SaccadeInferenceStatus* out_status) {
    Impl* state = from(context);
    if (state == nullptr || out_status == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Ticket* ticket = state->tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state == SACCADE_TICKET_QUEUED && timeout_ns == 0) {
        const SaccadeResult output = write_structure(out_status, status(handle, *ticket));
        return output == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : output;
    }
    state->run_ticket(handle, *ticket);
    return write_structure(out_status, status(handle, *ticket));
}

SaccadeResult SACCADE_CALL Backend::Impl::collect(void* context, SaccadeExecutionContextHandle execution_context,
                                                  SaccadeTicketHandle handle, SaccadeMutableSpanU8 output,
                                                  size_t* out_required) {
    Impl* state = from(context);
    if (state == nullptr || out_required == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_required = 0;
    Ticket* ticket = state->tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    *out_required = ticket->output_size;
    if (ticket->state == SACCADE_TICKET_CANCELLED || ticket->state == SACCADE_TICKET_FAILED) {
        const SaccadeResult result = ticket->result;
        state->release_ticket(*ticket);
        (void)state->tickets.erase(handle);
        return result;
    }
    if (ticket->state != SACCADE_TICKET_COMPLETE) {
        return SACCADE_ERROR_BUSY;
    }
    if (output.data == nullptr || output.size < ticket->output_size) {
        return SACCADE_ERROR_CAPACITY;
    }
    std::memcpy(output.data, ticket->output.data(), ticket->output_size);
    state->release_ticket(*ticket);
    return state->tickets.erase(handle);
}

SaccadeResult SACCADE_CALL Backend::Impl::cancel(void* context, SaccadeExecutionContextHandle execution_context,
                                                 SaccadeTicketHandle handle) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Ticket* ticket = state->tickets.get(handle);
    if (ticket == nullptr || ticket->context != execution_context) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    if (ticket->state != SACCADE_TICKET_QUEUED) {
        return SACCADE_ERROR_STATE;
    }
    ticket->state = SACCADE_TICKET_CANCELLED;
    ticket->result = SACCADE_ERROR_CANCELLED;
    state->release_ticket(*ticket);
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::reset(void* context, SaccadeExecutionContextHandle execution_context) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Context* execution = state->contexts.get(execution_context);
    if (execution == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    std::array<SaccadeTicketHandle, 8> handles{};
    size_t count = 0;
    state->tickets.for_each([&](SaccadeTicketHandle handle, const Ticket& ticket) noexcept {
        if (ticket.context == execution_context) {
            handles[count++] = handle;
        }
    });
    for (size_t index = 0; index < count; ++index) {
        Ticket* ticket = state->tickets.get(handles[index]);
        if (ticket != nullptr) {
            state->release_ticket(*ticket);
        }
        (void)state->tickets.erase(handles[index]);
    }
    execution->active_tickets = 0;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::synchronize(void* context, SaccadeExecutionContextHandle execution_context,
                                                      uint64_t timeout_ns) {
    Impl* state = from(context);
    if (state == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state->contexts.get(execution_context) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    bool queued = false;
    state->tickets.for_each([&](SaccadeTicketHandle handle, Ticket& ticket) noexcept {
        if (ticket.context == execution_context && ticket.state == SACCADE_TICKET_QUEUED) {
            queued = true;
            if (timeout_ns != 0) {
                state->run_ticket(handle, ticket);
            }
        }
    });
    return queued && timeout_ns == 0 ? SACCADE_ERROR_TIMEOUT : SACCADE_OK;
}

SaccadeResult SACCADE_CALL Backend::Impl::memory_stats(void* context, SaccadeExecutionContextHandle execution_context,
                                                       SaccadeMemoryStats* out_stats) {
    Impl* state = from(context);
    if (state == nullptr || out_stats == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (state->contexts.get(execution_context) == nullptr) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    SaccadeMemoryStats stats{};
    stats.host_committed = sizeof(Impl);
    stats.host_reserved = Backend::storage_size;
    stats.copied_bytes = state->copied_bytes;
    stats.high_water_bytes = Backend::storage_size;
    return write_structure(out_stats, stats);
}

SaccadeDeviceInfo Backend::device_info() const noexcept {
    SaccadeDeviceInfo info{};
    info.struct_size = static_cast<uint32_t>(sizeof(info));
    info.api_version = SACCADE_API_VERSION;
    info.stable_id = device_id;
    info.capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT;
    info.format_bits = SACCADE_FORMAT_BGRA8 | SACCADE_FORMAT_RGBA8 | SACCADE_FORMAT_BGRX8 | SACCADE_FORMAT_R8;
    info.precision_bits = SACCADE_PRECISION_FP32;
    info.import_bits = SACCADE_IMPORT_HOST;
    info.queue_capacity = 8;
    info.max_in_flight = 8;
    info.host_alignment = 1;
    info.device_alignment = 0;
    info.name = literal_span("scalar reference CPU");
    return info;
}

SaccadeInferenceProviderDesc Backend::provider() noexcept {
    SaccadeInferenceOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = detail::guarded_callback<&Impl::enumerate_devices>;
    ops.query_model = detail::guarded_callback<&Impl::query_model>;
    ops.create_model = detail::guarded_callback<&Impl::create_model>;
    ops.destroy_model = detail::guarded_callback<&Impl::destroy_model>;
    ops.create_context = detail::guarded_callback<&Impl::create_context>;
    ops.destroy_context = detail::guarded_callback<&Impl::destroy_context>;
    ops.submit = detail::guarded_callback<&Impl::submit>;
    ops.poll = detail::guarded_callback<&Impl::poll>;
    ops.wait = detail::guarded_callback<&Impl::wait>;
    ops.collect = detail::guarded_callback<&Impl::collect>;
    ops.cancel = detail::guarded_callback<&Impl::cancel>;
    ops.reset = detail::guarded_callback<&Impl::reset>;
    ops.synchronize = detail::guarded_callback<&Impl::synchronize>;
    ops.memory_stats = detail::guarded_callback<&Impl::memory_stats>;

    SaccadeProviderInfo info{};
    info.struct_size = static_cast<uint32_t>(sizeof(info));
    info.api_version = SACCADE_API_VERSION;
    info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    info.capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT |
                           SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    info.stable_id = provider_id;
    info.name = literal_span("scalar reference inference");

    SaccadeInferenceProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = info;
    desc.context = this;
    desc.ops = ops;
    return desc;
}

} // namespace saccade::backend::reference_cpu

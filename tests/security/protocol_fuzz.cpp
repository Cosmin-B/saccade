#include "agent/service.hpp"
#include "application/settings.hpp"
#include "backend/registry.hpp"
#include "input/plan.hpp"
#include "model/artifact.hpp"
#include "overlay/packet.hpp"
#include "scene/packet.hpp"

#include <saccade/saccade_agent.h>
#include <saccade/saccade_input.h>
#include <saccade/saccade_overlay.h>
#include <saccade/saccade_scene.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr size_t maximum_case_bytes = 4096;
constexpr uint32_t random_case_count = 200000;
constexpr uint64_t random_seed = UINT64_C(0x9E3779B97F4A7C15);
constexpr SaccadeAgentCapabilityBits agent_capabilities =
    SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER | SACCADE_AGENT_CAPABILITY_KEYBOARD |
    SACCADE_AGENT_CAPABILITY_WINDOW;

enum class Boundary : uint8_t { scene, overlay, input, artifact, settings, agent_observe, agent_query, agent_action };

enum class TestResult : int {
    success = 0,
    fixture_invalid,
    service_initialization_failed,
    scene_seed_rejected,
    overlay_seed_rejected,
    input_seed_rejected,
    artifact_seed_rejected,
    settings_seed_rejected,
    agent_seed_rejected,
    scene_view_out_of_bounds,
    overlay_view_out_of_bounds,
    input_view_out_of_bounds,
    artifact_view_out_of_bounds,
    settings_roundtrip_failed,
    agent_output_out_of_bounds,
    service_shutdown_failed
};

struct Case {
    std::array<uint8_t, maximum_case_bytes> bytes{};
    size_t size = 0;
    Boundary boundary = Boundary::scene;
};

static_assert(alignof(Case) >= alignof(SaccadeTargetPacketHeader));
static_assert(alignof(Case) >= alignof(SaccadeInputPlanHeader));

struct AgentFixture {
    saccade::scene::PacketView scene{};
    saccade::application::InteractionState state{};
    SaccadeAgentPhysicalState physical{};
};

class Random final {
  public:
    explicit Random(uint64_t seed) noexcept : state_(seed) {}

    uint64_t next() noexcept {
        uint64_t value = state_;
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        state_ = value;
        return value * UINT64_C(0x2545F4914F6CDD1D);
    }

    size_t bounded(size_t bound) noexcept { return bound == 0 ? 0 : static_cast<size_t>(next() % bound); }

  private:
    uint64_t state_;
};

template <class Record> Record* record_at(Case* value, size_t offset = 0) noexcept {
    return reinterpret_cast<Record*>(value->bytes.data() + offset);
}

void write_u32(uint8_t* bytes, size_t offset, uint32_t value) noexcept {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

void write_u64(uint8_t* bytes, size_t offset, uint64_t value) noexcept {
    for (uint32_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void make_scene(Case* value) noexcept {
    value->boundary = Boundary::scene;
    value->size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord) + 4U;

    auto* header = record_at<SaccadeTargetPacketHeader>(value);
    header->struct_size = sizeof(*header);
    header->packet_version = SACCADE_TARGET_PACKET_VERSION;
    header->target_count = 1;
    header->target_stride = sizeof(SaccadeTargetRecord);
    header->coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header->scene_epoch = 1;
    header->frame_id = 2;
    header->model_epoch = 3;
    header->session_epoch = 4;
    header->transform_epoch = 5;
    header->topology_epoch = 6;
    header->source_id = 7;
    header->targets_offset = sizeof(*header);
    header->total_size = value->size;

    auto* target = record_at<SaccadeTargetRecord>(value, sizeof(*header));
    target->target_id = 1;
    target->window_id = 2;
    target->display_id = 3;
    target->x_q8 = 256;
    target->y_q8 = 512;
    target->width_q8 = 1024;
    target->height_q8 = 512;
    target->safe_x_q8 = 512;
    target->safe_y_q8 = 768;
    target->confidence_q16 = UINT16_MAX;
    target->role = SACCADE_TARGET_ROLE_BUTTON;
    target->source_bits = SACCADE_TARGET_SOURCE_NEURAL;
    target->capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    target->flags = SACCADE_TARGET_ACTIONABLE;
    target->text.size = 4;
    std::memcpy(value->bytes.data() + sizeof(*header) + sizeof(*target), "Open", 4);
}

void make_overlay(Case* value) noexcept {
    value->boundary = Boundary::overlay;
    value->size = sizeof(SaccadeOverlayPacketHeader) + sizeof(SaccadeOverlayTarget) + sizeof(SaccadeOverlayStyle);

    auto* header = record_at<SaccadeOverlayPacketHeader>(value);
    header->struct_size = sizeof(*header);
    header->packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header->target_count = 1;
    header->target_stride = sizeof(SaccadeOverlayTarget);
    header->style_count = 1;
    header->style_stride = sizeof(SaccadeOverlayStyle);
    header->scene_epoch = 1;
    header->transform_epoch = 2;
    header->targets_offset = sizeof(*header);
    header->styles_offset = sizeof(*header) + sizeof(SaccadeOverlayTarget);

    auto* target = record_at<SaccadeOverlayTarget>(value, header->targets_offset);
    target->target_id = 1;
    target->x_q3 = 8;
    target->y_q3 = 16;
    target->width_q3 = 80;
    target->height_q3 = 40;
    target->label_x_q3 = 8;
    target->label_y_q3 = 64;
    target->confidence_q16 = UINT16_MAX;
    target->glyphs[0] = 1;
    target->glyph_count = 1;

    auto* style = record_at<SaccadeOverlayStyle>(value, header->styles_offset);
    style->target_stroke_q3 = 1;
    style->label_height_q3 = 16;
    style->label_padding_x_q3 = 2;
    style->glyph_width_q3 = 6;
    style->glyph_height_q3 = 8;
    style->glyph_advance_q3 = 7;
    style->active_stroke_q3 = 1;
}

void make_input_plan(Case* value) noexcept {
    value->boundary = Boundary::input;
    value->size = sizeof(SaccadeInputPlanHeader) + sizeof(SaccadeInputCommand);

    auto* header = record_at<SaccadeInputPlanHeader>(value);
    header->struct_size = sizeof(*header);
    header->plan_version = SACCADE_INPUT_PLAN_VERSION;
    header->command_count = 1;
    header->command_stride = sizeof(SaccadeInputCommand);
    header->required_permissions = SACCADE_INPUT_PERMISSION_POINTER;
    header->plan_id = 1;
    header->scene_epoch = 2;
    header->frame_id = 3;
    header->model_epoch = 4;
    header->session_epoch = 5;
    header->transform_epoch = 6;
    header->topology_epoch = 7;
    header->permission_epoch = 8;
    header->source_id = 9;
    header->deadline_ns = 10;
    header->commands_offset = sizeof(*header);
    header->total_size = value->size;

    auto* command = record_at<SaccadeInputCommand>(value, sizeof(*header));
    command->kind = SACCADE_INPUT_COMMAND_CLICK;
    command->target_id = 1;
    command->data0 = SACCADE_INPUT_BUTTON_LEFT;
    command->data1 = 1;
}

void make_artifact(Case* value) noexcept {
    value->boundary = Boundary::artifact;
    value->size = saccade::model::artifact_header_bytes + 1U;
    uint8_t* bytes = value->bytes.data();

    std::memcpy(bytes, "SCMD", 4);
    write_u32(bytes, 4, saccade::model::artifact_version);
    write_u32(bytes, 8, saccade::model::artifact_header_bytes);
    write_u32(bytes, 12, static_cast<uint32_t>(value->size));
    write_u64(bytes, 16, 1);
    write_u32(bytes, 24, static_cast<uint32_t>(saccade::model::GraphKind::ui_detector));
    write_u32(bytes, 28, static_cast<uint32_t>(saccade::model::ArtifactKind::fixed_graph));
    write_u32(bytes, 32, 16);
    write_u32(bytes, 36, 1280);
    write_u32(bytes, 40, 768);
    write_u32(bytes, 44, 3);
    write_u32(bytes, 48, 1);
    write_u32(bytes, 52, sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord));
    write_u64(bytes, 56, saccade::model::artifact_header_bytes);
    write_u64(bytes, 64, 1);
    write_u64(bytes, 88, 1);
    bytes[saccade::model::artifact_header_bytes] = 1;
}

bool make_settings(Case* value) noexcept {
    value->boundary = Boundary::settings;
    const saccade::application::SettingsDocument settings = saccade::application::default_settings();
    return saccade::application::encode_settings(settings, {value->bytes.data(), value->bytes.size()}, &value->size) ==
           SACCADE_OK;
}

void make_agent_observe(Case* value) noexcept {
    value->boundary = Boundary::agent_observe;
    value->size = sizeof(SaccadeAgentObserveRequest);
    auto* request = record_at<SaccadeAgentObserveRequest>(value);
    request->header.struct_size = sizeof(*request);
    request->header.api_version = SACCADE_AGENT_API_VERSION;
    request->header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request->request_id = 1;
    request->scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    request->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    request->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    request->maximum_targets = 1;
    request->target_stride = sizeof(SaccadeAgentTarget);
    request->total_capacity = maximum_case_bytes;
}

void make_agent_query(Case* value) noexcept {
    value->boundary = Boundary::agent_query;
    value->size = sizeof(SaccadeAgentQueryRequest);
    auto* request = record_at<SaccadeAgentQueryRequest>(value);
    request->header.struct_size = sizeof(*request);
    request->header.api_version = SACCADE_AGENT_API_VERSION;
    request->header.message_kind = SACCADE_AGENT_MESSAGE_QUERY_REQUEST;
    request->request_id = 2;
    request->scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    request->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    request->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    request->maximum_results = 1;
    request->filter_stride = sizeof(SaccadeAgentQueryFilter);
    request->filters_offset = sizeof(*request);
    request->total_size = sizeof(*request);
}

void make_agent_action(Case* value) noexcept {
    value->boundary = Boundary::agent_action;
    value->size = sizeof(SaccadeAgentActionBatch) + sizeof(SaccadeAgentAction);
    auto* request = record_at<SaccadeAgentActionBatch>(value);
    request->header.struct_size = sizeof(*request);
    request->header.api_version = SACCADE_AGENT_API_VERSION;
    request->header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_BATCH;
    request->request_id = 3;
    request->requested_capability_bits = SACCADE_AGENT_CAPABILITY_POINTER;
    request->policy = SACCADE_AGENT_BATCH_STOP_ON_FAILURE;
    request->deadline_ns = 2;
    request->action_count = 1;
    request->action_stride = sizeof(SaccadeAgentAction);
    request->actions_offset = sizeof(*request);
    request->payload_offset = static_cast<uint32_t>(value->size);
    request->total_size = static_cast<uint32_t>(value->size);
    auto* action = record_at<SaccadeAgentAction>(value, sizeof(*request));
    action->kind = SACCADE_AGENT_ACTION_ABORT;
}

bool contains(SaccadeSpanU8 bytes, const void* pointer, size_t size) noexcept {
    if (pointer == nullptr) return size == 0;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(bytes.data);
    const uintptr_t current = reinterpret_cast<uintptr_t>(pointer);
    if (bytes.size > std::numeric_limits<uintptr_t>::max() - begin) return false;
    const uintptr_t end = begin + bytes.size;
    return current >= begin && current <= end && size <= end - current;
}

SaccadeResult acquire_scene(void* context, saccade::scene::PacketView* output) noexcept {
    *output = static_cast<AgentFixture*>(context)->scene;
    return SACCADE_OK;
}

SaccadeResult read_state(void* context, saccade::application::InteractionState* output) noexcept {
    *output = static_cast<AgentFixture*>(context)->state;
    return SACCADE_OK;
}

SaccadeResult execute_plan(void*, SaccadeSpanU8, uint32_t, uint64_t) noexcept {
    return SACCADE_OK;
}

SaccadeResult read_physical_state(void* context, SaccadeAgentPhysicalState* output) noexcept {
    *output = static_cast<AgentFixture*>(context)->physical;
    return SACCADE_OK;
}

SaccadeResult abort_input(void*) noexcept {
    return SACCADE_OK;
}

SaccadeResult cycle_window(void*, bool) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_execute(void*, const SaccadeInputPlanDesc*, SaccadeTicketHandle*) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_poll(void*, SaccadeTicketHandle, SaccadeInputStatus*) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_wait(void*, SaccadeTicketHandle, uint64_t, SaccadeInputStatus*) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_cancel(void*, SaccadeTicketHandle) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_release_all(void*) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_synchronize(void*, uint64_t) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_reset(void*) noexcept {
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL input_memory_stats(void*, SaccadeMemoryStats*) noexcept {
    return SACCADE_OK;
}

uint32_t sample_u32(SaccadeSpanU8 bytes, size_t offset) noexcept {
    uint32_t value = 0;
    for (uint32_t index = 0; index < 4 && offset + index < bytes.size; ++index) {
        value |= static_cast<uint32_t>(bytes.data[offset + index]) << (index * 8U);
    }
    return value;
}

void fuzz_registry(SaccadeSpanU8 bytes, saccade::backend::ProviderRegistry* registry) noexcept {
    static constexpr std::array<uint8_t, 64> name{'p', 'r', 'o', 't', 'o', 'c', 'o', 'l', '-', 'f', 'u',
                                                  'z', 'z', '-', 'p', 'r', 'o', 'v', 'i', 'd', 'e', 'r'};

    SaccadeInputProviderDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.info.struct_size = sizeof(desc.info);
    desc.info.api_version = SACCADE_API_VERSION;
    desc.info.family = SACCADE_PROVIDER_FAMILY_INPUT;
    desc.info.stable_id = static_cast<uint64_t>(sample_u32(bytes, 8)) + 1U;
    desc.info.name = {name.data(), 22};
    desc.ops.struct_size = sizeof(desc.ops);
    desc.ops.api_version = SACCADE_API_VERSION;
    desc.ops.execute = input_execute;
    desc.ops.poll = input_poll;
    desc.ops.wait = input_wait;
    desc.ops.cancel = input_cancel;
    desc.ops.release_all = input_release_all;
    desc.ops.synchronize = input_synchronize;
    desc.ops.reset = input_reset;
    desc.ops.memory_stats = input_memory_stats;

    const uint32_t selector = sample_u32(bytes, 0) % 14U;
    const uint32_t value = sample_u32(bytes, 4);
    switch (selector) {
    case 0:
        desc.struct_size = value;
        break;
    case 1:
        desc.api_version = value;
        break;
    case 2:
        desc.info.struct_size = value;
        break;
    case 3:
        desc.info.api_version = value;
        break;
    case 4:
        desc.info.family = value;
        break;
    case 5:
        desc.info.stable_id = value;
        break;
    case 6:
        desc.info.name.size = value;
        break;
    case 7:
        desc.info.reserved[value % std::size(desc.info.reserved)] = value;
        break;
    case 8:
        desc.ops.struct_size = value;
        break;
    case 9:
        desc.ops.api_version = value;
        break;
    case 10:
        desc.ops.reserved[value % std::size(desc.ops.reserved)] = value;
        break;
    case 11:
        desc.reserved[value % std::size(desc.reserved)] = value;
        break;
    case 12:
        desc.ops.execute = value == 0 ? nullptr : input_execute;
        break;
    case 13:
        desc.info.capability_bits = value;
        break;
    }
    (void)registry->register_input(&desc, nullptr);
}

TestResult exercise(SaccadeSpanU8 bytes, saccade::agent::Service* service, saccade::backend::ProviderRegistry* registry,
                    std::array<uint8_t, maximum_case_bytes>* output) noexcept {
    saccade::scene::PacketView scene{};
    if (saccade::scene::validate_packet(bytes, &scene) == SACCADE_OK) {
        const size_t target_bytes = static_cast<size_t>(scene.header->target_count) * sizeof(SaccadeTargetRecord);
        if (!contains(bytes, scene.header, sizeof(*scene.header)) || !contains(bytes, scene.targets, target_bytes) ||
            !contains(bytes, scene.text, scene.text_size) || scene.byte_size > bytes.size) {
            return TestResult::scene_view_out_of_bounds;
        }
    }

    saccade::overlay::PacketView overlay{};
    if (saccade::overlay::validate_packet(bytes, &overlay) == SACCADE_OK) {
        const size_t target_bytes = static_cast<size_t>(overlay.header.target_count) * sizeof(SaccadeOverlayTarget);
        const size_t style_bytes = static_cast<size_t>(overlay.header.style_count) * sizeof(SaccadeOverlayStyle);
        if (!contains(bytes, overlay.targets, target_bytes) || !contains(bytes, overlay.styles, style_bytes)) {
            return TestResult::overlay_view_out_of_bounds;
        }
        size_t instance_count = 0;
        const SaccadeResult expanded = saccade::overlay::expand_static(overlay, {}, &instance_count);
        if (overlay.header.target_count == 0 && expanded != SACCADE_OK) {
            return TestResult::overlay_view_out_of_bounds;
        }
    }

    saccade::input::PlanView plan{};
    if (saccade::input::validate_plan(bytes, &plan) == SACCADE_OK) {
        const size_t command_bytes = static_cast<size_t>(plan.header->command_count) * sizeof(SaccadeInputCommand);
        if (!contains(bytes, plan.header, sizeof(*plan.header)) || !contains(bytes, plan.commands, command_bytes) ||
            plan.byte_size > bytes.size) {
            return TestResult::input_view_out_of_bounds;
        }
    }

    saccade::model::ArtifactView artifact{};
    if (saccade::model::parse_artifact(bytes, &artifact) == SACCADE_OK) {
        if (!contains(bytes, artifact.payload.data, artifact.payload.size) ||
            !contains(bytes, artifact.signed_message.data, artifact.signed_message.size) ||
            !contains(bytes, artifact.signature.data, artifact.signature.size)) {
            return TestResult::artifact_view_out_of_bounds;
        }
    }

    saccade::application::SettingsDocument settings{};
    if (saccade::application::decode_settings(bytes, &settings) == SACCADE_OK) {
        size_t encoded_size = 0;
        if (saccade::application::validate_settings(settings) != SACCADE_OK ||
            saccade::application::encode_settings(settings, {output->data(), output->size()}, &encoded_size) !=
                SACCADE_OK ||
            encoded_size > output->size()) {
            return TestResult::settings_roundtrip_failed;
        }
    }

    fuzz_registry(bytes, registry);

    size_t output_size = 0;
    (void)service->process(bytes, agent_capabilities, 1, {output->data(), output->size()}, &output_size);
    if (output_size > output->size()) return TestResult::agent_output_out_of_bounds;
    return TestResult::success;
}

TestResult verify_seed(const Case& value, saccade::agent::Service* service,
                       std::array<uint8_t, maximum_case_bytes>* output) noexcept {
    const SaccadeSpanU8 bytes{value.bytes.data(), value.size};
    switch (value.boundary) {
    case Boundary::scene: {
        saccade::scene::PacketView view{};
        return saccade::scene::validate_packet(bytes, &view) == SACCADE_OK ? TestResult::success
                                                                           : TestResult::scene_seed_rejected;
    }
    case Boundary::overlay: {
        saccade::overlay::PacketView view{};
        return saccade::overlay::validate_packet(bytes, &view) == SACCADE_OK ? TestResult::success
                                                                             : TestResult::overlay_seed_rejected;
    }
    case Boundary::input: {
        saccade::input::PlanView view{};
        return saccade::input::validate_plan(bytes, &view) == SACCADE_OK ? TestResult::success
                                                                         : TestResult::input_seed_rejected;
    }
    case Boundary::artifact: {
        saccade::model::ArtifactView view{};
        return saccade::model::parse_artifact(bytes, &view) == SACCADE_OK ? TestResult::success
                                                                          : TestResult::artifact_seed_rejected;
    }
    case Boundary::settings: {
        saccade::application::SettingsDocument settings{};
        return saccade::application::decode_settings(bytes, &settings) == SACCADE_OK
                   ? TestResult::success
                   : TestResult::settings_seed_rejected;
    }
    case Boundary::agent_observe:
    case Boundary::agent_query:
    case Boundary::agent_action: {
        size_t output_size = 0;
        return service->process(bytes, agent_capabilities, 1, {output->data(), output->size()}, &output_size) ==
                       SACCADE_OK
                   ? TestResult::success
                   : TestResult::agent_seed_rejected;
    }
    }
    return TestResult::fixture_invalid;
}

TestResult fuzz_seed(const Case& seed, saccade::agent::Service* service, saccade::backend::ProviderRegistry* registry,
                     std::array<uint8_t, maximum_case_bytes>* working,
                     std::array<uint8_t, maximum_case_bytes>* output) noexcept {
    for (size_t size = 0; size <= seed.size; ++size) {
        const TestResult result = exercise({seed.bytes.data(), size}, service, registry, output);
        if (result != TestResult::success) return result;
    }

    for (size_t byte = 0; byte < seed.size; ++byte) {
        for (uint32_t bit = 0; bit < 8; ++bit) {
            std::memcpy(working->data(), seed.bytes.data(), seed.size);
            (*working)[byte] ^= static_cast<uint8_t>(UINT8_C(1) << bit);
            const TestResult result = exercise({working->data(), seed.size}, service, registry, output);
            if (result != TestResult::success) return result;
        }
    }
    return TestResult::success;
}

TestResult fuzz_random(const std::array<Case, 8>& seeds, saccade::agent::Service* service,
                       saccade::backend::ProviderRegistry* registry, std::array<uint8_t, maximum_case_bytes>* working,
                       std::array<uint8_t, maximum_case_bytes>* output) noexcept {
    Random random(random_seed);
    for (uint32_t iteration = 0; iteration < random_case_count; ++iteration) {
        const Case& seed = seeds[random.bounded(seeds.size())];
        size_t size = seed.size;
        if ((random.next() & 7U) == 0) {
            size = random.bounded(maximum_case_bytes + 1U);
            for (size_t index = 0; index < size; ++index) {
                (*working)[index] = static_cast<uint8_t>(random.next());
            }
        } else {
            std::memcpy(working->data(), seed.bytes.data(), seed.size);
            const size_t growth = random.bounded(65);
            size = seed.size + growth <= maximum_case_bytes ? seed.size + growth : maximum_case_bytes;
            for (size_t index = seed.size; index < size; ++index) {
                (*working)[index] = static_cast<uint8_t>(random.next());
            }
            const size_t mutations = 1U + random.bounded(16);
            for (size_t mutation = 0; mutation < mutations && size != 0; ++mutation) {
                (*working)[random.bounded(size)] = static_cast<uint8_t>(random.next());
            }
            if ((random.next() & 3U) == 0) size = random.bounded(size + 1U);
        }

        const TestResult result = exercise({working->data(), size}, service, registry, output);
        if (result != TestResult::success) return result;
    }
    return TestResult::success;
}

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    static std::array<Case, 8> seeds{};
    make_scene(&seeds[0]);
    make_overlay(&seeds[1]);
    make_input_plan(&seeds[2]);
    make_artifact(&seeds[3]);
    if (!make_settings(&seeds[4])) return result(TestResult::fixture_invalid);
    make_agent_observe(&seeds[5]);
    make_agent_query(&seeds[6]);
    make_agent_action(&seeds[7]);

    static AgentFixture fixture{};
    if (saccade::scene::validate_packet({seeds[0].bytes.data(), seeds[0].size}, &fixture.scene) != SACCADE_OK) {
        return result(TestResult::fixture_invalid);
    }
    fixture.state.scene_epoch = fixture.scene.header->scene_epoch;
    fixture.state.transform_epoch = fixture.scene.header->transform_epoch;
    fixture.state.topology_epoch = fixture.scene.header->topology_epoch;
    fixture.state.permission_epoch = 1;
    fixture.state.focus_id = 2;
    fixture.state.window_id = 2;
    fixture.state.display_id = 3;
    fixture.state.permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD |
                                SACCADE_INPUT_PERMISSION_TEXT | SACCADE_INPUT_PERMISSION_WINDOW;
    fixture.physical.permission_epoch = fixture.state.permission_epoch;

    saccade::agent::Service service;
    if (service.initialize({&fixture, acquire_scene, read_state, execute_plan, read_physical_state, abort_input,
                            cycle_window, agent_capabilities}) != SACCADE_OK) {
        return result(TestResult::service_initialization_failed);
    }

    saccade::backend::ProviderRegistry registry;
    alignas(64) static std::array<uint8_t, maximum_case_bytes> working{};
    alignas(64) static std::array<uint8_t, maximum_case_bytes> output{};
    for (const Case& seed : seeds) {
        TestResult fuzz_result = verify_seed(seed, &service, &output);
        if (fuzz_result == TestResult::success) {
            fuzz_result = fuzz_seed(seed, &service, &registry, &working, &output);
        }
        if (fuzz_result != TestResult::success) return result(fuzz_result);
    }

    const TestResult fuzz_result = fuzz_random(seeds, &service, &registry, &working, &output);
    if (fuzz_result != TestResult::success) return result(fuzz_result);
    return service.shutdown() == SACCADE_OK ? result(TestResult::success) : result(TestResult::service_shutdown_failed);
}

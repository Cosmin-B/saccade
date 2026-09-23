// Test-only AgentClient transport. The production MCP server and native service
// execute unchanged; plans affect this bounded form state instead of the desktop.
#include "agent_client.hpp"
#include "agent/service.hpp"
#include "input/plan.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>

namespace {

constexpr uint32_t target_count = 4;
constexpr size_t text_capacity = 128;
constexpr size_t packet_capacity = sizeof(SaccadeTargetPacketHeader) + target_count * sizeof(SaccadeTargetRecord) + text_capacity;
constexpr SaccadeAgentCapabilityBits capabilities = SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER |
                                                    SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_WINDOW;

struct Form {
    alignas(8) std::array<uint8_t, packet_capacity> packet{};
    std::array<char, 33> name{};
    size_t name_size = 0;
    bool saved = false;
    uint64_t generation = 100;
    saccade::scene::PacketView scene{};
    saccade::interaction::InteractionState state{};
    SaccadeAgentPhysicalState physical{};
};

Form form;
saccade::agent::Service service;
bool connected = false;

void publish() noexcept {
    auto* header = reinterpret_cast<SaccadeTargetPacketHeader*>(form.packet.data());
    *header = {};
    header->struct_size = sizeof(*header);
    header->packet_version = SACCADE_TARGET_PACKET_VERSION;
    header->target_count = target_count;
    header->target_stride = sizeof(SaccadeTargetRecord);
    header->coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    header->scene_epoch = form.generation;
    header->frame_id = form.generation;
    header->capture_time_ns = saccade::tools::monotonic_time_ns();
    header->model_epoch = 1;
    header->session_epoch = 1;
    header->transform_epoch = 3;
    header->topology_epoch = 5;
    header->source_id = 1;
    header->targets_offset = sizeof(*header);
    auto* targets = reinterpret_cast<SaccadeTargetRecord*>(form.packet.data() + sizeof(*header));
    auto* text = form.packet.data() + sizeof(*header) + target_count * sizeof(*targets);
    std::array<char, 48> status{};
    const std::string_view prefix = form.saved ? "Saved: " : form.name_size != 0 ? "Entered: " : "Ready";
    std::memcpy(status.data(), prefix.data(), prefix.size());
    std::memcpy(status.data() + prefix.size(), form.name.data(), form.name_size);
    const std::array<std::string_view, target_count> labels{"Name", "Save", "Cancel",
                                                            std::string_view(status.data(), prefix.size() + form.name_size)};
    uint32_t text_size = 0;
    for (uint32_t index = 0; index < target_count; ++index) {
        auto& target = targets[index];
        target = {};
        target.target_id = 11 + index;
        target.window_id = 202;
        target.display_id = 303;
        target.x_q8 = 256 + static_cast<int32_t>(index) * 4096;
        target.y_q8 = 256;
        target.width_q8 = 2560;
        target.height_q8 = 2560;
        target.safe_x_q8 = target.x_q8 + 1280;
        target.safe_y_q8 = target.y_q8 + 1280;
        target.confidence_q16 = 65535;
        target.role = static_cast<SaccadeTargetRole>(index == 0   ? SACCADE_TARGET_ROLE_TEXT_FIELD
                                                     : index == 3 ? SACCADE_TARGET_ROLE_TEXT
                                                                  : SACCADE_TARGET_ROLE_BUTTON);
        target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
        target.flags = index == 3 ? 0U : static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE);
        target.capability_bits = index == 0   ? static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_TEXT)
                                 : index == 3 ? 0U
                                              : static_cast<uint32_t>(SACCADE_TARGET_CAPABILITY_BUTTON);
        target.text = {static_cast<uint16_t>(text_size), static_cast<uint16_t>(labels[index].size())};
        std::memcpy(text + text_size, labels[index].data(), labels[index].size());
        text_size += static_cast<uint32_t>(labels[index].size());
    }
    header->total_size = static_cast<uint32_t>(sizeof(*header) + target_count * sizeof(*targets) + text_size);
    form.scene = {header, targets, header->total_size, text, text_size};
    form.state.scene_epoch = form.generation;
    form.state.transform_epoch = 3;
    form.state.topology_epoch = 5;
    form.state.permission_epoch = 4;
    form.state.process_id = 101;
    form.state.foreground_process_id = 101;
    form.state.focus_id = 101;
    form.state.window_id = 202;
    form.state.display_id = 303;
    form.state.window_bounds = {0, 0, 32768, 32768};
    form.state.permissions = SACCADE_INPUT_PERMISSION_POINTER | SACCADE_INPUT_PERMISSION_KEYBOARD | SACCADE_INPUT_PERMISSION_TEXT;
    form.physical.permission_epoch = 4;
    form.physical.physical_sequence = 77;
}

SaccadeResult acquire_scene(void*, const SaccadeAgentScope& scope, const SaccadeAgentFreshness& freshness,
                            saccade::scene::PacketView* scene, saccade::interaction::InteractionState* state) noexcept {
    if (scope.kind != SACCADE_AGENT_SCOPE_ACTIVE_WINDOW)
        return SACCADE_ERROR_UNSUPPORTED;
    if (freshness.policy == SACCADE_AGENT_FRESHNESS_AFTER_GENERATION && form.generation <= freshness.after_generation)
        return SACCADE_ERROR_BUSY;
    *scene = form.scene;
    *state = form.state;
    return SACCADE_OK;
}

SaccadeResult execute_plan(void*, SaccadeSpanU8 bytes, uint32_t permissions, uint64_t) noexcept {
    saccade::input::PlanView plan{};
    if ((permissions & ~form.state.permissions) != 0 || saccade::input::validate_plan(bytes, &plan) != SACCADE_OK ||
        plan.header->window_id != 202 || plan.header->display_id != 303)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if ((plan.header->flags & SACCADE_INPUT_PLAN_DRY_RUN) != 0)
        return SACCADE_OK;
    for (uint32_t index = 0; index < plan.header->command_count; ++index) {
        const auto& command = plan.commands[index];
        if (command.kind == SACCADE_INPUT_COMMAND_TEXT) {
            if (command.target_id != 11 || command.payload_size == 0 || command.payload_size >= form.name.size())
                return SACCADE_ERROR_INVALID_ARGUMENT;
            std::memcpy(form.name.data(), bytes.data + command.payload_offset, command.payload_size);
            form.name_size = command.payload_size;
            form.saved = false;
        } else if (command.kind == SACCADE_INPUT_COMMAND_CLICK && command.target_id == 12) {
            if (form.name_size == 0)
                return SACCADE_ERROR_STATE;
            form.saved = true;
        } else if (command.kind == SACCADE_INPUT_COMMAND_CLICK && command.target_id == 13) {
            form.name_size = 0;
            form.saved = false;
        }
    }
    ++form.generation;
    publish();
    return SACCADE_OK;
}

SaccadeResult read_physical(void*, SaccadeAgentPhysicalState* output) noexcept {
    *output = form.physical;
    return SACCADE_OK;
}

SaccadeResult abort_input(void*) noexcept {
    return SACCADE_OK;
}

SaccadeResult cycle_window(void*, bool) noexcept {
    return SACCADE_ERROR_UNSUPPORTED;
}

} // namespace

namespace saccade::tools {

uint64_t monotonic_time_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool AgentClient::connect() noexcept {
    if (connected)
        return true;
    publish();
    connected =
        service.initialize({&form, acquire_scene, execute_plan, read_physical, abort_input, cycle_window, capabilities}) == SACCADE_OK;
    return connected;
}

bool AgentClient::hello(SaccadeAgentCapabilityBits requested, AgentClientStorage*, SaccadeAgentCapabilityBits* granted) noexcept {
    *granted = requested & capabilities;
    return connected;
}

bool AgentClient::transact(const void* request, size_t size, AgentClientStorage* storage, size_t* response_size) noexcept {
    return connected && service.process({static_cast<const uint8_t*>(request), size}, capabilities, monotonic_time_ns(),
                                        {storage->response.data(), storage->response.size()}, response_size) == SACCADE_OK;
}

void AgentClient::close() noexcept {
    if (connected)
        (void)service.shutdown();
    connected = false;
}

AgentClient::~AgentClient() {
    close();
}

} // namespace saccade::tools

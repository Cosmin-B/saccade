#ifndef SACCADE_TESTS_SUPPORT_INPUT_PLAN_HPP
#define SACCADE_TESTS_SUPPORT_INPUT_PLAN_HPP

#include <saccade/saccade_backend.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace saccade::test {

struct alignas(8) InputPlanFixture {
    std::array<uint8_t, sizeof(SaccadeInputPlanHeader) + 3U * sizeof(SaccadeInputCommand)> bytes{};
    size_t size = 0;
};

inline InputPlanFixture input_plan(uint32_t command_count) noexcept {
    InputPlanFixture fixture{};
    SaccadeInputPlanHeader header{};
    header.struct_size = sizeof(header);
    header.plan_version = SACCADE_INPUT_PLAN_VERSION;
    header.command_count = command_count;
    header.command_stride = sizeof(SaccadeInputCommand);
    header.flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    header.required_permissions = SACCADE_INPUT_PERMISSION_POINTER;
    header.plan_id = 1;
    header.scene_epoch = 2;
    header.frame_id = 3;
    header.model_epoch = 4;
    header.session_epoch = 5;
    header.transform_epoch = 6;
    header.topology_epoch = 7;
    header.permission_epoch = 8;
    header.source_id = 9;
    header.deadline_ns = 10;
    header.commands_offset = sizeof(header);
    header.total_size = sizeof(header) + static_cast<uint64_t>(command_count) * sizeof(SaccadeInputCommand);
    std::memcpy(fixture.bytes.data(), &header, sizeof(header));
    auto* commands = reinterpret_cast<SaccadeInputCommand*>(fixture.bytes.data() + sizeof(header));
    for (uint32_t index = 0; index < command_count; ++index) {
        commands[index].kind = SACCADE_INPUT_COMMAND_CLICK;
        commands[index].target_id = index + 1U;
        commands[index].data0 = SACCADE_INPUT_BUTTON_LEFT;
        commands[index].data1 = 1;
    }
    fixture.size = static_cast<size_t>(header.total_size);
    return fixture;
}

inline SaccadeInputPlanDesc input_plan_desc(const InputPlanFixture& fixture) noexcept {
    SaccadeInputPlanDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.plan = {fixture.bytes.data(), fixture.size};
    return desc;
}

} // namespace saccade::test

#endif

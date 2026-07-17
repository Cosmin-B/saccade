#include "application/window_navigator.hpp"
#include "scene/packet.hpp"
#include "scene/windows.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int { success = 0, collect_failed, cycle_failed, direction_failed, scene_failed };

struct Fixture {
    std::array<SaccadeWindowInfo, 7> windows{};
};

SaccadeWindowInfo window(uint64_t id, uint64_t process, int32_t x, int32_t y) noexcept {
    static constexpr char title[] = "Window";
    SaccadeWindowInfo value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.stable_id = id;
    value.process_id = process;
    value.desktop_bounds = {x, y, 100, 80};
    value.title = {reinterpret_cast<const uint8_t*>(title), sizeof(title) - 1U};
    return value;
}

SaccadeResult enumerate(void* context, uint32_t index, SaccadeWindowInfo* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    if (output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    if (index >= fixture->windows.size()) return SACCADE_ERROR_NOT_FOUND;
    *output = fixture->windows[index];
    return SACCADE_OK;
}

} // namespace

int main() {
    Fixture fixture{};
    fixture.windows = {window(10, 1, 0, 0),   window(11, 2, -200, 0), window(12, 3, 200, 0),   window(13, 4, 0, -200),
                       window(14, 5, 0, 200), window(16, 6, 50, 20),  window(15, 99, 400, 400)};
    SaccadeAccessibilityProviderDesc provider{};
    provider.struct_size = sizeof(provider);
    provider.api_version = SACCADE_API_VERSION;
    provider.context = &fixture;
    provider.ops.enumerate_windows = enumerate;
    saccade::application::WindowNavigator navigator;
    saccade::application::WindowSnapshot snapshot{};
    if (navigator.collect(provider, 99, &snapshot) != SACCADE_OK || snapshot.count != 6)
        return static_cast<int>(TestResult::collect_failed);
    uint64_t selected = 0;
    if (navigator.cycle(snapshot, 10, true, &selected) != SACCADE_OK || selected != 16 ||
        navigator.cycle(snapshot, 10, false, &selected) != SACCADE_OK || selected != 16 ||
        navigator.behind(snapshot, 10, &selected) != SACCADE_OK || selected != 16)
        return static_cast<int>(TestResult::cycle_failed);
    constexpr std::array<saccade::application::WindowDirection, 4> directions{
        saccade::application::WindowDirection::left, saccade::application::WindowDirection::right,
        saccade::application::WindowDirection::up, saccade::application::WindowDirection::down};
    constexpr std::array<uint64_t, 4> expected{11, 16, 13, 16};
    for (uint32_t index = 0; index < directions.size(); ++index)
        if (navigator.directional(snapshot, 10, directions[index], &selected) != SACCADE_OK ||
            selected != expected[index])
            return static_cast<int>(TestResult::direction_failed);
    saccade::application::WindowSnapshot tied{};
    tied.windows[0] = window(100, 1, 0, 0);
    tied.windows[1] = window(102, 2, 200, 0);
    tied.windows[2] = window(101, 3, 200, 0);
    tied.count = 3;
    if (navigator.directional(tied, 100, saccade::application::WindowDirection::right, &selected) != SACCADE_OK ||
        selected != 101)
        return static_cast<int>(TestResult::direction_failed);
    constexpr size_t packet_capacity = sizeof(SaccadeTargetPacketHeader) + 6U * sizeof(SaccadeTargetRecord) + 6U * 6U;
    alignas(SaccadeTargetPacketHeader) std::array<uint8_t, packet_capacity> bytes{};
    saccade::scene::WindowSceneConfig config{};
    config.scene_epoch = 1;
    config.frame_id = 2;
    config.model_epoch = 3;
    config.session_epoch = 4;
    config.transform_epoch = 5;
    config.topology_epoch = 6;
    config.source_id = 7;
    size_t byte_size = 0;
    saccade::scene::PacketView scene{};
    if (saccade::scene::build_window_scene(config, snapshot.windows.data(), snapshot.count,
                                           {bytes.data(), bytes.size()}, &byte_size) != SACCADE_OK ||
        saccade::scene::validate_packet({bytes.data(), byte_size}, &scene) != SACCADE_OK ||
        scene.header->target_count != 6 || scene.targets[0].target_id != 10 || scene.targets[0].window_id != 10 ||
        scene.targets[0].role != SACCADE_TARGET_ROLE_WINDOW ||
        scene.targets[0].capability_bits != SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE ||
        scene.target_text(0).size != 6 || std::memcmp(scene.target_text(0).data, "Window", 6) != 0)
        return static_cast<int>(TestResult::scene_failed);
    return static_cast<int>(TestResult::success);
}

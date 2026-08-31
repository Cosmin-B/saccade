#include "scene/visual_target_tracker.hpp"

#include <array>
#include <cstdint>

namespace {

using saccade::scene::VisualTargetTracker;
using saccade::scene::VisualTargetTrackerConfig;
using saccade::scene::VisualTargetTrackerStats;
using saccade::scene::VisualTargetTrackerStorage;

constexpr uint64_t test_window_id = 41;
constexpr uint64_t test_display_id = 7;
constexpr uint64_t test_session_epoch = 3;

struct Packet {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, 8> targets{};
};

struct Fixture {
    VisualTargetTracker tracker{};
    VisualTargetTrackerStorage storage{};
    VisualTargetTrackerStats stats{};

    bool initialize(uint32_t maximum_missed_frames = 3) noexcept {
        VisualTargetTrackerConfig config{};
        config.maximum_missed_frames = maximum_missed_frames;
        return tracker.initialize(&storage, config) == SACCADE_OK;
    }

    bool remap(Packet* packet) noexcept {
        return tracker.remap(&packet->header, packet->targets.data(), &stats) == SACCADE_OK;
    }
};

enum class TestResult : int {
    success = 0,
    initialization_failed,
    motion_failed,
    crossing_failed,
    lifetime_failed,
    repeated_frame_failed,
    epoch_reset_failed,
    window_translation_failed,
    window_resize_failed,
    semantic_identity_failed
};

int32_t q8(int32_t value) noexcept {
    return value * 256;
}

SaccadeTargetRecord window_target(int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
    SaccadeTargetRecord target{};
    target.target_id = test_window_id;
    target.window_id = test_window_id;
    target.display_id = test_display_id;
    target.x_q8 = q8(x);
    target.y_q8 = q8(y);
    target.width_q8 = q8(width);
    target.height_q8 = q8(height);
    target.safe_x_q8 = q8(x + width / 2);
    target.safe_y_q8 = q8(y + height / 2);
    target.confidence_q16 = UINT16_MAX;
    target.role = SACCADE_TARGET_ROLE_WINDOW;
    target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
    target.capability_bits = SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
    target.flags = SACCADE_TARGET_ACTIONABLE;
    return target;
}

SaccadeTargetRecord visual_target(uint64_t incoming_id, int32_t x, int32_t y, int32_t width = 48,
                                  int32_t height = 32) noexcept {
    SaccadeTargetRecord target{};
    target.target_id = incoming_id;
    target.window_id = test_window_id;
    target.display_id = test_display_id;
    target.x_q8 = q8(x);
    target.y_q8 = q8(y);
    target.width_q8 = q8(width);
    target.height_q8 = q8(height);
    target.safe_x_q8 = q8(x + width / 2);
    target.safe_y_q8 = q8(y + height / 2);
    target.confidence_q16 = 50000;
    target.role = SACCADE_TARGET_ROLE_BUTTON;
    target.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
    target.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    target.flags = SACCADE_TARGET_ACTIONABLE;
    return target;
}

SaccadeTargetRecord semantic_target(uint64_t id, uint16_t source_bits, int32_t x) noexcept {
    SaccadeTargetRecord target = visual_target(id, x, 80);
    target.source_bits = source_bits;
    return target;
}

Packet packet(uint64_t frame_id, int32_t window_x, int32_t window_y, int32_t window_width,
              int32_t window_height) noexcept {
    Packet value{};
    value.header.struct_size = sizeof(value.header);
    value.header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    value.header.target_stride = sizeof(SaccadeTargetRecord);
    value.header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    value.header.scene_epoch = frame_id;
    value.header.frame_id = frame_id;
    value.header.session_epoch = test_session_epoch;
    value.header.transform_epoch = 1;
    value.header.topology_epoch = 1;
    value.header.source_id = 1;
    value.header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    value.targets[0] = window_target(window_x, window_y, window_width, window_height);
    value.header.target_count = 1;
    return value;
}

Packet publication(uint64_t scene_epoch, uint64_t frame_id, int32_t window_x, int32_t window_y, int32_t window_width,
                   int32_t window_height) noexcept {
    Packet value = packet(frame_id, window_x, window_y, window_width, window_height);
    value.header.scene_epoch = scene_epoch;
    return value;
}

void append(Packet* packet, const SaccadeTargetRecord& target) noexcept {
    packet->targets[packet->header.target_count++] = target;
    packet->header.total_size = sizeof(SaccadeTargetPacketHeader) +
                                static_cast<uint64_t>(packet->header.target_count) * sizeof(SaccadeTargetRecord);
}

bool motion_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = packet(1, 0, 0, 1000, 700);
    append(&first, visual_target(101, 100, 120));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t stable_id = first.targets[1].target_id;

    Packet second = packet(2, 0, 0, 1000, 700);
    append(&second, visual_target(202, 132, 120));
    return fixture.remap(&second) && second.targets[1].target_id == stable_id && fixture.stats.matched_targets == 1;
}

bool crossing_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = packet(1, 0, 0, 800, 500);
    append(&first, visual_target(101, 100, 160, 40, 30));
    append(&first, visual_target(102, 300, 160, 40, 30));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t rightward_id = first.targets[1].target_id;
    const uint64_t leftward_id = first.targets[2].target_id;

    Packet second = packet(2, 0, 0, 800, 500);
    append(&second, visual_target(201, 140, 160, 40, 30));
    append(&second, visual_target(202, 260, 160, 40, 30));
    if (!fixture.remap(&second)) {
        return false;
    }

    Packet third = packet(3, 0, 0, 800, 500);
    append(&third, visual_target(301, 180, 160, 40, 30));
    append(&third, visual_target(302, 220, 160, 40, 30));
    if (!fixture.remap(&third)) {
        return false;
    }

    Packet fourth = packet(4, 0, 0, 800, 500);
    append(&fourth, visual_target(401, 180, 160, 40, 30));
    append(&fourth, visual_target(402, 220, 160, 40, 30));
    return fixture.remap(&fourth) && fourth.targets[1].target_id == leftward_id &&
           fourth.targets[2].target_id == rightward_id;
}

bool lifetime_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize(2)) {
        return false;
    }

    Packet first = packet(1, 0, 0, 800, 500);
    append(&first, visual_target(101, 100, 100));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t initial_id = first.targets[1].target_id;

    Packet absent = packet(2, 0, 0, 800, 500);
    if (!fixture.remap(&absent)) {
        return false;
    }
    Packet returned = packet(3, 0, 0, 800, 500);
    append(&returned, visual_target(301, 108, 100));
    if (!fixture.remap(&returned) || returned.targets[1].target_id != initial_id) {
        return false;
    }

    for (uint64_t frame_id = 4; frame_id <= 6; ++frame_id) {
        Packet missing = packet(frame_id, 0, 0, 800, 500);
        if (!fixture.remap(&missing)) {
            return false;
        }
    }
    Packet appeared = packet(7, 0, 0, 800, 500);
    append(&appeared, visual_target(701, 116, 100));
    return fixture.remap(&appeared) && appeared.targets[1].target_id != initial_id;
}

bool repeated_frame_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = publication(1, 1, 0, 0, 800, 500);
    append(&first, visual_target(101, 100, 100));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t stable_id = first.targets[1].target_id;

    Packet second = publication(2, 1, 0, 0, 800, 500);
    append(&second, visual_target(201, 100, 100));
    if (!fixture.remap(&second) || second.targets[1].target_id != stable_id) {
        return false;
    }

    Packet stale = publication(2, 1, 0, 0, 800, 500);
    append(&stale, visual_target(301, 100, 100));
    return fixture.tracker.remap(&stale.header, stale.targets.data(), &fixture.stats) == SACCADE_ERROR_STALE_HANDLE;
}

bool epoch_reset_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = packet(1, 0, 0, 800, 500);
    first.header.model_epoch = 1;
    append(&first, visual_target(101, 100, 100));
    if (!fixture.remap(&first)) {
        return false;
    }

    Packet changed = packet(2, 0, 0, 800, 500);
    changed.header.model_epoch = 2;
    append(&changed, visual_target(201, 100, 100));
    return fixture.remap(&changed) && fixture.stats.created_tracks == 1 && fixture.stats.matched_targets == 0;
}

bool window_translation_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = packet(1, 100, 80, 800, 600);
    append(&first, visual_target(101, 260, 200, 80, 48));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t stable_id = first.targets[1].target_id;

    Packet translated = packet(2, 620, 310, 800, 600);
    append(&translated, visual_target(201, 780, 430, 80, 48));
    return fixture.remap(&translated) && translated.targets[1].target_id == stable_id;
}

bool window_resize_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    Packet first = packet(1, 0, 0, 800, 600);
    append(&first, visual_target(101, 160, 120, 80, 60));
    if (!fixture.remap(&first)) {
        return false;
    }
    const uint64_t stable_id = first.targets[1].target_id;

    Packet resized = packet(2, 0, 0, 1200, 900);
    append(&resized, visual_target(201, 240, 180, 120, 90));
    return fixture.remap(&resized) && resized.targets[1].target_id == stable_id;
}

bool semantic_identity_test() noexcept {
    Fixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    constexpr uint64_t accessibility_id = UINT64_C(0xA11CE);
    constexpr uint64_t fused_id = UINT64_C(0xF05ED);
    Packet value = packet(1, 0, 0, 800, 500);
    append(&value, semantic_target(accessibility_id, SACCADE_TARGET_SOURCE_ACCESSIBILITY, 80));
    append(&value, semantic_target(fused_id, SACCADE_TARGET_SOURCE_ACCESSIBILITY | SACCADE_TARGET_SOURCE_NEURAL, 160));
    append(&value, visual_target(303, 240, 80));
    return fixture.remap(&value) && value.targets[0].target_id == test_window_id &&
           value.targets[1].target_id == accessibility_id && value.targets[2].target_id == fused_id &&
           value.targets[3].target_id != 303 && fixture.stats.passthrough_targets == 3;
}

} // namespace

int main() {
    Fixture initialization{};
    if (!initialization.initialize()) {
        return static_cast<int>(TestResult::initialization_failed);
    }
    if (!motion_test()) {
        return static_cast<int>(TestResult::motion_failed);
    }
    if (!crossing_test()) {
        return static_cast<int>(TestResult::crossing_failed);
    }
    if (!lifetime_test()) {
        return static_cast<int>(TestResult::lifetime_failed);
    }
    if (!repeated_frame_test()) {
        return static_cast<int>(TestResult::repeated_frame_failed);
    }
    if (!epoch_reset_test()) {
        return static_cast<int>(TestResult::epoch_reset_failed);
    }
    if (!window_translation_test()) {
        return static_cast<int>(TestResult::window_translation_failed);
    }
    if (!window_resize_test()) {
        return static_cast<int>(TestResult::window_resize_failed);
    }
    if (!semantic_identity_test()) {
        return static_cast<int>(TestResult::semantic_identity_failed);
    }
    return static_cast<int>(TestResult::success);
}

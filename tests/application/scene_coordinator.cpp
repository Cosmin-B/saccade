#include "application/scene_coordinator.hpp"
#include "tests/support/allocation_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using saccade::application::SceneCoordinator;
using saccade::application::SceneCoordinatorAdvance;
using saccade::application::SceneCoordinatorConfig;
using saccade::application::SceneCoordinatorStorage;
using saccade::application::SceneSource;
using saccade::scene::PacketView;
using saccade::scene::SceneStore;
using saccade::scene::SceneStoreStorage;

enum class TestResult : int {
    success,
    store_initialization_failed,
    coordinator_initialization_failed,
    semantic_request_failed,
    neural_first_publish_failed,
    semantic_publish_failed,
    fused_publish_failed,
    fused_wait_failed,
    semantic_only_publish_failed,
    matching_neural_fusion_failed,
    stale_semantic_handling_failed,
    statistics_failed,
    status_failed,
    scope_filter_failed,
    fusion_setting_failed,
    semantic_freshness_failed,
    semantic_cancellation_failed,
    shutdown_failed,
    allocation_failed
};

struct PacketStorage {
    SaccadeTargetPacketHeader header{};
    std::array<SaccadeTargetRecord, 1> targets{};
};

constexpr size_t packet_size = sizeof(SaccadeTargetPacketHeader) + sizeof(SaccadeTargetRecord);

SaccadeTargetRecord target(uint64_t id, int32_t x, uint16_t source_bits) noexcept {
    SaccadeTargetRecord value{};
    value.target_id = id;
    value.window_id = 90;
    value.display_id = 80;
    value.x_q8 = x * 256;
    value.y_q8 = 100 * 256;
    value.width_q8 = 40 * 256;
    value.height_q8 = 20 * 256;
    value.safe_x_q8 = value.x_q8 + 20 * 256;
    value.safe_y_q8 = value.y_q8 + 10 * 256;
    value.confidence_q16 = 50'000;
    if (source_bits == SACCADE_TARGET_SOURCE_ACCESSIBILITY) {
        value.role = SACCADE_TARGET_ROLE_BUTTON;
    } else {
        value.role = SACCADE_TARGET_ROLE_UNKNOWN;
    }
    value.source_bits = source_bits;
    value.capability_bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE | SACCADE_TARGET_CAPABILITY_BUTTON;
    value.flags = SACCADE_TARGET_ACTIONABLE;
    return value;
}

void packet(PacketStorage* storage, uint64_t scene_epoch, uint64_t frame_id, uint64_t model_epoch,
            uint64_t session_epoch, uint64_t transform_epoch, uint64_t topology_epoch, uint64_t source_id,
            SaccadeTargetRecord value) noexcept {
    storage->header = {};
    storage->header.struct_size = sizeof(storage->header);
    storage->header.packet_version = SACCADE_TARGET_PACKET_VERSION;
    storage->header.target_count = 1;
    storage->header.target_stride = sizeof(SaccadeTargetRecord);
    storage->header.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
    storage->header.scene_epoch = scene_epoch;
    storage->header.frame_id = frame_id;
    storage->header.model_epoch = model_epoch;
    storage->header.session_epoch = session_epoch;
    storage->header.transform_epoch = transform_epoch;
    storage->header.topology_epoch = topology_epoch;
    storage->header.source_id = source_id;
    storage->header.targets_offset = sizeof(SaccadeTargetPacketHeader);
    storage->header.total_size = packet_size;
    storage->targets[0] = value;
}

SaccadeSpanU8 bytes(const PacketStorage& storage) noexcept {
    return {reinterpret_cast<const uint8_t*>(&storage), packet_size};
}

struct AccessibilityFixture {
    PacketStorage packet_{};
    SaccadeAccessibilityQueryDesc query_{};
    SaccadeTicketHandle ticket_ = 0;
    uint32_t running_polls_ = 0;
    uint32_t requests_ = 0;
    uint32_t releases_ = 0;
    bool active_ = false;
    bool incomplete_ = false;

    static AccessibilityFixture* from(void* context) noexcept { return static_cast<AccessibilityFixture*>(context); }

    static SaccadeResult SACCADE_CALL request(void* context, const SaccadeAccessibilityQueryDesc* query,
                                              SaccadeTicketHandle* output) noexcept {
        AccessibilityFixture* fixture = from(context);
        if (fixture == nullptr || query == nullptr || output == nullptr || fixture->active_)
            return SACCADE_ERROR_INVALID_ARGUMENT;
        fixture->query_ = *query;
        fixture->ticket_ = static_cast<SaccadeTicketHandle>(++fixture->requests_);
        fixture->active_ = true;
        packet(&fixture->packet_, fixture->ticket_, query->frame_id, 700, query->session_epoch, query->transform_epoch,
               query->topology_epoch, query->window_id,
               target(1000 + query->frame_id, 101, SACCADE_TARGET_SOURCE_ACCESSIBILITY));
        fixture->packet_.header.flags = fixture->incomplete_ ? SACCADE_TARGET_PACKET_INCOMPLETE : 0;
        *output = fixture->ticket_;
        return SACCADE_OK;
    }

    SaccadeAccessibilityStatus status(uint32_t state) const noexcept {
        SaccadeAccessibilityStatus value{};
        value.struct_size = sizeof(value);
        value.api_version = SACCADE_API_VERSION;
        value.state = state;
        value.result = state == SACCADE_TICKET_CANCELLED ? SACCADE_ERROR_CANCELLED : SACCADE_OK;
        value.ticket = ticket_;
        value.snapshot = state == SACCADE_TICKET_COMPLETE ? ticket_ + 100 : 0;
        value.session_epoch = query_.session_epoch;
        value.transform_epoch = query_.transform_epoch;
        value.target_count = 1;
        value.required_bytes = packet_size;
        value.topology_epoch = query_.topology_epoch;
        value.frame_id = query_.frame_id;
        return value;
    }

    static SaccadeResult SACCADE_CALL poll(void* context, SaccadeTicketHandle ticket,
                                           SaccadeAccessibilityStatus* output) noexcept {
        AccessibilityFixture* fixture = from(context);
        if (fixture == nullptr || output == nullptr || !fixture->active_ || ticket != fixture->ticket_)
            return SACCADE_ERROR_STALE_HANDLE;
        if (fixture->running_polls_ != 0) {
            --fixture->running_polls_;
            *output = fixture->status(SACCADE_TICKET_RUNNING);
        } else {
            *output = fixture->status(SACCADE_TICKET_COMPLETE);
        }
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL wait(void* context, SaccadeTicketHandle ticket, uint64_t,
                                           SaccadeAccessibilityStatus* output) noexcept {
        AccessibilityFixture* fixture = from(context);
        if (fixture == nullptr || output == nullptr || !fixture->active_ || ticket != fixture->ticket_)
            return SACCADE_ERROR_STALE_HANDLE;
        *output = fixture->status(SACCADE_TICKET_CANCELLED);
        fixture->active_ = false;
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL collect(void* context, SaccadeSnapshotHandle snapshot,
                                              SaccadeMutableSpanU8 output, size_t* required) noexcept {
        AccessibilityFixture* fixture = from(context);
        if (fixture == nullptr || required == nullptr || !fixture->active_ || snapshot != fixture->ticket_ + 100) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        *required = packet_size;
        if (output.data == nullptr || output.size < *required) {
            return SACCADE_ERROR_CAPACITY;
        }
        std::memcpy(output.data, &fixture->packet_, packet_size);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL cancel(void* context, SaccadeTicketHandle ticket) noexcept {
        AccessibilityFixture* fixture = from(context);
        return fixture != nullptr && fixture->active_ && ticket == fixture->ticket_ ? SACCADE_OK
                                                                                    : SACCADE_ERROR_STALE_HANDLE;
    }

    static SaccadeResult SACCADE_CALL release(void* context, SaccadeSnapshotHandle snapshot) noexcept {
        AccessibilityFixture* fixture = from(context);
        if (fixture == nullptr || !fixture->active_ || snapshot != fixture->ticket_ + 100) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        fixture->active_ = false;
        ++fixture->releases_;
        return SACCADE_OK;
    }

    SaccadeAccessibilityProviderDesc descriptor() noexcept {
        SaccadeAccessibilityProviderDesc value{};
        value.struct_size = sizeof(value);
        value.api_version = SACCADE_API_VERSION;
        value.context = this;
        value.ops.struct_size = sizeof(value.ops);
        value.ops.api_version = SACCADE_API_VERSION;
        value.ops.request = request;
        value.ops.poll = poll;
        value.ops.wait = wait;
        value.ops.collect = collect;
        value.ops.cancel = cancel;
        value.ops.release = release;
        return value;
    }
};

struct FreshnessFixture {
    bool current_ = true;

    static bool current(void* context, const SaccadeAccessibilityQueryDesc&) noexcept {
        return static_cast<FreshnessFixture*>(context)->current_;
    }
};

SaccadeAccessibilityQueryDesc query(uint64_t frame_id) noexcept {
    SaccadeAccessibilityQueryDesc value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.window_id = 90;
    value.scope = {0, 0, 1920, 1080};
    value.target_capacity = 256;
    value.session_epoch = 20;
    value.transform_epoch = 30;
    value.topology_epoch = 40;
    value.frame_id = frame_id;
    return value;
}

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    static SceneStoreStorage neural_storage;
    static SceneStoreStorage output_storage;
    static SceneCoordinatorStorage coordinator_storage;
    SceneStore neural_scenes;
    SceneStore output_scenes;
    if (neural_scenes.initialize(&neural_storage) != SACCADE_OK ||
        output_scenes.initialize(&output_storage) != SACCADE_OK) {
        return result(TestResult::store_initialization_failed);
    }
    AccessibilityFixture accessibility;
    FreshnessFixture freshness;
    SceneCoordinator coordinator;
    SceneCoordinatorConfig config{};
    config.neural_scenes = &neural_scenes;
    config.output_scenes = &output_scenes;
    config.accessibility = accessibility.descriptor();
    config.semantic_freshness = {&freshness, FreshnessFixture::current};
    if (coordinator.initialize(config, &coordinator_storage) != SACCADE_OK) {
        return result(TestResult::coordinator_initialization_failed);
    }
    saccade::scene::FusionConfig updated_fusion = config.fusion;
    updated_fusion.iou_threshold_q16 = 40000;
    if (coordinator.set_fusion(updated_fusion) != SACCADE_OK) return result(TestResult::fusion_setting_failed);
    updated_fusion.iou_threshold_q16 = 0;
    if (coordinator.set_fusion(updated_fusion) != SACCADE_ERROR_INVALID_ARGUMENT)
        return result(TestResult::fusion_setting_failed);

    saccade::test::begin_allocation_tracking();
    static PacketStorage neural;
    packet(&neural, 7, 10, 500, 20, 30, 40, 501, target(10, 100, SACCADE_TARGET_SOURCE_NEURAL));
    accessibility.running_polls_ = 1;
    if (neural_scenes.publish_copy(bytes(neural)) != SACCADE_OK ||
        coordinator.request_semantic(query(10)) != SACCADE_OK) {
        return result(TestResult::semantic_request_failed);
    }
    SceneCoordinatorAdvance advance{};
    if (coordinator.advance(&advance) != SACCADE_OK || !advance.scene_published || advance.semantic_collected ||
        advance.scene_epoch != 1 || advance.frame_id != 10 || advance.target_count != 1) {
        return result(TestResult::neural_first_publish_failed);
    }
    PacketView scene{};
    if (output_scenes.acquire_latest(&scene) != SACCADE_OK ||
        scene.targets[0].source_bits != SACCADE_TARGET_SOURCE_NEURAL) {
        return result(TestResult::neural_first_publish_failed);
    }

    if (coordinator.set_source(SceneSource::semantic) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        !advance.scene_published || !advance.semantic_collected || advance.scene_epoch != 2 ||
        advance.target_count != 1 || output_scenes.acquire_latest(&scene) != SACCADE_OK ||
        scene.targets[0].target_id != 1010 || scene.targets[0].source_bits != SACCADE_TARGET_SOURCE_ACCESSIBILITY) {
        return result(TestResult::semantic_publish_failed);
    }

    if (coordinator.set_source(SceneSource::fused) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        !advance.scene_published || advance.semantic_collected || advance.scene_epoch != 3 ||
        advance.target_count != 1 || output_scenes.acquire_latest(&scene) != SACCADE_OK ||
        scene.targets[0].target_id != 1010 ||
        scene.targets[0].source_bits != (SACCADE_TARGET_SOURCE_ACCESSIBILITY | SACCADE_TARGET_SOURCE_NEURAL)) {
        return result(TestResult::fused_publish_failed);
    }

    accessibility.incomplete_ = true;
    if (coordinator.set_source(SceneSource::semantic) != SACCADE_OK ||
        coordinator.request_semantic(query(11)) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        !advance.scene_published || !advance.semantic_collected || advance.scene_epoch != 4 || advance.frame_id != 11 ||
        advance.packet_flags != SACCADE_TARGET_PACKET_INCOMPLETE ||
        output_scenes.acquire_latest(&scene) != SACCADE_OK ||
        scene.targets[0].source_bits != SACCADE_TARGET_SOURCE_ACCESSIBILITY) {
        return result(TestResult::semantic_only_publish_failed);
    }

    if (coordinator.set_source(SceneSource::fused) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        advance.scene_published || advance.semantic_collected) {
        return result(TestResult::fused_wait_failed);
    }

    packet(&neural, 8, 11, 500, 20, 30, 40, 501, target(11, 100, SACCADE_TARGET_SOURCE_NEURAL));
    if (neural_scenes.publish_copy(bytes(neural)) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        !advance.scene_published || advance.semantic_collected || advance.scene_epoch != 5 || advance.frame_id != 11 ||
        advance.packet_flags != SACCADE_TARGET_PACKET_INCOMPLETE ||
        output_scenes.acquire_latest(&scene) != SACCADE_OK || scene.targets[0].target_id != 1011 ||
        scene.targets[0].source_bits != (SACCADE_TARGET_SOURCE_ACCESSIBILITY | SACCADE_TARGET_SOURCE_NEURAL)) {
        return result(TestResult::matching_neural_fusion_failed);
    }

    const auto scene_status = coordinator.status();
    if (scene_status.scene_epoch != 5 || scene_status.frame_id != 11 || scene_status.transform_epoch != 30 ||
        scene_status.topology_epoch != 40 || scene_status.source_id != 90 || scene_status.target_count != 1 ||
        scene_status.packet_flags != SACCADE_TARGET_PACKET_INCOMPLETE || scene_status.source != SceneSource::fused ||
        scene_status.semantic_running || !scene_status.semantic_available) {
        return result(TestResult::status_failed);
    }

    accessibility.incomplete_ = false;
    if (coordinator.request_semantic(query(9)) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        advance.scene_published || !advance.semantic_collected) {
        return result(TestResult::stale_semantic_handling_failed);
    }
    freshness.current_ = false;
    if (coordinator.request_semantic(query(12)) != SACCADE_OK || coordinator.advance(&advance) != SACCADE_OK ||
        advance.scene_published || advance.semantic_collected || coordinator.semantic_running()) {
        return result(TestResult::semantic_freshness_failed);
    }
    freshness.current_ = true;
    const auto stats = coordinator.stats();
    if (stats.advances != 8 || stats.semantic_requests != 4 || stats.semantic_completed != 3 ||
        stats.semantic_stale != 2 || stats.neural_updates != 2 || stats.fused_publications != 2 ||
        stats.single_source_publications != 3 || stats.targets_published != 5 || stats.semantic_incomplete != 1 ||
        stats.incomplete_publications != 2 || stats.text_truncated_publications != 0 || stats.failures != 0 ||
        accessibility.releases_ != 4) {
        return result(TestResult::statistics_failed);
    }
    const saccade::geometry::RectQ8 scope{1000 * 256, 1000 * 256, 100 * 256, 100 * 256};
    if (coordinator.set_source(SceneSource::pixel) != SACCADE_OK || coordinator.set_scope(&scope) != SACCADE_OK ||
        coordinator.advance(&advance) != SACCADE_OK || !advance.scene_published || advance.target_count != 0 ||
        output_scenes.acquire_latest(&scene) != SACCADE_OK || scene.header->target_count != 0)
        return result(TestResult::scope_filter_failed);
    accessibility.running_polls_ = 1;
    if (coordinator.request_semantic(query(13)) != SACCADE_OK || coordinator.cancel_semantic() != SACCADE_OK ||
        accessibility.active_ || coordinator.semantic_running()) {
        return result(TestResult::semantic_cancellation_failed);
    }
    accessibility.running_polls_ = 1;
    if (coordinator.request_semantic(query(14)) != SACCADE_OK || coordinator.shutdown() != SACCADE_OK ||
        accessibility.active_) {
        return result(TestResult::shutdown_failed);
    }
    if (saccade::test::end_allocation_tracking() != 0) {
        return result(TestResult::allocation_failed);
    }
    return result(TestResult::success);
}

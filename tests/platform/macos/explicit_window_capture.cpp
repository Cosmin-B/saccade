#include "platform/macos/explicit_window_capture.hpp"

#include <array>
#include <cstdint>

namespace {

using saccade::platform::macos::explicit_window_current_space;
using saccade::platform::macos::explicit_window_visible;
using saccade::platform::macos::ExplicitWindowCapture;
using saccade::platform::macos::ExplicitWindowIdentity;
using saccade::platform::macos::ExplicitWindowRetirementReason;
using saccade::platform::macos::NativeCapturedFrame;
using saccade::platform::macos::SceneCaptureFrame;

enum class TestResult : int {
    success,
    selection_failed,
    identity_failed,
    disappearance_failed,
    held_lease_failed,
    switching_failed,
    failure_cleanup_failed,
    repeated_session_failed,
};

constexpr uint32_t selected_flags = explicit_window_visible | explicit_window_current_space;
constexpr SaccadeRectI32 bounds_a{100, 200, 800, 600};
constexpr SaccadeRectI32 bounds_b{900, 100, 640, 480};
constexpr uint64_t display_source = UINT64_C(0x0100000000000007);
constexpr uint64_t source_a = UINT64_C(0x020000000000005b);
constexpr uint64_t source_b = UINT64_C(0x0200000000000021);

ExplicitWindowIdentity identity_a() noexcept {
    return {410, 91, source_a, bounds_a, selected_flags, 0};
}

ExplicitWindowIdentity identity_b() noexcept {
    return {512, 33, source_b, bounds_b, selected_flags, 0};
}

struct FakeProvider {
    struct Stream {
        uint64_t handle = 0;
        uint64_t source_id = 0;
        uint64_t frame = 0;
        bool active = false;
        bool running = false;
        bool leased = false;
    };

    std::array<SaccadeCaptureSourceInfo, 3> sources{};
    uint32_t source_count = 0;
    Stream stream{};
    uint64_t next_stream = 100;
    uint64_t next_frame = 1000;
    uint64_t created_source = 0;
    uint32_t created_queue_capacity = 0;
    uint32_t created_max_width = 0;
    uint32_t created_max_height = 0;
    uint32_t enumerations = 0;
    uint32_t creates = 0;
    uint32_t starts = 0;
    uint32_t stops = 0;
    uint32_t destroys = 0;
    uint32_t acquires = 0;
    uint32_t releases = 0;
    uint32_t native_reads = 0;
    SaccadeResult create_result = SACCADE_OK;
    SaccadeResult start_result = SACCADE_OK;
    SaccadeResult stop_result = SACCADE_OK;
    SaccadeResult destroy_result = SACCADE_OK;
    SaccadeResult acquire_result = SACCADE_OK;
    SaccadeResult release_result = SACCADE_OK;
    SaccadeResult native_result = SACCADE_OK;
};

SaccadeCaptureSourceInfo source(uint64_t stable_id, uint32_t kind, SaccadeRectI32 bounds) noexcept {
    SaccadeCaptureSourceInfo value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.stable_id = stable_id;
    value.kind = kind;
    value.desktop_bounds = bounds;
    return value;
}

void set_sources(FakeProvider* fake, bool include_a = true, bool include_b = true) noexcept {
    fake->source_count = 0;
    fake->sources[fake->source_count++] = source(display_source, SACCADE_CAPTURE_SOURCE_DISPLAY, {0, 0, 1920, 1080});
    if (include_a)
        fake->sources[fake->source_count++] = source(source_a, SACCADE_CAPTURE_SOURCE_WINDOW, bounds_a);
    if (include_b)
        fake->sources[fake->source_count++] = source(source_b, SACCADE_CAPTURE_SOURCE_WINDOW, bounds_b);
}

SaccadeResult SACCADE_CALL enumerate_sources(void* context, uint32_t index, SaccadeCaptureSourceInfo* output) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->enumerations;
    if (output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (index >= fake->source_count)
        return SACCADE_ERROR_NOT_FOUND;
    *output = fake->sources[index];
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL create_stream(void* context, const SaccadeCaptureStreamDesc* desc, SaccadeCaptureStreamHandle* output) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->creates;
    if (desc == nullptr || output == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    fake->created_source = desc->source_id;
    fake->created_queue_capacity = desc->queue_capacity;
    fake->created_max_width = desc->max_width;
    fake->created_max_height = desc->max_height;
    if (fake->create_result != SACCADE_OK)
        return fake->create_result;
    fake->stream = {fake->next_stream++, desc->source_id, 0, true, false, false};
    *output = fake->stream.handle;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL destroy_stream(void* context, SaccadeCaptureStreamHandle stream) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->destroys;
    if (!fake->stream.active || stream != fake->stream.handle)
        return SACCADE_ERROR_STALE_HANDLE;
    if (fake->destroy_result != SACCADE_OK)
        return fake->destroy_result;
    fake->stream = {};
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL start_stream(void* context, SaccadeCaptureStreamHandle stream) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->starts;
    if (!fake->stream.active || stream != fake->stream.handle)
        return SACCADE_ERROR_STALE_HANDLE;
    if (fake->start_result != SACCADE_OK)
        return fake->start_result;
    fake->stream.running = true;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL stop_stream(void* context, SaccadeCaptureStreamHandle stream) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->stops;
    if (!fake->stream.active || stream != fake->stream.handle)
        return SACCADE_ERROR_STALE_HANDLE;
    if (fake->stop_result != SACCADE_OK)
        return fake->stop_result;
    fake->stream.running = false;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL acquire_frame(void* context, SaccadeCaptureStreamHandle stream, uint64_t, SaccadeCapturedFrame* output) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->acquires;
    if (!fake->stream.active || !fake->stream.running || fake->stream.leased || stream != fake->stream.handle || output == nullptr)
        return SACCADE_ERROR_STATE;
    if (fake->acquire_result != SACCADE_OK)
        return fake->acquire_result;
    fake->stream.leased = true;
    fake->stream.frame = fake->next_frame++;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = SACCADE_API_VERSION;
    output->frame = fake->stream.frame;
    output->source_id = fake->stream.source_id;
    output->frame_id = fake->stream.frame + 10;
    output->transform_epoch = 70;
    output->timestamp_ns = 80;
    output->width = 320;
    output->height = 240;
    output->pixel_format = SACCADE_FORMAT_BGRA8;
    return SACCADE_OK;
}

SaccadeResult SACCADE_CALL release_frame(void* context, SaccadeCaptureStreamHandle stream, SaccadeFrameHandle frame) {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->releases;
    if (!fake->stream.active || !fake->stream.leased || stream != fake->stream.handle || frame != fake->stream.frame)
        return SACCADE_ERROR_STALE_HANDLE;
    if (fake->release_result != SACCADE_OK)
        return fake->release_result;
    fake->stream.leased = false;
    fake->stream.frame = 0;
    return SACCADE_OK;
}

SaccadeResult read_native_frame(void* context, SaccadeCaptureStreamHandle stream, SaccadeFrameHandle frame,
                                NativeCapturedFrame* output) noexcept {
    auto* fake = static_cast<FakeProvider*>(context);
    ++fake->native_reads;
    if (output == nullptr || stream != fake->stream.handle || frame != fake->stream.frame)
        return SACCADE_ERROR_STALE_HANDLE;
    if (fake->native_result != SACCADE_OK)
        return fake->native_result;
    *output = {reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000)),
               reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000)),
               reinterpret_cast<void*>(static_cast<uintptr_t>(0x3000)),
               17,
               0,
               SACCADE_FORMAT_BGRA8,
               320,
               240};
    return SACCADE_OK;
}

SaccadeCaptureProviderDesc descriptor(FakeProvider* fake) noexcept {
    SaccadeCaptureProviderDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    desc.context = fake;
    desc.ops.struct_size = sizeof(desc.ops);
    desc.ops.api_version = SACCADE_API_VERSION;
    desc.ops.enumerate_sources = enumerate_sources;
    desc.ops.create = create_stream;
    desc.ops.destroy = destroy_stream;
    desc.ops.start = start_stream;
    desc.ops.stop = stop_stream;
    desc.ops.acquire = acquire_frame;
    desc.ops.release = release_frame;
    return desc;
}

bool exact_selection_and_frames() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    SceneCaptureFrame frame{};
    const ExplicitWindowIdentity selected = identity_a();
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 640, 480) != SACCADE_OK ||
        capture.select(selected, 10) != SACCADE_OK || !capture.active() || capture.session_epoch() != 10 ||
        fake.created_source != source_a || fake.created_source == display_source || fake.created_queue_capacity != 3 ||
        fake.created_max_width != 640 || fake.created_max_height != 480 || fake.creates != 1 || fake.starts != 1 ||
        capture.acquire(&frame) != SACCADE_OK || frame.frame.source_id != source_a || frame.display_id != selected.window_id ||
        frame.topology_epoch != 10 || frame.frame.frame == 0 || frame.native.metal_texture == nullptr || fake.native_reads != 1 ||
        capture.acquire(&frame) != SACCADE_ERROR_BUSY || capture.release(frame) != SACCADE_OK ||
        capture.release(frame) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }
    return capture.shutdown() == SACCADE_OK && fake.stops == 1 && fake.destroys == 1;
}

bool wrong_identity_or_source_never_falls_back() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK)
        return false;

    ExplicitWindowIdentity wrong_source = identity_a();
    wrong_source.capture_source_id = display_source;
    if (capture.select(wrong_source, 20) != SACCADE_ERROR_STALE_HANDLE || fake.creates != 0)
        return false;
    ExplicitWindowIdentity wrong_bounds = identity_a();
    ++wrong_bounds.bounds.x;
    if (capture.select(wrong_bounds, 20) != SACCADE_ERROR_STALE_HANDLE || fake.creates != 0 ||
        capture.select(identity_a(), 20) != SACCADE_OK) {
        return false;
    }

    ExplicitWindowIdentity wrong_process = identity_a();
    ++wrong_process.process_id;
    return capture.synchronize(wrong_process) == SACCADE_ERROR_STALE_HANDLE && !capture.active() &&
           capture.retirement_reason() == ExplicitWindowRetirementReason::identity_changed && fake.stops == 1 && fake.destroys == 1 &&
           capture.shutdown() == SACCADE_OK;
}

bool disappearance_retires_stream() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK ||
        capture.select(identity_a(), 30) != SACCADE_OK) {
        return false;
    }
    set_sources(&fake, false, true);
    SceneCaptureFrame frame{};
    return capture.synchronize(identity_a()) == SACCADE_ERROR_NOT_FOUND && !capture.active() &&
           capture.retirement_reason() == ExplicitWindowRetirementReason::disappeared && fake.stops == 1 && fake.destroys == 1 &&
           capture.acquire(&frame) == SACCADE_ERROR_STALE_HANDLE && capture.shutdown() == SACCADE_OK;
}

bool held_frame_drains_before_retirement() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    SceneCaptureFrame frame{};
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK ||
        capture.select(identity_a(), 40) != SACCADE_OK || capture.acquire(&frame) != SACCADE_OK) {
        return false;
    }
    set_sources(&fake, false, true);
    SceneCaptureFrame blocked{};
    if (capture.synchronize(identity_a()) != SACCADE_ERROR_NOT_FOUND || capture.active() || fake.stops != 0 || fake.destroys != 0 ||
        capture.acquire(&blocked) != SACCADE_ERROR_STALE_HANDLE || capture.select(identity_b(), 41) != SACCADE_ERROR_BUSY ||
        capture.release(frame) != SACCADE_OK || fake.releases != 1 || fake.stops != 1 || fake.destroys != 1 ||
        capture.release(frame) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }
    return capture.select(identity_b(), 41) == SACCADE_OK && capture.retire(ExplicitWindowRetirementReason::replaced) == SACCADE_OK &&
           capture.shutdown() == SACCADE_OK;
}

bool switching_never_reuses_old_handles() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    SceneCaptureFrame frame_a{};
    SceneCaptureFrame frame_b{};
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK ||
        capture.select(identity_a(), 50) != SACCADE_OK || capture.acquire(&frame_a) != SACCADE_OK ||
        capture.release(frame_a) != SACCADE_OK || capture.select(identity_b(), 51) != SACCADE_ERROR_BUSY ||
        capture.retire(ExplicitWindowRetirementReason::replaced) != SACCADE_OK || capture.select(identity_b(), 51) != SACCADE_OK ||
        capture.acquire(&frame_b) != SACCADE_OK || capture.release(frame_a) != SACCADE_ERROR_STALE_HANDLE ||
        capture.release(frame_b) != SACCADE_OK || capture.retire(ExplicitWindowRetirementReason::replaced) != SACCADE_OK ||
        capture.select(identity_a(), 52) != SACCADE_OK || capture.release(frame_b) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }
    return fake.created_source == source_a && fake.creates == 3 && fake.starts == 3 && fake.stops == 2 && fake.destroys == 2 &&
           capture.shutdown() == SACCADE_OK;
}

bool start_and_stop_failures_are_recoverable() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK)
        return false;
    fake.start_result = SACCADE_ERROR_BACKEND;
    if (capture.select(identity_a(), 60) != SACCADE_ERROR_BACKEND || capture.active() || fake.destroys != 1)
        return false;
    fake.start_result = SACCADE_OK;
    if (capture.select(identity_a(), 60) != SACCADE_OK)
        return false;
    fake.stop_result = SACCADE_ERROR_BACKEND;
    if (capture.retire(ExplicitWindowRetirementReason::replaced) != SACCADE_ERROR_BACKEND || capture.active() ||
        capture.select(identity_b(), 61) != SACCADE_ERROR_BUSY || fake.destroys != 1) {
        return false;
    }
    fake.stop_result = SACCADE_OK;
    return capture.retire(ExplicitWindowRetirementReason::replaced) == SACCADE_OK && fake.destroys == 2 &&
           capture.select(identity_b(), 61) == SACCADE_OK && capture.shutdown() == SACCADE_OK;
}

bool consumed_frame_is_not_retried_when_deferred_teardown_fails() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    SceneCaptureFrame frame{};
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK ||
        capture.select(identity_a(), 65) != SACCADE_OK || capture.acquire(&frame) != SACCADE_OK ||
        capture.retire(ExplicitWindowRetirementReason::replaced) != SACCADE_ERROR_BUSY) {
        return false;
    }
    fake.stop_result = SACCADE_ERROR_BACKEND;
    if (capture.release(frame) != SACCADE_OK || !capture.retiring() || fake.releases != 1 || fake.stream.leased || fake.stops != 1 ||
        fake.destroys != 0 || capture.release(frame) != SACCADE_ERROR_STALE_HANDLE) {
        return false;
    }
    fake.stop_result = SACCADE_OK;
    return capture.drain_retirement() == SACCADE_OK && !capture.retiring() && fake.stops == 2 && fake.destroys == 1 &&
           capture.select(identity_b(), 66) == SACCADE_OK && capture.shutdown() == SACCADE_OK;
}

bool destructor_consumes_the_last_published_lease() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    SceneCaptureFrame frame{};
    {
        ExplicitWindowCapture capture;
        if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK ||
            capture.select(identity_a(), 67) != SACCADE_OK || capture.acquire(&frame) != SACCADE_OK) {
            return false;
        }
    }
    return fake.releases == 1 && fake.stops == 1 && fake.destroys == 1 && !fake.stream.active;
}

bool repeated_sessions_balance_provider_ownership() noexcept {
    FakeProvider fake;
    set_sources(&fake);
    ExplicitWindowCapture capture;
    if (capture.initialize(descriptor(&fake), &fake, read_native_frame, 0, 0) != SACCADE_OK)
        return false;
    for (uint64_t epoch = 70; epoch != 78; ++epoch) {
        const ExplicitWindowIdentity selected = (epoch & 1U) == 0 ? identity_a() : identity_b();
        SceneCaptureFrame frame{};
        if (capture.select(selected, epoch) != SACCADE_OK || capture.acquire(&frame) != SACCADE_OK ||
            capture.release(frame) != SACCADE_OK || capture.retire(ExplicitWindowRetirementReason::replaced) != SACCADE_OK) {
            return false;
        }
    }
    return fake.creates == 8 && fake.starts == 8 && fake.acquires == 8 && fake.native_reads == 8 && fake.releases == 8 && fake.stops == 8 &&
           fake.destroys == 8 && capture.shutdown() == SACCADE_OK;
}

} // namespace

int main() {
    if (!exact_selection_and_frames())
        return static_cast<int>(TestResult::selection_failed);
    if (!wrong_identity_or_source_never_falls_back())
        return static_cast<int>(TestResult::identity_failed);
    if (!disappearance_retires_stream())
        return static_cast<int>(TestResult::disappearance_failed);
    if (!held_frame_drains_before_retirement())
        return static_cast<int>(TestResult::held_lease_failed);
    if (!switching_never_reuses_old_handles())
        return static_cast<int>(TestResult::switching_failed);
    if (!start_and_stop_failures_are_recoverable())
        return static_cast<int>(TestResult::failure_cleanup_failed);
    if (!consumed_frame_is_not_retried_when_deferred_teardown_fails())
        return static_cast<int>(TestResult::failure_cleanup_failed);
    if (!destructor_consumes_the_last_published_lease())
        return static_cast<int>(TestResult::failure_cleanup_failed);
    if (!repeated_sessions_balance_provider_ownership())
        return static_cast<int>(TestResult::repeated_session_failed);
    return static_cast<int>(TestResult::success);
}

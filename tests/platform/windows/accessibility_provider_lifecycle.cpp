#include "platform/windows/accessibility_provider.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>

namespace {

using saccade::platform::windows::AccessibilityProvider;

enum class TestResult : int {
    success,
    preinitialize_request_accepted,
    initialization_failed,
    queued_request_failed,
    invalid_query_wait_failed,
    invalid_query_snapshot_failed,
    invalid_query_release_failed,
    cancellation_request_failed,
    cancellation_wait_failed,
    shutdown_failed,
    shutdown_unbounded,
    postshutdown_request_accepted,
    reinitialization_failed,
    final_shutdown_failed
};

SaccadeAccessibilityQueryDesc query() noexcept {
    SaccadeAccessibilityQueryDesc value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.window_id = UINT64_MAX;
    value.scope = {0, 0, 1, 1};
    value.target_capacity = 1;
    value.session_epoch = 1;
    value.transform_epoch = 1;
    value.topology_epoch = 1;
    value.frame_id = 1;
    return value;
}

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    AccessibilityProvider provider;
    const SaccadeAccessibilityProviderDesc descriptor = provider.descriptor();
    const SaccadeAccessibilityQueryDesc request = query();
    SaccadeTicketHandle ticket = 1;
    if (descriptor.ops.request(descriptor.context, &request, &ticket) != SACCADE_ERROR_STATE || ticket != 0) {
        return result(TestResult::preinitialize_request_accepted);
    }
    if (provider.initialize() != SACCADE_OK) return result(TestResult::initialization_failed);
    if (descriptor.ops.request(descriptor.context, &request, &ticket) != SACCADE_OK || ticket == 0) {
        return result(TestResult::queued_request_failed);
    }
    SaccadeAccessibilityStatus status{};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    if (descriptor.ops.wait(descriptor.context, ticket, UINT64_C(2'000'000'000), &status) != SACCADE_OK ||
        status.state != SACCADE_TICKET_COMPLETE || status.snapshot == 0 || status.target_count != 0 ||
        status.required_bytes != sizeof(SaccadeTargetPacketHeader)) {
        return result(TestResult::invalid_query_wait_failed);
    }
    std::array<uint8_t, sizeof(SaccadeTargetPacketHeader)> packet{};
    size_t required = 0;
    if (descriptor.ops.collect(descriptor.context, status.snapshot, {packet.data(), packet.size()}, &required) !=
            SACCADE_OK ||
        required != packet.size()) {
        return result(TestResult::invalid_query_snapshot_failed);
    }
    const auto* header = reinterpret_cast<const SaccadeTargetPacketHeader*>(packet.data());
    if (header->target_count != 0 || header->total_size != packet.size() ||
        (header->flags & SACCADE_TARGET_PACKET_INCOMPLETE) == 0) {
        return result(TestResult::invalid_query_snapshot_failed);
    }
    if (descriptor.ops.release(descriptor.context, status.snapshot) != SACCADE_OK) {
        return result(TestResult::invalid_query_release_failed);
    }

    ticket = 0;
    if (descriptor.ops.request(descriptor.context, &request, &ticket) != SACCADE_OK || ticket == 0 ||
        descriptor.ops.cancel(descriptor.context, ticket) != SACCADE_OK) {
        return result(TestResult::cancellation_request_failed);
    }
    status = {};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    const SaccadeResult waited = descriptor.ops.wait(descriptor.context, ticket, UINT64_MAX, &status);
    if ((waited != SACCADE_OK && waited != SACCADE_ERROR_CANCELLED) || status.state != SACCADE_TICKET_CANCELLED) {
        return result(TestResult::cancellation_wait_failed);
    }

    const uint64_t shutdown_started = GetTickCount64();
    if (provider.shutdown() != SACCADE_OK) return result(TestResult::shutdown_failed);
    if (GetTickCount64() - shutdown_started > 1'000) return result(TestResult::shutdown_unbounded);
    if (descriptor.ops.request(descriptor.context, &request, &ticket) != SACCADE_ERROR_STATE || ticket != 0) {
        return result(TestResult::postshutdown_request_accepted);
    }
    if (provider.initialize() != SACCADE_OK) return result(TestResult::reinitialization_failed);
    return result(provider.shutdown() == SACCADE_OK ? TestResult::success : TestResult::final_shutdown_failed);
}

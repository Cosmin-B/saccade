#include "platform/windows/accessibility_provider.hpp"
#include "scene/packet.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>
#include <vector>

using saccade::platform::windows::AccessibilityProvider;

namespace {

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int main() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = window_proc;
    type.lpszClassName = L"SaccadeAccessibilityProviderTest";
    if (RegisterClassW(&type) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    HWND window = CreateWindowExW(0, type.lpszClassName, L"provider test", WS_OVERLAPPEDWINDOW, 100, 100, 640, 480,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 2;
    HWND button = CreateWindowExW(0, L"BUTTON", L"Run", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 30, 30, 120, 40, window,
                                  nullptr, instance, nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 30, 90, 240, 32,
                                window, nullptr, instance, nullptr);
    if (button == nullptr || edit == nullptr) return 3;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    AccessibilityProvider provider;
    if (provider.initialize() != SACCADE_OK) return 4;
    SaccadeAccessibilityProviderDesc desc = provider.descriptor();
    SaccadeAccessibilityQueryDesc query{};
    query.struct_size = sizeof(query);
    query.api_version = SACCADE_API_VERSION;
    query.window_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
    query.scope = {100, 100, 640, 480};
    query.target_capacity = 64;
    query.session_epoch = 7;
    query.transform_epoch = 9;
    query.topology_epoch = 11;
    query.frame_id = 13;

    SaccadeTicketHandle ticket = 0;
    if (desc.ops.request(desc.context, &query, &ticket) != SACCADE_OK || ticket == 0) return 5;
    SaccadeAccessibilityStatus status{};
    status.struct_size = sizeof(status);
    status.api_version = SACCADE_API_VERSION;
    const uint64_t deadline = GetTickCount64() + 5000;
    SaccadeResult polled = SACCADE_OK;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        polled = desc.ops.poll(desc.context, ticket, &status);
        if (polled != SACCADE_OK || status.state == SACCADE_TICKET_COMPLETE || status.state == SACCADE_TICKET_FAILED)
            break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    if (polled != SACCADE_OK || status.state != SACCADE_TICKET_COMPLETE || status.snapshot == 0 ||
        status.target_count < 2)
        return 6;

    size_t required = 0;
    if (desc.ops.collect(desc.context, status.snapshot, {}, &required) != SACCADE_ERROR_CAPACITY ||
        required != status.required_bytes)
        return 7;
    std::vector<uint8_t> packet(required);
    if (desc.ops.collect(desc.context, status.snapshot, {packet.data(), packet.size()}, &required) != SACCADE_OK)
        return 8;
    const auto* header = reinterpret_cast<const SaccadeTargetPacketHeader*>(packet.data());
    saccade::scene::PacketView validated{};
    if (saccade::scene::validate_packet({packet.data(), packet.size()}, &validated) != SACCADE_OK) return 9;
    if (header->packet_version != SACCADE_TARGET_PACKET_VERSION ||
        header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || header->session_epoch != 7 ||
        header->transform_epoch != 9 || header->topology_epoch != 11 || header->frame_id != 13 ||
        header->target_count != status.target_count)
        return 9;
    const auto* targets = reinterpret_cast<const SaccadeTargetRecord*>(packet.data() + header->targets_offset);
    bool found_button = false;
    bool found_text = false;
    for (uint32_t index = 0; index < header->target_count; ++index) {
        found_button |= targets[index].role == SACCADE_TARGET_ROLE_BUTTON &&
                        (targets[index].capability_bits & SACCADE_TARGET_CAPABILITY_BUTTON) != 0;
        found_text |= targets[index].role == SACCADE_TARGET_ROLE_TEXT_FIELD &&
                      (targets[index].capability_bits & SACCADE_TARGET_CAPABILITY_TEXT) != 0 &&
                      (targets[index].capability_bits & SACCADE_TARGET_CAPABILITY_TEXT_SELECT) != 0;
    }
    if (!found_button || !found_text || desc.ops.release(desc.context, status.snapshot) != SACCADE_OK) return 10;

    DestroyWindow(window);
    return provider.shutdown() == SACCADE_OK ? 0 : 11;
}

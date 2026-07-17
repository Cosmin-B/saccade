#include "platform/windows/input_executor.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class TestResult : int {
    success,
    no_interactive_desktop,
    class_registration_failed,
    window_creation_failed,
    window_focus_failed,
    executor_initialization_failed,
    left_click_failed,
    alternate_click_failed,
    drag_failed,
    scroll_failed,
    text_failed,
    physical_override_failed,
    stale_plan_failed,
    pointer_restore_failed,
    shutdown_failed
};

constexpr int test_skipped = 77;
constexpr char live_test_category[] = "workflow";
constexpr uint64_t topology_epoch = 17;
constexpr uint64_t permission_epoch = 19;
constexpr uint32_t maximum_commands = 8;
constexpr uint32_t maximum_payload_bytes = 64;
constexpr uint32_t pump_duration_ms = 80;
constexpr uint32_t expected_text_length = 7;
constexpr size_t plan_capacity = sizeof(SaccadeInputPlanHeader) +
                                 static_cast<size_t>(maximum_commands) * sizeof(SaccadeInputCommand) +
                                 maximum_payload_bytes;

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

struct WindowState {
    uint32_t moves = 0;
    uint32_t drags = 0;
    uint32_t left_down = 0;
    uint32_t left_up = 0;
    uint32_t right_down = 0;
    uint32_t right_up = 0;
    uint32_t middle_down = 0;
    uint32_t middle_up = 0;
    uint32_t vertical_scroll = 0;
    uint32_t horizontal_scroll = 0;
    uint32_t key_down = 0;
    std::array<wchar_t, maximum_payload_bytes> text{};
    uint32_t text_count = 0;
};

WindowState* window_state(HWND window) noexcept {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    WindowState* state = window_state(window);
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
    case WM_MOUSEMOVE:
        ++state->moves;
        state->drags += (wparam & MK_LBUTTON) != 0 ? 1U : 0U;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        ++state->left_down;
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
        ++state->left_up;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        ++state->right_down;
        return 0;
    case WM_RBUTTONUP:
        ++state->right_up;
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        ++state->middle_down;
        return 0;
    case WM_MBUTTONUP:
        ++state->middle_up;
        return 0;
    case WM_MOUSEWHEEL:
        ++state->vertical_scroll;
        return 0;
    case WM_MOUSEHWHEEL:
        ++state->horizontal_scroll;
        return 0;
    case WM_KEYDOWN:
        ++state->key_down;
        return 0;
    case WM_CHAR:
        if (state->text_count != state->text.size()) state->text[state->text_count++] = static_cast<wchar_t>(wparam);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void pump_messages(uint32_t milliseconds) noexcept {
    const uint64_t deadline = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
}

struct alignas(8) PlanStorage {
    std::array<uint8_t, plan_capacity> bytes{};
};

SaccadeSpanU8 make_plan(PlanStorage* storage, uint64_t plan_id, uint32_t permissions,
                        const SaccadeInputCommand* commands, uint32_t command_count, uint64_t window_id,
                        const uint8_t* payload = nullptr, uint32_t payload_size = 0) noexcept {
    if (storage == nullptr || commands == nullptr || command_count == 0 || command_count > maximum_commands ||
        payload_size > maximum_payload_bytes) {
        return {};
    }
    *storage = {};
    SaccadeInputPlanHeader header{};
    header.struct_size = sizeof(header);
    header.plan_version = SACCADE_INPUT_PLAN_VERSION;
    header.command_count = command_count;
    header.command_stride = sizeof(SaccadeInputCommand);
    header.flags = SACCADE_INPUT_PLAN_STOP_ON_FAILURE;
    header.required_permissions = permissions;
    header.plan_id = plan_id;
    header.scene_epoch = 1;
    header.frame_id = 2;
    header.model_epoch = 3;
    header.session_epoch = 4;
    header.transform_epoch = 5;
    header.topology_epoch = topology_epoch;
    header.permission_epoch = permission_epoch;
    header.source_id = 6;
    header.focus_id = window_id;
    header.window_id = window_id;
    header.display_id = 7;
    header.deadline_ns = UINT64_MAX;
    header.commands_offset = sizeof(header);
    header.total_size =
        sizeof(header) + static_cast<uint64_t>(command_count) * sizeof(SaccadeInputCommand) + payload_size;
    std::memcpy(storage->bytes.data(), &header, sizeof(header));
    std::memcpy(storage->bytes.data() + sizeof(header), commands,
                static_cast<size_t>(command_count) * sizeof(SaccadeInputCommand));
    if (payload_size != 0) {
        std::memcpy(storage->bytes.data() + header.total_size - payload_size, payload, payload_size);
    }
    return {storage->bytes.data(), static_cast<size_t>(header.total_size)};
}

SaccadeInputCommand pointer_command(uint32_t kind, POINT point) noexcept {
    SaccadeInputCommand command{};
    command.kind = kind;
    command.flags = SACCADE_INPUT_COMMAND_ABSOLUTE;
    command.target_id = 1;
    command.x_q8 = point.x * 256;
    command.y_q8 = point.y * 256;
    return command;
}

class LiveContext final {
  public:
    ~LiveContext() {
        if (executor_initialized_) {
            (void)executor_.release_all();
            (void)executor_.shutdown();
        }
        SetCursorPos(original_pointer_.x, original_pointer_.y);
        if (window_ != nullptr) DestroyWindow(window_);
    }

    LiveContext(const LiveContext&) = delete;
    LiveContext& operator=(const LiveContext&) = delete;

    LiveContext() = default;

    WindowState state_{};
    HWND window_ = nullptr;
    POINT original_pointer_{};
    saccade::platform::windows::InputExecutor executor_{};
    bool executor_initialized_ = false;
};

bool execute(saccade::platform::windows::InputExecutor* executor, SaccadeSpanU8 plan, uint32_t permissions) noexcept {
    saccade::platform::windows::InputExecutionResult execution{};
    return executor->execute(plan, permissions, 1, &execution) == SACCADE_OK && execution.flags == 0;
}

} // namespace

int main() {
    char category[sizeof(live_test_category)]{};
    const DWORD category_size =
        GetEnvironmentVariableA("SACCADE_ALLOW_LIVE_TESTS", category, static_cast<DWORD>(sizeof(category)));
    if (category_size != sizeof(live_test_category) - 1U ||
        std::memcmp(category, live_test_category, category_size) != 0)
        return test_skipped;

    HDESK desktop_handle = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (desktop_handle == nullptr) return test_skipped;
    CloseDesktop(desktop_handle);

    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LiveContext context;
    if (!GetCursorPos(&context.original_pointer_)) return result(TestResult::no_interactive_desktop);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.style = CS_DBLCLKS;
    type.hInstance = instance;
    type.lpfnWndProc = window_proc;
    type.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    type.lpszClassName = L"SaccadeInputWorkflowLive";
    if (RegisterClassW(&type) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return result(TestResult::class_registration_failed);

    context.window_ =
        CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName, L"Saccade Input Qualification", WS_OVERLAPPEDWINDOW, 160,
                        160, 640, 420, nullptr, nullptr, instance, &context.state_);
    if (context.window_ == nullptr) return result(TestResult::window_creation_failed);
    ShowWindow(context.window_, SW_SHOW);
    SetForegroundWindow(context.window_);
    SetFocus(context.window_);
    pump_messages(pump_duration_ms);
    if (GetForegroundWindow() != context.window_ || GetFocus() != context.window_)
        return result(TestResult::window_focus_failed);

    RECT client{};
    if (!GetClientRect(context.window_, &client)) return result(TestResult::window_creation_failed);
    POINT first{client.left + 100, client.top + (client.bottom - client.top) / 2};
    POINT second{client.right - 100, first.y};
    if (!ClientToScreen(context.window_, &first) || !ClientToScreen(context.window_, &second))
        return result(TestResult::window_creation_failed);

    const saccade::platform::windows::VirtualDesktop virtual_desktop{
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        static_cast<uint32_t>(GetSystemMetrics(SM_CXVIRTUALSCREEN)),
        static_cast<uint32_t>(GetSystemMetrics(SM_CYVIRTUALSCREEN)), topology_epoch};
    const saccade::platform::windows::InputSink sink{nullptr, saccade::platform::windows::submit_with_send_input,
                                                     nullptr};
    if (context.executor_.initialize(virtual_desktop, sink, permission_epoch, context.original_pointer_.x * 256,
                                     context.original_pointer_.y * 256) != SACCADE_OK) {
        return result(TestResult::executor_initialization_failed);
    }
    context.executor_initialized_ = true;
    static PlanStorage storage;
    const uint64_t window_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context.window_));

    std::array<SaccadeInputCommand, 2> left{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_CLICK, first)};
    left[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    left[1].data1 = 1;
    if (!execute(&context.executor_,
                 make_plan(&storage, 1, SACCADE_INPUT_PERMISSION_POINTER, left.data(),
                           static_cast<uint32_t>(left.size()), window_id),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::left_click_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.moves == 0 || context.state_.left_down != 1 || context.state_.left_up != 1)
        return result(TestResult::left_click_failed);

    std::array<SaccadeInputCommand, 2> alternate{pointer_command(SACCADE_INPUT_COMMAND_CLICK, first),
                                                 pointer_command(SACCADE_INPUT_COMMAND_CLICK, first)};
    alternate[0].data0 = SACCADE_INPUT_BUTTON_RIGHT;
    alternate[0].data1 = 2;
    alternate[1].data0 = SACCADE_INPUT_BUTTON_MIDDLE;
    alternate[1].data1 = 1;
    if (!execute(&context.executor_,
                 make_plan(&storage, 2, SACCADE_INPUT_PERMISSION_POINTER, alternate.data(),
                           static_cast<uint32_t>(alternate.size()), window_id),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::alternate_click_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.right_down != 2 || context.state_.right_up != 2 || context.state_.middle_down != 1 ||
        context.state_.middle_up != 1) {
        return result(TestResult::alternate_click_failed);
    }

    std::array<SaccadeInputCommand, 4> drag{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, first),
                                            pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, second),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_UP, second)};
    drag[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    drag[3].data0 = SACCADE_INPUT_BUTTON_LEFT;
    const uint32_t drag_down_before = context.state_.left_down;
    const uint32_t drag_up_before = context.state_.left_up;
    if (!execute(&context.executor_,
                 make_plan(&storage, 3, SACCADE_INPUT_PERMISSION_POINTER, drag.data(),
                           static_cast<uint32_t>(drag.size()), window_id),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::drag_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.left_down != drag_down_before + 1U || context.state_.left_up != drag_up_before + 1U ||
        context.state_.drags == 0) {
        return result(TestResult::drag_failed);
    }

    SaccadeInputCommand scroll = pointer_command(SACCADE_INPUT_COMMAND_SCROLL, second);
    scroll.delta_x_q8 = 256;
    scroll.delta_y_q8 = -512;
    if (!execute(&context.executor_, make_plan(&storage, 4, SACCADE_INPUT_PERMISSION_POINTER, &scroll, 1, window_id),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::scroll_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.vertical_scroll == 0 || context.state_.horizontal_scroll == 0)
        return result(TestResult::scroll_failed);

    constexpr std::array<uint8_t, expected_text_length> text{'S', 'a', 'c', 'c', 'a', 'd', 'e'};
    SaccadeInputCommand text_command{};
    text_command.kind = SACCADE_INPUT_COMMAND_TEXT;
    text_command.target_id = 1;
    text_command.payload_offset = sizeof(SaccadeInputPlanHeader) + sizeof(SaccadeInputCommand);
    text_command.payload_size = static_cast<uint32_t>(text.size());
    SetFocus(context.window_);
    if (!execute(&context.executor_,
                 make_plan(&storage, 5, SACCADE_INPUT_PERMISSION_TEXT, &text_command, 1, window_id, text.data(),
                           static_cast<uint32_t>(text.size())),
                 SACCADE_INPUT_PERMISSION_TEXT)) {
        return result(TestResult::text_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.text_count != text.size() ||
        std::memcmp(context.state_.text.data(), L"Saccade", text.size() * sizeof(wchar_t)) != 0) {
        return result(TestResult::text_failed);
    }

    std::array<SaccadeInputCommand, 2> hold{pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, second),
                                            pointer_command(SACCADE_INPUT_COMMAND_BUTTON_DOWN, second)};
    hold[1].data0 = SACCADE_INPUT_BUTTON_LEFT;
    const uint32_t release_before = context.state_.left_up;
    if (!execute(&context.executor_,
                 make_plan(&storage, 6, SACCADE_INPUT_PERMISSION_POINTER, hold.data(),
                           static_cast<uint32_t>(hold.size()), window_id),
                 SACCADE_INPUT_PERMISSION_POINTER) ||
        !context.executor_.synthetic_input_active() ||
        context.executor_.physical_override(context.original_pointer_.x * 256, context.original_pointer_.y * 256) !=
            SACCADE_OK ||
        context.executor_.synthetic_input_active()) {
        return result(TestResult::physical_override_failed);
    }
    pump_messages(pump_duration_ms);
    if (context.state_.left_up != release_before + 1U) return result(TestResult::physical_override_failed);

    SaccadeInputCommand stale = pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, first);
    SaccadeSpanU8 stale_plan = make_plan(&storage, 7, SACCADE_INPUT_PERMISSION_POINTER, &stale, 1, window_id);
    reinterpret_cast<SaccadeInputPlanHeader*>(storage.bytes.data())->topology_epoch = topology_epoch + 1U;
    saccade::platform::windows::InputExecutionResult execution{};
    if (context.executor_.execute(stale_plan, SACCADE_INPUT_PERMISSION_POINTER, 1, &execution) !=
        SACCADE_ERROR_STALE_HANDLE) {
        return result(TestResult::stale_plan_failed);
    }

    POINT restore = context.original_pointer_;
    SaccadeInputCommand restore_command = pointer_command(SACCADE_INPUT_COMMAND_POINTER_MOVE, restore);
    if (!execute(&context.executor_,
                 make_plan(&storage, 8, SACCADE_INPUT_PERMISSION_POINTER, &restore_command, 1, window_id),
                 SACCADE_INPUT_PERMISSION_POINTER)) {
        return result(TestResult::pointer_restore_failed);
    }
    pump_messages(pump_duration_ms);
    POINT restored{};
    if (!GetCursorPos(&restored) || restored.x != restore.x || restored.y != restore.y)
        return result(TestResult::pointer_restore_failed);

    if (context.executor_.shutdown() != SACCADE_OK) return result(TestResult::shutdown_failed);
    context.executor_initialized_ = false;
    return result(TestResult::success);
}

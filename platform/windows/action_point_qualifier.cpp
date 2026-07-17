#include "platform/windows/action_point_qualifier.hpp"

#include <array>
#include <cmath>
#include <cwchar>
#include <limits>
#include <utility>

namespace saccade::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD uia_focus_timeout_ms = 4;
constexpr uint64_t uia_focus_budget_ms = 6;

bool budget_expired(uint64_t started_ms) noexcept {
    return GetTickCount64() - started_ms >= uia_focus_budget_ms;
}

bool point_coordinate(int32_t q8, LONG* output) noexcept {
    const double value = static_cast<double>(q8) / 256.0;
    if (value < std::numeric_limits<LONG>::min() || value > std::numeric_limits<LONG>::max()) return false;
    *output = static_cast<LONG>(std::llround(value));
    return true;
}

ActionPointDisposition inspect_element(IUIAutomationElement* element, bool require_geometry,
                                       uint64_t started_ms) noexcept {
    BOOL password = FALSE;
    if (FAILED(element->get_CurrentIsPassword(&password))) return ActionPointDisposition::unavailable;
    if (password != FALSE) return ActionPointDisposition::secure;
    if (!require_geometry) return ActionPointDisposition::qualified;
    if (budget_expired(started_ms)) return ActionPointDisposition::unavailable;
    BOOL offscreen = TRUE;
    int process_id = 0;
    RECT bounds{};
    if (FAILED(element->get_CurrentIsOffscreen(&offscreen)) || budget_expired(started_ms) ||
        FAILED(element->get_CurrentProcessId(&process_id)) || budget_expired(started_ms) ||
        FAILED(element->get_CurrentBoundingRectangle(&bounds)) || process_id <= 0 || offscreen != FALSE ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return ActionPointDisposition::unavailable;
    }
    return ActionPointDisposition::qualified;
}

ActionPointDisposition inspect_ancestry(IUIAutomation* automation, IUIAutomationElement* first, uint64_t window_id,
                                        bool require_geometry, uint64_t started_ms) noexcept {
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || walker == nullptr)
        return ActionPointDisposition::unavailable;

    ComPtr<IUIAutomationElement> element = first;
    constexpr uint32_t ancestor_limit = 16;
    bool window_matched = window_id == 0;

    for (uint32_t depth = 0; depth < ancestor_limit; ++depth) {
        if (budget_expired(started_ms)) return ActionPointDisposition::unavailable;
        const ActionPointDisposition disposition =
            inspect_element(element.Get(), require_geometry && depth == 0, started_ms);
        if (disposition != ActionPointDisposition::qualified) return disposition;

        UIA_HWND native_window = nullptr;
        if (SUCCEEDED(element->get_CurrentNativeWindowHandle(&native_window)) && native_window != nullptr) {
            window_matched |= reinterpret_cast<uintptr_t>(native_window) == static_cast<uintptr_t>(window_id);
        }

        ComPtr<IUIAutomationElement> parent;
        const HRESULT parent_result = walker->GetParentElement(element.Get(), &parent);
        if (FAILED(parent_result)) return ActionPointDisposition::unavailable;
        if (parent == nullptr)
            return window_matched ? ActionPointDisposition::qualified : ActionPointDisposition::unavailable;
        element = std::move(parent);
    }
    return ActionPointDisposition::unavailable;
}

bool password_window(HWND window) noexcept {
    std::array<wchar_t, 32> name{};
    const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    if (length <= 0) return false;
    const bool edit = _wcsicmp(name.data(), L"Edit") == 0 || _wcsnicmp(name.data(), L"RichEdit", 8) == 0;
    return edit && (GetWindowLongPtrW(window, GWL_STYLE) & ES_PASSWORD) != 0;
}

ActionPointDisposition qualify_window(HWND candidate, uint64_t window_id) noexcept {
    if (candidate == nullptr || IsWindow(candidate) == 0 || IsWindowVisible(candidate) == 0)
        return ActionPointDisposition::unavailable;
    const HWND root = GetAncestor(candidate, GA_ROOT);
    if (window_id != 0 && reinterpret_cast<uintptr_t>(root) != static_cast<uintptr_t>(window_id))
        return ActionPointDisposition::unavailable;
    DWORD process_id = 0;
    if (GetWindowThreadProcessId(candidate, &process_id) == 0 || process_id == 0)
        return ActionPointDisposition::unavailable;
    for (HWND current = candidate; current != nullptr; current = GetParent(current)) {
        if (password_window(current)) return ActionPointDisposition::secure;
        if (current == root) break;
    }
    return ActionPointDisposition::qualified;
}

ActionPointDisposition qualify_focus_with_windows(uint64_t window_id) noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr ||
        (window_id != 0 && reinterpret_cast<uintptr_t>(foreground) != static_cast<uintptr_t>(window_id))) {
        return ActionPointDisposition::unavailable;
    }
    const DWORD thread_id = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const HWND focus = thread_id != 0 && GetGUIThreadInfo(thread_id, &info) != 0 && info.hwndFocus != nullptr
                           ? info.hwndFocus
                           : foreground;
    return qualify_window(focus, window_id);
}

} // namespace

SaccadeResult ActionPointQualifier::initialize() noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    owner_thread_id_ = GetCurrentThreadId();
    initialized_ = true;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return SACCADE_OK;
    co_initialized_ = SUCCEEDED(apartment);
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation_));
    if (FAILED(result))
        result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation_));
    if (FAILED(result)) {
        automation_.Reset();
        return SACCADE_OK;
    }
    ComPtr<IUIAutomation2> bounded;
    if (FAILED(automation_.As(&bounded)) || FAILED(bounded->put_ConnectionTimeout(uia_focus_timeout_ms)) ||
        FAILED(bounded->put_TransactionTimeout(uia_focus_timeout_ms))) {
        automation_.Reset();
        return SACCADE_OK;
    }
    ComPtr<IUIAutomation6> recovery;
    if (SUCCEEDED(automation_.As(&recovery))) {
        (void)recovery->put_ConnectionRecoveryBehavior(ConnectionRecoveryBehaviorOptions_Disabled);
    }
    return SACCADE_OK;
}

SaccadeResult ActionPointQualifier::shutdown() noexcept {
    if (!initialized_) return SACCADE_OK;
    if (owner_thread_id_ != GetCurrentThreadId()) return SACCADE_ERROR_STATE;
    automation_.Reset();
    if (co_initialized_) CoUninitialize();
    owner_thread_id_ = 0;
    co_initialized_ = false;
    initialized_ = false;
    return SACCADE_OK;
}

ActionPointDisposition ActionPointQualifier::qualify(int32_t x_q8, int32_t y_q8, uint64_t window_id) const noexcept {
    if (!initialized_ || owner_thread_id_ != GetCurrentThreadId()) return ActionPointDisposition::unavailable;
    POINT point{};
    if (!point_coordinate(x_q8, &point.x) || !point_coordinate(y_q8, &point.y))
        return ActionPointDisposition::unavailable;

    if (automation_ != nullptr) {
        const uint64_t started_ms = GetTickCount64();
        ComPtr<IUIAutomationElement> element;
        if (SUCCEEDED(automation_->ElementFromPoint(point, &element)) && element != nullptr) {
            const ActionPointDisposition disposition =
                inspect_ancestry(automation_.Get(), element.Get(), window_id, true, started_ms);
            if (disposition != ActionPointDisposition::unavailable) return disposition;
        }
    }
    return qualify_window(WindowFromPoint(point), window_id);
}

ActionPointDisposition ActionPointQualifier::qualify_focus(uint64_t window_id) const noexcept {
    if (!initialized_ || owner_thread_id_ != GetCurrentThreadId()) return ActionPointDisposition::unavailable;

    if (automation_ != nullptr) {
        const uint64_t started_ms = GetTickCount64();
        ComPtr<IUIAutomationElement> element;
        if (SUCCEEDED(automation_->GetFocusedElement(&element)) && element != nullptr) {
            const ActionPointDisposition disposition =
                inspect_ancestry(automation_.Get(), element.Get(), window_id, true, started_ms);
            if (disposition != ActionPointDisposition::unavailable) return disposition;
        }
    }
    return qualify_focus_with_windows(window_id);
}

} // namespace saccade::platform::windows

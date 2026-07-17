#include "platform/windows/accessibility_provider.hpp"

#include "core/cache_line.hpp"
#include "platform/windows/display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dwmapi.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <windows.h>
#include <wrl/client.h>

#include "platform/windows/accessibility_target_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace saccade::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t maximum_windows = 256;
constexpr uint64_t provider_id = UINT64_C(0x57494E5549410001);
constexpr char provider_name[] = "Windows UI Automation";
constexpr uint64_t nanoseconds_per_millisecond = 1'000'000;
constexpr DWORD uia_connection_timeout_ms = 100;
constexpr DWORD uia_transaction_timeout_ms = 100;
constexpr uint64_t query_budget_ms = 500;
constexpr DWORD cancelled_wait_ms = 16;
constexpr DWORD shutdown_wait_ms = 750;

constexpr DWORD wait_milliseconds(uint64_t timeout_ns) noexcept {
    if (timeout_ns == UINT64_MAX) return INFINITE;
    const uint64_t whole = timeout_ns / nanoseconds_per_millisecond;
    const uint64_t rounded = whole + (timeout_ns % nanoseconds_per_millisecond != 0 ? 1U : 0U);
    return static_cast<DWORD>(std::min<uint64_t>(rounded, INFINITE - 1U));
}

static_assert(wait_milliseconds(0) == 0);
static_assert(wait_milliseconds(1) == 1);
static_assert(wait_milliseconds(nanoseconds_per_millisecond) == 1);
static_assert(wait_milliseconds(UINT64_MAX - 1U) == INFINITE - 1U);
static_assert(wait_milliseconds(UINT64_MAX) == INFINITE);

enum class WorkState : uint32_t { idle, queued, running, complete, cancelled, failed, stopping };

struct WindowEntry {
    uint64_t id = 0;
    uint64_t process_id = 0;
    SaccadeRectI32 bounds{};
    std::array<uint8_t, 256> title{};
    uint32_t title_size = 0;
};

struct WindowCollector {
    std::array<WindowEntry, maximum_windows>* windows = nullptr;
    uint32_t count = 0;
};

bool valid_window(HWND window) noexcept {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }
    RECT bounds{};
    return GetWindowRect(window, &bounds) && bounds.right > bounds.left && bounds.bottom > bounds.top;
}

BOOL CALLBACK collect_window(HWND window, LPARAM context) noexcept {
    auto* collector = reinterpret_cast<WindowCollector*>(context);
    if (!valid_window(window) || collector->count == maximum_windows) {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == GetCurrentProcessId()) {
        return TRUE;
    }

    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        GetWindowRect(window, &bounds);
    }
    WindowEntry& entry = (*collector->windows)[collector->count++];
    entry.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
    entry.process_id = process_id;
    entry.bounds = {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};

    std::array<wchar_t, 256> wide{};
    const int wide_size = GetWindowTextW(window, wide.data(), static_cast<int>(wide.size()));
    if (wide_size > 0) {
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, reinterpret_cast<char*>(entry.title.data()),
                                static_cast<int>(entry.title.size()), nullptr, nullptr);
        entry.title_size = bytes > 0 ? static_cast<uint32_t>(bytes) : 0;
    }
    return TRUE;
}

int32_t q8(double value) noexcept {
    constexpr double minimum = static_cast<double>(std::numeric_limits<int32_t>::min()) / 256.0;
    constexpr double maximum = static_cast<double>(std::numeric_limits<int32_t>::max()) / 256.0;
    return static_cast<int32_t>(std::llround(std::clamp(value, minimum, maximum) * 256.0));
}

bool intersects(const SaccadeRectI32& scope, const tagRECT& rect) noexcept {
    const int64_t scope_right = static_cast<int64_t>(scope.x) + scope.width;
    const int64_t scope_bottom = static_cast<int64_t>(scope.y) + scope.height;
    return rect.right > scope.x && rect.bottom > scope.y && rect.left < scope_right && rect.top < scope_bottom;
}

SaccadeTargetRole role_for(CONTROLTYPEID type) noexcept {
    switch (type) {
    case UIA_ButtonControlTypeId:
        return SACCADE_TARGET_ROLE_BUTTON;
    case UIA_HyperlinkControlTypeId:
        return SACCADE_TARGET_ROLE_LINK;
    case UIA_TextControlTypeId:
    case UIA_DocumentControlTypeId:
        return SACCADE_TARGET_ROLE_TEXT;
    case UIA_EditControlTypeId:
        return SACCADE_TARGET_ROLE_TEXT_FIELD;
    case UIA_CheckBoxControlTypeId:
        return SACCADE_TARGET_ROLE_CHECKBOX;
    case UIA_RadioButtonControlTypeId:
        return SACCADE_TARGET_ROLE_RADIO;
    case UIA_MenuItemControlTypeId:
        return SACCADE_TARGET_ROLE_MENU_ITEM;
    case UIA_SliderControlTypeId:
        return SACCADE_TARGET_ROLE_SLIDER;
    case UIA_ImageControlTypeId:
        return SACCADE_TARGET_ROLE_IMAGE;
    case UIA_WindowControlTypeId:
        return SACCADE_TARGET_ROLE_WINDOW;
    default:
        return SACCADE_TARGET_ROLE_UNKNOWN;
    }
}

uint64_t hash_runtime_id(SAFEARRAY* runtime_id, uint64_t window_id, uint32_t order) noexcept {
    uint64_t hash = UINT64_C(1469598103934665603);
    bool has_runtime_id = false;
    if (runtime_id != nullptr && SafeArrayGetDim(runtime_id) == 1) {
        LONG lower = 0;
        LONG upper = -1;
        if (SUCCEEDED(SafeArrayGetLBound(runtime_id, 1, &lower)) &&
            SUCCEEDED(SafeArrayGetUBound(runtime_id, 1, &upper))) {
            for (LONG index = lower; index <= upper; ++index) {
                int value = 0;
                if (SUCCEEDED(SafeArrayGetElement(runtime_id, &index, &value))) {
                    has_runtime_id = true;
                    hash ^= static_cast<uint32_t>(value);
                    hash *= UINT64_C(1099511628211);
                }
            }
        }
    }
    hash ^= window_id;
    hash *= UINT64_C(1099511628211);
    if (!has_runtime_id) {
        hash ^= order;
    }
    return hash == 0 ? 1 : hash;
}

uint64_t display_at(LONG x, LONG y) noexcept {
    const POINT point{x, y};
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) ? stable_display_id(info.szDevice) : 0;
}

bool bool_property(IUIAutomationElement* element, PROPERTYID property) noexcept {
    VARIANT value{};
    VariantInit(&value);
    const HRESULT result = element->GetCurrentPropertyValue(property, &value);
    const bool enabled = SUCCEEDED(result) && value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    return enabled;
}

uint32_t capabilities(IUIAutomationElement* element, CONTROLTYPEID type) noexcept {
    uint32_t bits = SACCADE_TARGET_CAPABILITY_POINTER_MOVE;
    if (bool_property(element, UIA_IsInvokePatternAvailablePropertyId)) {
        bits |= SACCADE_TARGET_CAPABILITY_INVOKE | SACCADE_TARGET_CAPABILITY_BUTTON;
    }
    if (bool_property(element, UIA_IsScrollPatternAvailablePropertyId)) {
        bits |= SACCADE_TARGET_CAPABILITY_SCROLL;
    }
    if (bool_property(element, UIA_IsValuePatternAvailablePropertyId) || type == UIA_EditControlTypeId) {
        bits |= SACCADE_TARGET_CAPABILITY_TEXT;
    }
    if (type == UIA_TextControlTypeId || type == UIA_DocumentControlTypeId || type == UIA_EditControlTypeId) {
        bits |= SACCADE_TARGET_CAPABILITY_TEXT_SELECT;
    }
    switch (type) {
    case UIA_ButtonControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_RadioButtonControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_TabItemControlTypeId:
        bits |= SACCADE_TARGET_CAPABILITY_BUTTON;
        break;
    case UIA_WindowControlTypeId:
        bits |= SACCADE_TARGET_CAPABILITY_WINDOW_ACTIVATE;
        break;
    default:
        break;
    }
    return bits;
}

HRESULT create_target_condition(IUIAutomation* automation, IUIAutomationCondition** output) noexcept {
    constexpr size_t condition_count =
        accessibility_target_control_types.size() + accessibility_target_pattern_properties.size();
    std::array<ComPtr<IUIAutomationCondition>, condition_count> conditions{};
    std::array<IUIAutomationCondition*, condition_count> raw{};
    size_t count = 0;

    for (CONTROLTYPEID type : accessibility_target_control_types) {
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = type;
        const HRESULT created = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value,
                                                                    conditions[count].ReleaseAndGetAddressOf());
        if (FAILED(created)) return created;
        raw[count] = conditions[count].Get();
        ++count;
    }
    for (PROPERTYID property : accessibility_target_pattern_properties) {
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        const HRESULT created =
            automation->CreatePropertyCondition(property, value, conditions[count].ReleaseAndGetAddressOf());
        if (FAILED(created)) return created;
        raw[count] = conditions[count].Get();
        ++count;
    }
    return automation->CreateOrConditionFromNativeArray(raw.data(), static_cast<int>(raw.size()), output);
}

template <typename T> SaccadeResult write_structure(T* destination, const T& value) noexcept {
    if (destination == nullptr || destination->struct_size < sizeof(T) ||
        destination->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t size = destination->struct_size;
    *destination = value;
    destination->struct_size = size;
    destination->api_version = SACCADE_API_VERSION;
    return SACCADE_OK;
}

} // namespace

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct AccessibilityProvider::Impl {
    alignas(core::destructive_interference_size) std::atomic<WorkState> state_{WorkState::idle};
    alignas(core::destructive_interference_size) std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> client_detached_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_requested_{false};
    HANDLE request_event_ = nullptr;
    HANDLE done_event_ = nullptr;
    HANDLE worker_ = nullptr;
    DWORD worker_thread_id_ = 0;
    SaccadeAccessibilityQueryDesc query_{};
    SaccadeTicketHandle ticket_ = 0;
    SaccadeSnapshotHandle snapshot_ = 0;
    uint64_t next_ticket_ = 1;
    SaccadeResult result_ = SACCADE_OK;
    uint32_t target_count_ = 0;
    uint32_t text_size_ = 0;
    uint32_t packet_size_ = 0;
    uint32_t packet_flags_ = 0;
    uint64_t query_started_ms_ = 0;
    int32_t native_error_ = 0;
    AccessibilityProviderStats stats_{};
    std::array<WindowEntry, maximum_windows> windows_{};
    uint32_t window_count_ = 0;
    alignas(64) SaccadeTargetPacketHeader header_{};
    SaccadeTargetRecord* targets_ = nullptr;
    uint8_t* text_ = nullptr;

    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }

    [[nodiscard]] bool query_expired() const noexcept {
        return GetTickCount64() - query_started_ms_ >= query_budget_ms;
    }

    void finish_cancelled() noexcept {
        result_ = SACCADE_ERROR_CANCELLED;
        ++stats_.cancelled;
        state_.store(WorkState::cancelled, std::memory_order_release);
    }

    void complete_empty(int32_t native_error) noexcept {
        target_count_ = 0;
        text_size_ = 0;
        packet_flags_ = SACCADE_TARGET_PACKET_INCOMPLETE;
        native_error_ = native_error;
        result_ = SACCADE_OK;
        header_ = {};
        header_.struct_size = sizeof(header_);
        header_.packet_version = SACCADE_TARGET_PACKET_VERSION;
        header_.target_stride = sizeof(SaccadeTargetRecord);
        header_.flags = packet_flags_;
        header_.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
        header_.scene_epoch = ticket_;
        header_.frame_id = query_.frame_id;
        header_.model_epoch = provider_id;
        header_.session_epoch = query_.session_epoch;
        header_.transform_epoch = query_.transform_epoch;
        header_.topology_epoch = query_.topology_epoch;
        header_.source_id = query_.window_id;
        header_.targets_offset = sizeof(header_);
        header_.total_size = sizeof(header_);
        packet_size_ = sizeof(header_);
        snapshot_ = ticket_ | (UINT64_C(1) << 63);
        ++stats_.completed;
        ++stats_.failed;
        state_.store(WorkState::complete, std::memory_order_release);
    }

    void request_call_cancellation() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
        if (state_.load(std::memory_order_acquire) == WorkState::running && worker_thread_id_ != 0) {
            (void)CoCancelCall(worker_thread_id_, 0);
        }
    }

    [[nodiscard]] bool stop_query() noexcept {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            finish_cancelled();
            return true;
        }
        if (query_expired()) {
            complete_empty(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            return true;
        }
        return false;
    }

    void append_name(IUIAutomationElement* element, bool password, SaccadeTargetRecord* target) noexcept {
        if (password) {
            target->flags |= SACCADE_TARGET_TEXT_REDACTED;
            return;
        }
        BSTR name = nullptr;
        const HRESULT name_result = element->get_CurrentName(&name);
        if (FAILED(name_result) || name == nullptr) {
            if (FAILED(name_result)) {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                native_error_ = name_result;
            }
            if (name != nullptr) SysFreeString(name);
            return;
        }
        const uint32_t wide_size = SysStringLen(name);
        bool contains_nul = false;
        for (uint32_t index = 0; index < wide_size; ++index)
            contains_nul |= name[index] == L'\0';
        const int encoded_size = contains_nul || wide_size > static_cast<uint32_t>(std::numeric_limits<int>::max())
                                     ? 0
                                     : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name,
                                                           static_cast<int>(wide_size), nullptr, 0, nullptr, nullptr);
        if (encoded_size > 0 &&
            static_cast<uint32_t>(encoded_size) <= SACCADE_TARGET_PACKET_MAX_TEXT_BYTES - text_size_) {
            const int written =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, static_cast<int>(wide_size),
                                    reinterpret_cast<char*>(text_ + text_size_), encoded_size, nullptr, nullptr);
            if (written == encoded_size) {
                target->text = {static_cast<uint16_t>(text_size_), static_cast<uint16_t>(written)};
                text_size_ += static_cast<uint32_t>(written);
            }
        } else if (encoded_size > 0) {
            target->flags |= SACCADE_TARGET_TEXT_TRUNCATED;
            packet_flags_ |= SACCADE_TARGET_PACKET_TEXT_TRUNCATED;
        }
        SysFreeString(name);
    }

    static DWORD WINAPI worker_entry(void* context) noexcept {
        static_cast<Impl*>(context)->worker_loop();
        return 0;
    }

    void worker_loop() noexcept {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT cancellation = SUCCEEDED(initialized) ? CoEnableCallCancellation(nullptr) : initialized;
        ComPtr<IUIAutomation> automation;
        ComPtr<IUIAutomationCondition> target_condition;
        HRESULT native = SUCCEEDED(initialized) ? cancellation : initialized;
        if (SUCCEEDED(native)) {
            native = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            if (FAILED(native)) {
                native =
                    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            }
        }
        if (SUCCEEDED(native)) {
            ComPtr<IUIAutomation2> bounded;
            native = automation.As(&bounded);
            if (SUCCEEDED(native)) native = bounded->put_ConnectionTimeout(uia_connection_timeout_ms);
            if (SUCCEEDED(native)) native = bounded->put_TransactionTimeout(uia_transaction_timeout_ms);
        }
        if (SUCCEEDED(native)) {
            ComPtr<IUIAutomation6> recovery;
            if (SUCCEEDED(automation.As(&recovery))) {
                (void)recovery->put_ConnectionRecoveryBehavior(ConnectionRecoveryBehaviorOptions_Disabled);
            }
        }
        if (SUCCEEDED(native)) {
            native = create_target_condition(automation.Get(), target_condition.ReleaseAndGetAddressOf());
        }

        for (;;) {
            const DWORD waited = WaitForSingleObject(request_event_, INFINITE);
            if (waited != WAIT_OBJECT_0) {
                native_error_ = static_cast<int32_t>(GetLastError());
                result_ = SACCADE_ERROR_BACKEND;
                ready_.store(false, std::memory_order_release);
                state_.store(WorkState::failed, std::memory_order_release);
                if (done_event_ != nullptr) SetEvent(done_event_);
                break;
            }
            if (stop_requested_.load(std::memory_order_acquire)) break;
            state_.store(WorkState::running, std::memory_order_release);
            if (FAILED(native)) {
                complete_empty(native);
                SetEvent(done_event_);
                if (client_detached_.load(std::memory_order_acquire)) {
                    ticket_ = 0;
                    snapshot_ = 0;
                    state_.store(WorkState::idle, std::memory_order_release);
                }
                continue;
            }
            run_query(automation.Get(), target_condition.Get());
            SetEvent(done_event_);
            if (client_detached_.load(std::memory_order_acquire)) {
                ticket_ = 0;
                snapshot_ = 0;
                state_.store(WorkState::idle, std::memory_order_release);
            }
        }
        target_condition.Reset();
        automation.Reset();
        if (SUCCEEDED(cancellation)) {
            (void)CoDisableCallCancellation(nullptr);
        }
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
    }

    void run_query(IUIAutomation* automation, IUIAutomationCondition* target_condition) noexcept {
        target_count_ = 0;
        text_size_ = 0;
        packet_size_ = 0;
        packet_flags_ = 0;
        result_ = SACCADE_OK;
        native_error_ = 0;
        if (stop_query()) return;

        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(query_.window_id));
        ComPtr<IUIAutomationElement> root;
        HRESULT native = IsWindow(window) ? automation->ElementFromHandle(window, &root) : E_INVALIDARG;
        ComPtr<IUIAutomationElementArray> elements;
        if (SUCCEEDED(native)) {
            native = root->FindAll(TreeScope_Descendants, target_condition, &elements);
        }
        int length = 0;
        if (SUCCEEDED(native)) {
            native = elements->get_Length(&length);
        }
        if (FAILED(native)) {
            if (cancel_requested_.load(std::memory_order_acquire))
                finish_cancelled();
            else
                complete_empty(native);
            return;
        }

        const uint32_t capacity = std::min(query_.target_capacity, SACCADE_TARGET_PACKET_MAX_TARGETS);
        for (int index = 0; index < length && target_count_ < capacity; ++index) {
            if (stop_query()) return;
            ComPtr<IUIAutomationElement> element;
            if (FAILED(elements->GetElement(index, &element))) {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                continue;
            }
            if (stop_query()) return;
            RECT rect{};
            BOOL offscreen = TRUE;
            const HRESULT bounds_result = element->get_CurrentBoundingRectangle(&rect);
            const HRESULT offscreen_result = element->get_CurrentIsOffscreen(&offscreen);
            if (stop_query()) return;
            if (FAILED(bounds_result) || FAILED(offscreen_result)) {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                native_error_ = FAILED(bounds_result) ? bounds_result : offscreen_result;
                continue;
            }
            if (offscreen || rect.right <= rect.left || rect.bottom <= rect.top || !intersects(query_.scope, rect)) {
                continue;
            }
            CONTROLTYPEID control_type = UIA_CustomControlTypeId;
            BOOL enabled = FALSE;
            BOOL password = FALSE;
            const HRESULT type_result = element->get_CurrentControlType(&control_type);
            const HRESULT enabled_result = element->get_CurrentIsEnabled(&enabled);
            const HRESULT password_result = element->get_CurrentIsPassword(&password);
            if (stop_query()) return;
            if (FAILED(type_result) || FAILED(enabled_result) || FAILED(password_result)) {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                native_error_ = FAILED(type_result)      ? type_result
                                : FAILED(enabled_result) ? enabled_result
                                                         : password_result;
                continue;
            }

            SaccadeTargetRecord& target = targets_[target_count_];
            target = {};
            SAFEARRAY* runtime_id = nullptr;
            const HRESULT runtime_id_result = element->GetRuntimeId(&runtime_id);
            if (stop_query()) {
                if (runtime_id != nullptr) SafeArrayDestroy(runtime_id);
                return;
            }
            if (FAILED(runtime_id_result)) {
                packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
                native_error_ = runtime_id_result;
            }
            target.target_id = hash_runtime_id(runtime_id, query_.window_id, target_count_);
            if (runtime_id != nullptr) {
                SafeArrayDestroy(runtime_id);
            }
            target.parent_id = query_.window_id;
            target.window_id = query_.window_id;
            target.display_id = display_at((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
            target.x_q8 = q8(rect.left);
            target.y_q8 = q8(rect.top);
            target.width_q8 = q8(static_cast<double>(rect.right) - rect.left);
            target.height_q8 = q8(static_cast<double>(rect.bottom) - rect.top);
            target.safe_x_q8 = q8((static_cast<double>(rect.left) + rect.right) * 0.5);
            target.safe_y_q8 = q8((static_cast<double>(rect.top) + rect.bottom) * 0.5);
            target.confidence_q16 = UINT16_MAX;
            target.role = role_for(control_type);
            target.source_bits = SACCADE_TARGET_SOURCE_ACCESSIBILITY;
            target.capability_bits = capabilities(element.Get(), control_type);
            if (stop_query()) return;
            target.flags = enabled ? SACCADE_TARGET_ACTIONABLE | SACCADE_TARGET_APPROXIMATE : SACCADE_TARGET_DISABLED;
            target.flags |= password ? SACCADE_TARGET_SECURE : 0;
            if (!enabled || password) {
                target.capability_bits = 0;
                target.flags &= ~static_cast<uint32_t>(SACCADE_TARGET_ACTIONABLE);
            }
            target.order = target_count_;
            append_name(element.Get(), password != FALSE, &target);
            if (stop_query()) return;
            ++target_count_;
        }
        if (target_count_ == capacity && target_count_ < static_cast<uint32_t>(length)) {
            packet_flags_ |= SACCADE_TARGET_PACKET_INCOMPLETE;
        }
        if (stop_query()) return;

        header_ = {};
        header_.struct_size = sizeof(header_);
        header_.packet_version = SACCADE_TARGET_PACKET_VERSION;
        header_.target_count = target_count_;
        header_.target_stride = sizeof(SaccadeTargetRecord);
        header_.flags = packet_flags_;
        header_.coordinate_space = SACCADE_COORDINATE_SPACE_DESKTOP_Q8;
        header_.scene_epoch = ticket_;
        header_.frame_id = query_.frame_id;
        header_.model_epoch = provider_id;
        header_.session_epoch = query_.session_epoch;
        header_.transform_epoch = query_.transform_epoch;
        header_.topology_epoch = query_.topology_epoch;
        header_.source_id = query_.window_id;
        header_.targets_offset = sizeof(header_);
        packet_size_ =
            static_cast<uint32_t>(sizeof(header_) + target_count_ * sizeof(SaccadeTargetRecord) + text_size_);
        header_.total_size = packet_size_;
        snapshot_ = ticket_ | (UINT64_C(1) << 63);
        ++stats_.completed;
        stats_.targets += target_count_;
        state_.store(WorkState::complete, std::memory_order_release);
    }

    SaccadeResult refresh_windows() noexcept {
        WindowCollector collector{&windows_, 0};
        if (!EnumWindows(collect_window, reinterpret_cast<LPARAM>(&collector))) {
            native_error_ = static_cast<int32_t>(GetLastError());
            return SACCADE_ERROR_BACKEND;
        }
        window_count_ = collector.count;
        ++stats_.window_refreshes;
        return SACCADE_OK;
    }

    SaccadeAccessibilityStatus status() const noexcept {
        SaccadeAccessibilityStatus output{};
        output.ticket = ticket_;
        output.session_epoch = query_.session_epoch;
        output.transform_epoch = query_.transform_epoch;
        output.topology_epoch = query_.topology_epoch;
        output.frame_id = query_.frame_id;
        output.target_count = target_count_;
        output.required_bytes = packet_size_;
        switch (state_.load(std::memory_order_acquire)) {
        case WorkState::queued:
            output.state = SACCADE_TICKET_QUEUED;
            break;
        case WorkState::running:
            output.state = SACCADE_TICKET_RUNNING;
            break;
        case WorkState::complete:
            output.state = SACCADE_TICKET_COMPLETE;
            output.snapshot = snapshot_;
            break;
        case WorkState::cancelled:
            output.state = SACCADE_TICKET_CANCELLED;
            output.result = SACCADE_ERROR_CANCELLED;
            break;
        case WorkState::failed:
            output.state = SACCADE_TICKET_FAILED;
            output.result = result_;
            break;
        default:
            output.state = SACCADE_TICKET_FAILED;
            output.result = SACCADE_ERROR_STATE;
            break;
        }
        return output;
    }

    static Impl* from(void* context) noexcept { return static_cast<Impl*>(context); }

    static SaccadeResult SACCADE_CALL enumerate(void* context, uint32_t index, SaccadeWindowInfo* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (index == 0) {
            const SaccadeResult refreshed = state->refresh_windows();
            if (refreshed != SACCADE_OK) return refreshed;
        }
        if (index >= state->window_count_) return SACCADE_ERROR_NOT_FOUND;
        const WindowEntry& entry = state->windows_[index];
        SaccadeWindowInfo value{};
        value.stable_id = entry.id;
        value.process_id = entry.process_id;
        value.desktop_bounds = entry.bounds;
        value.title = {entry.title.data(), entry.title_size};
        return write_structure(output, value);
    }

    static SaccadeResult SACCADE_CALL request(void* context, const SaccadeAccessibilityQueryDesc* query,
                                              SaccadeTicketHandle* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || query == nullptr || output == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *output = 0;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (query->struct_size < sizeof(*query) || query->api_version != SACCADE_API_VERSION || query->window_id == 0 ||
            query->scope.width <= 0 || query->scope.height <= 0 || query->target_capacity == 0 ||
            query->target_capacity > SACCADE_TARGET_PACKET_MAX_TARGETS || query->flags != 0 ||
            query->session_epoch == 0 || query->transform_epoch == 0 || query->topology_epoch == 0 ||
            query->frame_id == 0) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        if (state->state_.load(std::memory_order_acquire) != WorkState::idle) {
            return SACCADE_ERROR_BUSY;
        }
        if (ResetEvent(state->done_event_) == 0) return SACCADE_ERROR_BACKEND;
        state->query_ = *query;
        state->ticket_ = state->next_ticket_++;
        if (state->next_ticket_ == 0 || state->next_ticket_ >= (UINT64_C(1) << 63)) {
            state->next_ticket_ = 1;
        }
        state->snapshot_ = 0;
        state->target_count_ = 0;
        state->packet_size_ = 0;
        state->query_started_ms_ = GetTickCount64();
        state->cancel_requested_.store(false, std::memory_order_relaxed);
        state->client_detached_.store(false, std::memory_order_relaxed);
        state->state_.store(WorkState::queued, std::memory_order_release);
        if (SetEvent(state->request_event_) == 0) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
            return SACCADE_ERROR_BACKEND;
        }
        ++state->stats_.requests;
        *output = state->ticket_;
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL poll(void* context, SaccadeTicketHandle ticket,
                                           SaccadeAccessibilityStatus* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (ticket == 0 || ticket != state->ticket_ ||
            state->state_.load(std::memory_order_acquire) == WorkState::idle) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        if (state->query_expired()) state->request_call_cancellation();
        const WorkState current = state->state_.load(std::memory_order_acquire);
        const SaccadeResult written = write_structure(output, state->status());
        if (written == SACCADE_OK && (current == WorkState::cancelled || current == WorkState::failed)) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
        }
        return written;
    }

    static SaccadeResult SACCADE_CALL wait(void* context, SaccadeTicketHandle ticket, uint64_t timeout_ns,
                                           SaccadeAccessibilityStatus* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (ticket == 0 || ticket != state->ticket_) return SACCADE_ERROR_STALE_HANDLE;
        ++state->stats_.waits;
        const DWORD milliseconds = wait_milliseconds(timeout_ns);
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current == WorkState::queued || current == WorkState::running) {
            const DWORD bounded = state->cancel_requested_.load(std::memory_order_acquire)
                                      ? std::min(milliseconds, cancelled_wait_ms)
                                      : milliseconds;
            const DWORD waited = WaitForSingleObject(state->done_event_, bounded);
            if (waited == WAIT_TIMEOUT) {
                if (state->cancel_requested_.load(std::memory_order_acquire)) {
                    state->client_detached_.store(true, std::memory_order_release);
                    SaccadeAccessibilityStatus cancelled = state->status();
                    cancelled.state = SACCADE_TICKET_CANCELLED;
                    cancelled.result = SACCADE_ERROR_CANCELLED;
                    const SaccadeResult written = write_structure(output, cancelled);
                    return written == SACCADE_OK ? SACCADE_ERROR_CANCELLED : written;
                }
                const SaccadeResult written = write_structure(output, state->status());
                return written == SACCADE_OK ? SACCADE_ERROR_TIMEOUT : written;
            }
            if (waited != WAIT_OBJECT_0) return SACCADE_ERROR_BACKEND;
        }
        const WorkState terminal = state->state_.load(std::memory_order_acquire);
        const SaccadeResult written = write_structure(output, state->status());
        if (written == SACCADE_OK && (terminal == WorkState::cancelled || terminal == WorkState::failed)) {
            state->ticket_ = 0;
            state->state_.store(WorkState::idle, std::memory_order_release);
        }
        return written;
    }

    static SaccadeResult SACCADE_CALL collect(void* context, SaccadeSnapshotHandle snapshot,
                                              SaccadeMutableSpanU8 output, size_t* required) noexcept {
        Impl* state = from(context);
        if (state == nullptr || required == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 ||
            snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        *required = state->packet_size_;
        if (output.data == nullptr || output.size < state->packet_size_) return SACCADE_ERROR_CAPACITY;
        std::memcpy(output.data, &state->header_, sizeof(state->header_));
        std::memcpy(output.data + sizeof(state->header_), state->targets_,
                    state->target_count_ * sizeof(SaccadeTargetRecord));
        std::memcpy(output.data + sizeof(state->header_) + state->target_count_ * sizeof(SaccadeTargetRecord),
                    state->text_, state->text_size_);
        state->stats_.copied_bytes += state->packet_size_;
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL cancel(void* context, SaccadeTicketHandle ticket) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (ticket == 0 || ticket != state->ticket_) return SACCADE_ERROR_STALE_HANDLE;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running) return SACCADE_ERROR_STATE;
        state->request_call_cancellation();
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL release(void* context, SaccadeSnapshotHandle snapshot) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        if (state->state_.load(std::memory_order_acquire) != WorkState::complete || snapshot == 0 ||
            snapshot != state->snapshot_)
            return SACCADE_ERROR_STALE_HANDLE;
        state->snapshot_ = 0;
        state->ticket_ = 0;
        state->state_.store(WorkState::idle, std::memory_order_release);
        return SACCADE_OK;
    }

    static SaccadeResult SACCADE_CALL synchronize(void* context, uint64_t timeout_ns) noexcept {
        Impl* state = from(context);
        if (state == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        const WorkState current = state->state_.load(std::memory_order_acquire);
        if (current != WorkState::queued && current != WorkState::running) return SACCADE_OK;
        if (state->query_expired()) state->request_call_cancellation();
        const DWORD requested = wait_milliseconds(timeout_ns);
        const DWORD milliseconds = state->cancel_requested_.load(std::memory_order_acquire)
                                       ? std::min(requested, cancelled_wait_ms)
                                       : requested;
        const DWORD waited = WaitForSingleObject(state->done_event_, milliseconds);
        if (waited == WAIT_TIMEOUT && state->cancel_requested_.load(std::memory_order_acquire)) {
            state->client_detached_.store(true, std::memory_order_release);
            return SACCADE_ERROR_CANCELLED;
        }
        if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_TIMEOUT;
        return waited == WAIT_OBJECT_0 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    }

    static SaccadeResult SACCADE_CALL memory(void* context, SaccadeMemoryStats* output) noexcept {
        Impl* state = from(context);
        if (state == nullptr || output == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
        if (!state->ready()) return SACCADE_ERROR_STATE;
        SaccadeMemoryStats value{};
        constexpr uint64_t target_bytes =
            static_cast<uint64_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
        value.host_committed = sizeof(Impl) + target_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;
        value.host_reserved = sizeof(Impl) + target_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;
        value.copied_bytes = state->stats_.copied_bytes;
        value.high_water_bytes =
            sizeof(SaccadeTargetPacketHeader) +
            static_cast<uint64_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord) +
            SACCADE_TARGET_PACKET_MAX_TEXT_BYTES;
        return write_structure(output, value);
    }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

AccessibilityProvider::AccessibilityProvider() noexcept {
    void* const memory = VirtualAlloc(nullptr, sizeof(Impl), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory != nullptr) state_ = new (memory) Impl{};
}

AccessibilityProvider::~AccessibilityProvider() {
    if (state_ == nullptr) return;
    if (shutdown() != SACCADE_OK) {
        // A UIA call that ignores both its timeout and COM cancellation keeps this
        // control block alive until process teardown instead of racing its worker.
        state_ = nullptr;
        return;
    }
    state_->~Impl();
    VirtualFree(state_, 0, MEM_RELEASE);
    state_ = nullptr;
}

AccessibilityProvider::Impl& AccessibilityProvider::impl() noexcept {
    return *state_;
}

const AccessibilityProvider::Impl& AccessibilityProvider::impl() const noexcept {
    return *state_;
}

SaccadeResult AccessibilityProvider::initialize() noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (state_ == nullptr) return SACCADE_ERROR_BACKEND;
    Impl& state = impl();
    constexpr size_t target_bytes =
        static_cast<size_t>(SACCADE_TARGET_PACKET_MAX_TARGETS) * sizeof(SaccadeTargetRecord);
    state.targets_ = static_cast<SaccadeTargetRecord*>(VirtualAlloc(
        nullptr, target_bytes + SACCADE_TARGET_PACKET_MAX_TEXT_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (state.targets_ == nullptr) return SACCADE_ERROR_BACKEND;
    state.text_ = reinterpret_cast<uint8_t*>(state.targets_) + target_bytes;
    state.request_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state.done_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.request_event_ == nullptr || state.done_event_ == nullptr) {
        shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    state.worker_ = CreateThread(nullptr, 0, &Impl::worker_entry, &state, 0, &state.worker_thread_id_);
    if (state.worker_ == nullptr) {
        shutdown();
        return SACCADE_ERROR_BACKEND;
    }
    state.stop_requested_.store(false, std::memory_order_relaxed);
    state.cancel_requested_.store(false, std::memory_order_relaxed);
    state.state_.store(WorkState::idle, std::memory_order_relaxed);
    state.ready_.store(true, std::memory_order_release);
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::shutdown() noexcept {
    if (state_ == nullptr) return SACCADE_OK;
    Impl& state = impl();
    state.ready_.store(false, std::memory_order_release);
    state.stop_requested_.store(true, std::memory_order_release);
    state.request_call_cancellation();
    if (state.worker_ != nullptr) {
        if (state.request_event_ != nullptr) SetEvent(state.request_event_);
        const DWORD waited = WaitForSingleObject(state.worker_, shutdown_wait_ms);
        if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_TIMEOUT;
        if (waited != WAIT_OBJECT_0) return SACCADE_ERROR_BACKEND;
        CloseHandle(state.worker_);
        state.worker_ = nullptr;
        state.worker_thread_id_ = 0;
    }
    state.ticket_ = 0;
    state.snapshot_ = 0;
    state.query_ = {};
    if (state.done_event_ != nullptr) {
        CloseHandle(state.done_event_);
        state.done_event_ = nullptr;
    }
    if (state.request_event_ != nullptr) {
        CloseHandle(state.request_event_);
        state.request_event_ = nullptr;
    }
    if (state.targets_ != nullptr) {
        VirtualFree(state.targets_, 0, MEM_RELEASE);
        state.targets_ = nullptr;
        state.text_ = nullptr;
    }
    state.state_.store(WorkState::idle, std::memory_order_relaxed);
    initialized_ = false;
    return SACCADE_OK;
}

SaccadeAccessibilityProviderDesc AccessibilityProvider::descriptor() noexcept {
    SaccadeAccessibilityOps ops{};
    ops.struct_size = sizeof(ops);
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_windows = &Impl::enumerate;
    ops.request = &Impl::request;
    ops.poll = &Impl::poll;
    ops.wait = &Impl::wait;
    ops.collect = &Impl::collect;
    ops.cancel = &Impl::cancel;
    ops.release = &Impl::release;
    ops.synchronize = &Impl::synchronize;
    ops.memory_stats = &Impl::memory;

    SaccadeAccessibilityProviderDesc output{};
    output.struct_size = sizeof(output);
    output.api_version = SACCADE_API_VERSION;
    output.info.struct_size = sizeof(output.info);
    output.info.api_version = SACCADE_API_VERSION;
    output.info.family = SACCADE_PROVIDER_FAMILY_ACCESSIBILITY;
    output.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_ASYNC | SACCADE_PROVIDER_CAPABILITY_CANCELLATION;
    output.info.stable_id = provider_id;
    output.info.name = {reinterpret_cast<const uint8_t*>(provider_name), sizeof(provider_name) - 1};
    output.context = state_;
    output.ops = ops;
    return output;
}

SaccadeResult AccessibilityProvider::read_stats(AccessibilityProviderStats* output) const noexcept {
    if (output == nullptr || state_ == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running) {
        return SACCADE_ERROR_BUSY;
    }
    *output = impl().stats_;
    return SACCADE_OK;
}

SaccadeResult AccessibilityProvider::read_last_native_error(int32_t* output) const noexcept {
    if (output == nullptr || state_ == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const WorkState current = impl().state_.load(std::memory_order_acquire);
    if (current == WorkState::queued || current == WorkState::running) {
        return SACCADE_ERROR_BUSY;
    }
    *output = impl().native_error_;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows

#include "application/binding_editor.hpp"
#include "application/debugger_layout.hpp"
#include "application/desktop_host.hpp"
#include "application/settings_controller.hpp"
#if defined(SACCADE_HAS_WINDOWS_ML)
#include "application/recovery_schedule.hpp"
#include "apps/model_trust.hpp"
#include "model/p256_verifier.hpp"
#include "platform/windows/agent_pipe.hpp"
#include "platform/windows/desktop_pipeline.hpp"
#include "platform/windows/runtime_scheduling.hpp"
#endif
#include "platform/windows/global_hotkeys.hpp"
#include "platform/windows/keyboard.hpp"
#include "platform/windows/operating_system.hpp"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

using saccade::application::Command;
using saccade::application::CommandEvent;
using saccade::application::DesktopHost;
using saccade::platform::windows::GlobalHotkeys;

constexpr wchar_t window_class_name[] = L"SaccadeNotificationHost";
constexpr wchar_t settings_class_name[] = L"SaccadeSettingsWindow";
constexpr wchar_t bindings_class_name[] = L"SaccadeBindingsWindow";
constexpr wchar_t diagnostics_class_name[] = L"SaccadeDiagnosticsWindow";
constexpr wchar_t application_name[] = L"Saccade";
constexpr UINT tray_message = WM_APP + 1U;
constexpr UINT menu_settings = 1;
constexpr UINT menu_suspend = 2;
constexpr UINT menu_restart = 3;
constexpr UINT menu_quit = 4;
constexpr UINT menu_diagnostics = 5;
constexpr UINT tray_identifier = 1;
constexpr UINT_PTR diagnostics_timer_identifier = 2;
constexpr uint64_t runtime_tick_period_ns = UINT64_C(8'333'333);
constexpr int64_t waitable_timer_unit_ns = 100;
constexpr int settings_width = 600;
constexpr int settings_height = 760;
constexpr int settings_label_x = 24;
constexpr int settings_control_x = 220;
constexpr int settings_row_height = 34;
constexpr int settings_control_width = 330;
constexpr int settings_control_height = 26;

enum SettingsControl : int {
    settings_hint_alphabet = 100,
    settings_hint_language,
    settings_hint_priority,
    settings_hint_placement,
    settings_hint_sorting,
    settings_source,
    settings_scope,
    settings_monitor,
    settings_confidence,
    settings_text_confidence,
    settings_duplicate_iou,
    settings_minimum_width,
    settings_minimum_height,
    settings_merge_policy,
    settings_grid_rows,
    settings_grid_columns,
    settings_grid_margin_x,
    settings_grid_margin_y,
    settings_timeout,
    settings_hold_duration,
    settings_drag_duration,
    settings_scroll_duration,
    settings_scroll_vertical,
    settings_scroll_horizontal,
    settings_click_control,
    settings_click_alt,
    settings_click_shift,
    settings_click_meta,
    settings_initial_mode,
    settings_final_pointer,
    settings_pointer_movement,
    settings_anchor_x,
    settings_anchor_y,
    settings_font_family,
    settings_placement,
    settings_theme,
    settings_font_size,
    settings_font_weight,
    settings_label_color,
    settings_background_color,
    settings_outline_color,
    settings_glow_color,
    settings_outline_width,
    settings_glow_radius,
    settings_compute,
    settings_compute_device,
    settings_animate,
    settings_reduced_motion,
    settings_reset_page,
    settings_reset_page_button,
    settings_import,
    settings_export,
    settings_bindings,
    settings_apply,
    settings_defaults,
    settings_cancel
};

enum BindingControl : int {
    binding_command = 300,
    binding_key,
    binding_control,
    binding_alt,
    binding_shift,
    binding_meta,
    binding_session_only,
    binding_list,
    binding_set,
    binding_remove,
    binding_done
};

constexpr int binding_visual_first = 1000;
constexpr int binding_visual_count = static_cast<int>(saccade::application::binding_key_count);

enum DiagnosticsControl : int {
    diagnostics_view = 500,
    diagnostics_text,
    diagnostics_capture,
    diagnostics_dry_run,
    diagnostics_replay,
    diagnostics_clear,
    diagnostics_fault,
    diagnostics_arm_fault
};

LPCWSTR application_icon() noexcept {
    return MAKEINTRESOURCEW(IDI_SACCADE);
}

bool settings_path(std::array<wchar_t, 32768>* output) noexcept {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", output->data(), static_cast<DWORD>(output->size()));
    if (size == 0 || size >= output->size()) return false;
    constexpr wchar_t directory[] = L"\\Saccade";
    constexpr wchar_t file[] = L"\\settings.bin";
    if (size + std::size(directory) + std::size(file) > output->size()) return false;
    std::memcpy(output->data() + size, directory, sizeof(directory));
    (void)CreateDirectoryW(output->data(), nullptr);
    const size_t directory_size = size + std::size(directory) - 1U;
    std::memcpy(output->data() + directory_size, file, sizeof(file));
    return true;
}

bool load_settings(saccade::application::SettingsDocument* output) noexcept {
    std::array<wchar_t, 32768> path{};
    if (!settings_path(&path)) return false;
    HANDLE file =
        CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
    DWORD size = 0;
    const BOOL read = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr);
    const BOOL closed = CloseHandle(file);
    return read != 0 && closed != 0 && size != 0 &&
           saccade::application::decode_settings({bytes.data(), size}, output) == SACCADE_OK;
}

SaccadeResult save_settings(const saccade::application::SettingsDocument& settings) noexcept {
    std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
    size_t size = 0;
    SaccadeResult result = saccade::application::encode_settings(settings, {bytes.data(), bytes.size()}, &size);
    if (result != SACCADE_OK) return result;
    std::array<wchar_t, 32768> path{};
    if (!settings_path(&path)) return SACCADE_ERROR_BACKEND;
    std::array<wchar_t, 32768> temporary = path;
    constexpr wchar_t suffix[] = L".tmp";
    const size_t path_size = std::wcslen(temporary.data());
    if (path_size + std::size(suffix) > temporary.size()) return SACCADE_ERROR_CAPACITY;
    std::memcpy(temporary.data() + path_size, suffix, sizeof(suffix));
    HANDLE file = CreateFileW(temporary.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SACCADE_ERROR_BACKEND;
    DWORD written = 0;
    const BOOL saved = WriteFile(file, bytes.data(), static_cast<DWORD>(size), &written, nullptr);
    const BOOL flushed = FlushFileBuffers(file);
    const BOOL closed = CloseHandle(file);
    if (saved == 0 || flushed == 0 || closed == 0 || written != size) {
        (void)DeleteFileW(temporary.data());
        return SACCADE_ERROR_BACKEND;
    }
    return MoveFileExW(temporary.data(), path.data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0
               ? SACCADE_OK
               : SACCADE_ERROR_BACKEND;
}

class Application final {
  public:
    int run(HINSTANCE instance, int show_command) noexcept;

  private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK settings_proc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK bindings_proc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK diagnostics_proc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static SaccadeResult dispatch(void*, Command, uint64_t) noexcept;
    static SaccadeResult set_suspended(void*, bool) noexcept;
    static SaccadeResult apply_settings(void*, const saccade::application::SettingsDocument&) noexcept;
    static SaccadeResult neutralize(void*) noexcept;
    static void observe(void*, uint64_t) noexcept;
    static void observe_command(void*, uint64_t) noexcept;
    static SaccadeResult open_settings(void*) noexcept;
    static SaccadeResult restart(void*) noexcept;
    static SaccadeResult quit(void*) noexcept;
    static bool route_key(void*, const saccade::application::KeyEvent&) noexcept;
#if defined(SACCADE_HAS_WINDOWS_ML)
    static SaccadeResult process_agent(void*, SaccadeSpanU8, SaccadeAgentCapabilityBits, uint64_t, SaccadeMutableSpanU8,
                                       size_t*) noexcept;
#endif

    SaccadeResult initialize(HINSTANCE, int) noexcept;
    void shutdown() noexcept;
    void show_menu() noexcept;
    void update_tray() noexcept;
    uint64_t timestamp_ns() const noexcept;
    SaccadeResult request_restart() noexcept;
    SaccadeResult launch_replacement() noexcept;
    SaccadeResult initialize_pipeline() noexcept;
    SaccadeResult shutdown_pipeline() noexcept;
    void begin_pipeline_recovery(SaccadeResult, uint64_t) noexcept;
    void advance_pipeline() noexcept;
    SaccadeResult arm_runtime_timer(uint64_t) noexcept;
    SaccadeResult set_input_available(bool available) noexcept;
    void show_diagnostics() noexcept;
    void create_diagnostics_view() noexcept;
    void layout_diagnostics_view(int32_t width, int32_t height) noexcept;
    void refresh_diagnostics() noexcept;
    SaccadeResult show_settings() noexcept;
    void create_settings_view() noexcept;
    SaccadeResult stage_settings_view() noexcept;
    SaccadeResult reset_settings_view_page() noexcept;
    SaccadeResult commit_settings(bool defaults) noexcept;
    SaccadeResult import_settings_document() noexcept;
    SaccadeResult export_settings_document() noexcept;
    SaccadeResult show_binding_editor() noexcept;
    void create_binding_view() noexcept;
    void load_selected_binding() noexcept;
    void refresh_binding_list() noexcept;
    SaccadeResult modify_binding(bool remove) noexcept;

    DesktopHost host_{};
    GlobalHotkeys hotkeys_{};
    saccade::application::SettingsController settings_{};
#if defined(SACCADE_HAS_WINDOWS_ML)
    saccade::model::P256ArtifactVerifier verifier_{};
    saccade::platform::windows::RuntimeScheduling runtime_scheduling_{};
    saccade::platform::windows::DesktopPipeline pipeline_{};
    saccade::platform::windows::AgentPipe agent_pipe_{};
    saccade::platform::windows::AgentPipeStorage agent_pipe_storage_{};
    std::array<char, 32768> artifact_path_{};
    std::array<char, 32768> shader_directory_{};
    saccade::application::RecoverySchedule pipeline_recovery_{};
    HANDLE runtime_timer_ = nullptr;
    uint64_t next_runtime_tick_ns_ = 0;
#endif
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND settings_window_ = nullptr;
    HWND bindings_window_ = nullptr;
    HWND diagnostics_window_ = nullptr;
    std::array<wchar_t, 256> debugger_operation_{};
    int settings_scroll_y_ = 0;
    int settings_scroll_max_ = 0;
    NOTIFYICONDATAW tray_{};
    uint64_t counter_frequency_ = 0;
    SaccadeResult fault_ = SACCADE_OK;
    bool scene_incomplete_ = false;
    bool tray_added_ = false;
    bool host_initialized_ = false;
    bool hotkeys_initialized_ = false;
    bool settings_initialized_ = false;
#if defined(SACCADE_HAS_WINDOWS_ML)
    bool verifier_initialized_ = false;
    bool pipeline_initialized_ = false;
    bool pipeline_cleanup_required_ = false;
    bool agent_pipe_initialized_ = false;
    bool session_notifications_ = false;
#endif
    bool initialized_ = false;
    bool restart_requested_ = false;
    DPI_AWARENESS_CONTEXT previous_dpi_context_ = nullptr;

    static thread_local Application* owner_;
};

thread_local Application* Application::owner_ = nullptr;

SaccadeResult Application::dispatch(void* context, Command command, uint64_t now_ns) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (!app->pipeline_initialized_) return app->fault_;
    if (command == Command::type_text) {
        if (OpenClipboard(app->window_) == 0) return SACCADE_ERROR_BUSY;
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        const auto* wide = handle == nullptr ? nullptr : static_cast<const wchar_t*>(GlobalLock(handle));
        std::array<uint8_t, saccade::interaction::maximum_action_payload_bytes> utf8{};
        SaccadeResult staged = SACCADE_ERROR_NOT_FOUND;
        if (wide != nullptr) {
            const size_t wide_size = std::wcslen(wide);
            const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                                     wide_size <= INT_MAX ? static_cast<int>(wide_size) : 0, nullptr, 0,
                                                     nullptr, nullptr);
            if (required > 0 && static_cast<size_t>(required) <= utf8.size()) {
                const int converted = WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, wide, static_cast<int>(wide_size),
                    reinterpret_cast<char*>(utf8.data()), static_cast<int>(utf8.size()), nullptr, nullptr);
                if (converted == required)
                    staged = app->pipeline_.set_text({utf8.data(), static_cast<size_t>(required)});
            } else if (required > 0) {
                staged = SACCADE_ERROR_CAPACITY;
            }
            (void)GlobalUnlock(handle);
        }
        (void)CloseClipboard();
        if (staged != SACCADE_OK) return staged;
    }
    const SaccadeResult result = app->pipeline_.request(command, now_ns);
    if (result != SACCADE_OK) {
        app->fault_ = result;
        app->update_tray();
    }
    return result;
#else
    (void)command;
    (void)now_ns;
    return app->fault_;
#endif
}

SaccadeResult Application::set_suspended(void* context, bool value) noexcept {
    auto* app = static_cast<Application*>(context);
    const SaccadeResult result = app->hotkeys_.set_suspended(value);
    if (result == SACCADE_OK) app->update_tray();
    return result;
}

SaccadeResult Application::apply_settings(void* context,
                                          const saccade::application::SettingsDocument& settings) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (app->pipeline_initialized_) {
        const SaccadeResult applied = app->pipeline_.apply_settings(settings, app->timestamp_ns());
        if (applied != SACCADE_OK) return applied;
    }
#endif
    const SaccadeResult replaced = app->hotkeys_.replace(settings.bindings.data(), settings.binding_count);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (replaced != SACCADE_OK && app->pipeline_initialized_)
        (void)app->pipeline_.apply_settings(app->settings_.current(), app->timestamp_ns());
#endif
    if (replaced != SACCADE_OK || !app->settings_initialized_) return replaced;
    const SaccadeResult saved = save_settings(settings);
    if (saved == SACCADE_OK) return SACCADE_OK;
    (void)app->hotkeys_.replace(app->settings_.current().bindings.data(), app->settings_.current().binding_count);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (app->pipeline_initialized_) (void)app->pipeline_.apply_settings(app->settings_.current(), app->timestamp_ns());
#endif
    return saved;
}

SaccadeResult Application::neutralize(void* context) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    return app->pipeline_initialized_ ? app->pipeline_.observe_physical_input(app->timestamp_ns()) : SACCADE_OK;
#else
    (void)app;
    return SACCADE_OK;
#endif
}

void Application::observe(void* context, uint64_t now_ns) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (app->pipeline_initialized_) (void)app->pipeline_.observe_physical_input(now_ns);
#else
    (void)app;
    (void)now_ns;
#endif
}

void Application::observe_command(void* context, uint64_t) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (app->pipeline_initialized_) (void)app->pipeline_.neutralize_synthetic_input();
#else
    (void)app;
#endif
}

bool Application::route_key(void* context, const saccade::application::KeyEvent& event) noexcept {
    auto* app = static_cast<Application*>(context);
#if defined(SACCADE_HAS_WINDOWS_ML)
    return app->pipeline_initialized_ && app->pipeline_.route_key(event);
#else
    (void)app;
    (void)event;
    return false;
#endif
}

#if defined(SACCADE_HAS_WINDOWS_ML)
SaccadeResult Application::process_agent(void* context, SaccadeSpanU8 request,
                                         SaccadeAgentCapabilityBits client_capabilities, uint64_t now_ns,
                                         SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    auto* app = static_cast<Application*>(context);
    return app->pipeline_initialized_
               ? app->pipeline_.process_agent(request, client_capabilities, now_ns, output, output_size)
               : SACCADE_ERROR_STATE;
}
#endif

SaccadeResult Application::open_settings(void* context) noexcept {
    return static_cast<Application*>(context)->show_settings();
}

SaccadeResult Application::show_settings() noexcept {
    if (settings_window_ != nullptr) {
        (void)SetForegroundWindow(settings_window_);
        return SACCADE_OK;
    }
    if (settings_.editing()) (void)settings_.cancel();
    const SaccadeResult begun = settings_.begin_edit();
    if (begun != SACCADE_OK) return begun;
    RECT desktop{};
    (void)SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    const int x = desktop.left + (desktop.right - desktop.left - settings_width) / 2;
    const int y = desktop.top + (desktop.bottom - desktop.top - settings_height) / 2;
    settings_scroll_y_ = 0;
    settings_scroll_max_ = 0;
    settings_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, settings_class_name, L"Saccade settings",
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VSCROLL, x, y, settings_width,
                                       settings_height, window_, nullptr, instance_, this);
    if (settings_window_ == nullptr) {
        (void)settings_.cancel();
        return SACCADE_ERROR_BACKEND;
    }
    ShowWindow(settings_window_, SW_SHOW);
    (void)SetForegroundWindow(settings_window_);
    return SACCADE_OK;
}

void Application::create_settings_view() noexcept {
    const auto& settings = settings_.staged();
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    int row = 0;
    auto control = [&](LPCWSTR klass, LPCWSTR text, DWORD style, int identifier, int x, int width,
                       int height = settings_control_height) noexcept -> HWND {
        HWND item =
            CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, x,
                            20 + row * settings_row_height - settings_scroll_y_, width, height, settings_window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance_, nullptr);
        if (item != nullptr) (void)SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return item;
    };
    auto field = [&](LPCWSTR label, int identifier, LPCWSTR value) noexcept {
        (void)control(L"STATIC", label, SS_RIGHT, 0, settings_label_x, 175);
        (void)control(L"EDIT", value, WS_BORDER | ES_AUTOHSCROLL, identifier, settings_control_x,
                      settings_control_width);
        ++row;
    };
    auto combo = [&](LPCWSTR label, int identifier, const wchar_t* const* items, uint32_t count,
                     uint32_t selected) noexcept {
        (void)control(L"STATIC", label, SS_RIGHT, 0, settings_label_x, 175);
        HWND item = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, identifier, settings_control_x,
                            settings_control_width, 220);
        for (uint32_t index = 0; item != nullptr && index < count; ++index)
            (void)SendMessageW(item, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[index]));
        if (item != nullptr) (void)SendMessageW(item, CB_SETCURSEL, selected, 0);
        ++row;
    };
    auto unsigned_text = [](uint64_t value, bool hexadecimal, wchar_t* output, size_t capacity) noexcept {
        if (hexadecimal)
            (void)swprintf_s(output, capacity, L"0x%llX", static_cast<unsigned long long>(value));
        else
            (void)swprintf_s(output, capacity, L"%llu", static_cast<unsigned long long>(value));
    };
    std::array<wchar_t, 128> value{};
    std::array<wchar_t, saccade::interaction::maximum_hint_alphabet + 1> alphabet{};
    for (uint32_t index = 0; index < settings.hints.alphabet_count; ++index)
        alphabet[index] = static_cast<wchar_t>(settings.hints.alphabet[index]);
    field(L"Hint alphabet", settings_hint_alphabet, alphabet.data());
    std::array<wchar_t, 128> language{};
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, settings.hints.language.data(), -1, language.data(),
                              static_cast<int>(language.size()));
    field(L"Hint language", settings_hint_language, language.data());
    constexpr const wchar_t* priorities[]{L"Scene order", L"Pointer", L"Scope center", L"Randomized"};
    combo(L"Hint priority", settings_hint_priority, priorities, 4, static_cast<uint32_t>(settings.hints.priority));
    constexpr const wchar_t* placements[]{L"Automatic", L"Above", L"Below", L"Left", L"Right"};
    combo(L"Hint placement", settings_hint_placement, placements, 5, static_cast<uint32_t>(settings.hints.placement));
    constexpr const wchar_t* sorting[]{L"Sorted", L"Randomized"};
    combo(L"Hint sorting", settings_hint_sorting, sorting, 2, static_cast<uint32_t>(settings.hints.sorting));

    constexpr const wchar_t* sources[]{L"Pixel", L"Semantic", L"Grid", L"Fused"};
    combo(L"Target source", settings_source, sources, 4, static_cast<uint32_t>(settings.source));
    unsigned_text(settings.detector.confidence_q16, false, value.data(), value.size());
    field(L"Confidence (Q16)", settings_confidence, value.data());
    unsigned_text(settings.detector.text_sensitivity_q16, false, value.data(), value.size());
    field(L"Text sensitivity (Q16)", settings_text_confidence, value.data());
    unsigned_text(settings.detector.duplicate_iou_q16, false, value.data(), value.size());
    field(L"Duplicate IoU (Q16)", settings_duplicate_iou, value.data());
    unsigned_text(settings.detector.minimum_width_q8, false, value.data(), value.size());
    field(L"Minimum width (Q8)", settings_minimum_width, value.data());
    unsigned_text(settings.detector.minimum_height_q8, false, value.data(), value.size());
    field(L"Minimum height (Q8)", settings_minimum_height, value.data());
    constexpr const wchar_t* merge[]{L"Balanced", L"Text first", L"Controls first", L"Disabled"};
    combo(L"Merge policy", settings_merge_policy, merge, 4, static_cast<uint32_t>(settings.detector.merge_policy));
    unsigned_text(settings.grid.rows, false, value.data(), value.size());
    field(L"Grid rows", settings_grid_rows, value.data());
    unsigned_text(settings.grid.columns, false, value.data(), value.size());
    field(L"Grid columns", settings_grid_columns, value.data());
    unsigned_text(settings.grid.margin_x_q8, false, value.data(), value.size());
    field(L"Grid margin X (Q8)", settings_grid_margin_x, value.data());
    unsigned_text(settings.grid.margin_y_q8, false, value.data(), value.size());
    field(L"Grid margin Y (Q8)", settings_grid_margin_y, value.data());

    constexpr const wchar_t* scopes[]{L"Desktop", L"Active window", L"Monitor"};
    combo(L"Scope", settings_scope, scopes, 3, static_cast<uint32_t>(settings.scope));
    unsigned_text(settings.monitor_stable_id, true, value.data(), value.size());
    field(L"Monitor stable ID", settings_monitor, value.data());

    constexpr const wchar_t* final_positions[]{L"Target", L"Original position", L"Anchor"};
    combo(L"Final pointer", settings_final_pointer, final_positions, 3,
          static_cast<uint32_t>(settings.pointer.final_position));
    unsigned_text(settings.pointer.movement_duration_ms, false, value.data(), value.size());
    field(L"Movement (ms)", settings_pointer_movement, value.data());
    (void)swprintf_s(value.data(), value.size(), L"%d", settings.pointer.anchor_x_q8);
    field(L"Anchor X (Q8)", settings_anchor_x, value.data());
    (void)swprintf_s(value.data(), value.size(), L"%d", settings.pointer.anchor_y_q8);
    field(L"Anchor Y (Q8)", settings_anchor_y, value.data());
    constexpr const wchar_t* modes[]{L"Single", L"Dual", L"Multi", L"Path"};
    combo(L"Initial mode", settings_initial_mode, modes, 4, static_cast<uint32_t>(settings.actions.initial_mode) - 1U);
    unsigned_text(settings.actions.timeout_ms, false, value.data(), value.size());
    field(L"Timeout (ms)", settings_timeout, value.data());
    unsigned_text(settings.actions.hold_duration_ms, false, value.data(), value.size());
    field(L"Hold (ms)", settings_hold_duration, value.data());
    unsigned_text(settings.actions.drag_duration_ms, false, value.data(), value.size());
    field(L"Drag (ms)", settings_drag_duration, value.data());
    unsigned_text(settings.actions.scroll_duration_ms, false, value.data(), value.size());
    field(L"Continuous scroll lease (ms, 0 = 250)", settings_scroll_duration, value.data());
    (void)swprintf_s(value.data(), value.size(), L"%d", settings.actions.scroll_vertical_q8);
    field(L"Vertical scroll (Q8)", settings_scroll_vertical, value.data());
    (void)swprintf_s(value.data(), value.size(), L"%d", settings.actions.scroll_horizontal_q8);
    field(L"Horizontal scroll (Q8)", settings_scroll_horizontal, value.data());
    (void)control(L"STATIC", L"Click modifiers", SS_RIGHT, 0, settings_label_x, 175);
    constexpr int modifier_width = 78;
    const int modifier_ids[]{settings_click_control, settings_click_alt, settings_click_shift, settings_click_meta};
    const wchar_t* modifier_names[]{L"Control", L"Alt", L"Shift", L"Meta"};
    const uint32_t modifier_bits[]{SACCADE_INPUT_MODIFIER_CONTROL, SACCADE_INPUT_MODIFIER_ALT,
                                   SACCADE_INPUT_MODIFIER_SHIFT, SACCADE_INPUT_MODIFIER_META};
    for (uint32_t index = 0; index < 4; ++index) {
        HWND item = control(L"BUTTON", modifier_names[index], BS_AUTOCHECKBOX, modifier_ids[index],
                            settings_control_x + static_cast<int>(index) * modifier_width, modifier_width);
        if (item != nullptr)
            (void)SendMessageW(
                item, BM_SETCHECK,
                (settings.actions.click_modifiers & modifier_bits[index]) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    ++row;

    std::array<wchar_t, 128> font_family{};
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, settings.appearance.font_family.data(), -1,
                              font_family.data(), static_cast<int>(font_family.size()));
    field(L"Font family", settings_font_family, font_family.data());
    combo(L"Label placement", settings_placement, placements, 5, static_cast<uint32_t>(settings.appearance.placement));
    constexpr const wchar_t* themes[]{L"System", L"High contrast", L"Light", L"Dark", L"Custom"};
    combo(L"Theme", settings_theme, themes, 5, static_cast<uint32_t>(settings.appearance.theme));
    unsigned_text(settings.appearance.font_size_q8, false, value.data(), value.size());
    field(L"Font size (Q8)", settings_font_size, value.data());
    unsigned_text(settings.appearance.font_weight, false, value.data(), value.size());
    field(L"Font weight", settings_font_weight, value.data());
    unsigned_text(settings.appearance.label_rgba, true, value.data(), value.size());
    field(L"Label RGBA", settings_label_color, value.data());
    unsigned_text(settings.appearance.background_rgba, true, value.data(), value.size());
    field(L"Background RGBA", settings_background_color, value.data());
    unsigned_text(settings.appearance.outline_rgba, true, value.data(), value.size());
    field(L"Outline RGBA", settings_outline_color, value.data());
    unsigned_text(settings.appearance.glow_rgba, true, value.data(), value.size());
    field(L"Glow RGBA", settings_glow_color, value.data());
    unsigned_text(settings.appearance.outline_width_q8, false, value.data(), value.size());
    field(L"Outline width (Q8)", settings_outline_width, value.data());
    unsigned_text(settings.appearance.glow_radius_q8, false, value.data(), value.size());
    field(L"Glow radius (Q8)", settings_glow_radius, value.data());

    (void)control(L"STATIC", L"Motion", SS_RIGHT, 0, settings_label_x, 175);
    HWND animate = control(L"BUTTON", L"Animate overlay", BS_AUTOCHECKBOX, settings_animate, settings_control_x, 155);
    if (animate != nullptr)
        (void)SendMessageW(
            animate, BM_SETCHECK,
            (settings.flags & saccade::application::settings_animate_overlay) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND reduced =
        control(L"BUTTON", L"Reduced motion", BS_AUTOCHECKBOX, settings_reduced_motion, settings_control_x + 165, 155);
    if (reduced != nullptr)
        (void)SendMessageW(
            reduced, BM_SETCHECK,
            (settings.flags & saccade::application::settings_reduced_motion) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    ++row;

    constexpr const wchar_t* compute[]{L"Automatic", L"CPU only", L"CPU + GPU", L"CPU + accelerator", L"Named device"};
    combo(L"Compute", settings_compute, compute, 5, static_cast<uint32_t>(settings.compute.policy));
    unsigned_text(settings.compute.device_stable_id, true, value.data(), value.size());
    field(L"Compute device ID", settings_compute_device, value.data());

    constexpr const wchar_t* pages[]{L"Bindings",   L"Hints",  L"Detector", L"Scope", L"Pointer and actions",
                                     L"Appearance", L"Compute"};
    combo(L"Reset page", settings_reset_page, pages, 7, 1);
    (void)control(L"BUTTON", L"Reset selected page", BS_PUSHBUTTON, settings_reset_page_button, settings_control_x,
                  170);
    ++row;

#if defined(SACCADE_HAS_WINDOWS_ML)
    const auto stats = pipeline_.stats();
    wchar_t diagnostics[160]{};
    (void)swprintf_s(diagnostics, L"Frames %llu   Activations %llu   Overlay %llu   Failures %llu",
                     stats.frames_offered, stats.activations, stats.overlay_presents, stats.failures);
    (void)control(L"STATIC", diagnostics, SS_LEFT, 0, settings_label_x, settings_control_width + 180);
#endif
    ++row;
    (void)control(L"BUTTON", L"Import...", BS_PUSHBUTTON, settings_import, settings_label_x, 130);
    (void)control(L"BUTTON", L"Export...", BS_PUSHBUTTON, settings_export, settings_label_x + 140, 130);
    (void)control(L"BUTTON", L"Bindings...", BS_PUSHBUTTON, settings_bindings, settings_label_x + 280, 130);
    ++row;
    (void)control(L"BUTTON", L"Restore Defaults", BS_PUSHBUTTON, settings_defaults, settings_label_x, 130);
    (void)control(L"BUTTON", L"Cancel", BS_PUSHBUTTON, settings_cancel, settings_control_x + 70, 100);
    (void)control(L"BUTTON", L"Apply", BS_DEFPUSHBUTTON, settings_apply, settings_control_x + 180, 120);
    ++row;

    RECT client{};
    (void)GetClientRect(settings_window_, &client);
    const int content_height = 20 + row * settings_row_height;
    const int page_height = client.bottom - client.top;
    settings_scroll_max_ = std::max(0, content_height - page_height);
    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    scroll.nMin = 0;
    scroll.nMax = content_height - 1;
    scroll.nPage = static_cast<UINT>(std::max(1, page_height));
    scroll.nPos = settings_scroll_y_;
    (void)SetScrollInfo(settings_window_, SB_VERT, &scroll, TRUE);
}

SaccadeResult Application::import_settings_document() noexcept {
    std::array<wchar_t, 32768> path{};
    constexpr wchar_t filter[] = L"Saccade settings\0*.bin\0All files\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = settings_window_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) == 0) return SACCADE_ERROR_CANCELLED;
    HANDLE file =
        CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SACCADE_ERROR_BACKEND;
    std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size) == 0 || file_size.QuadPart <= 0 ||
        static_cast<uint64_t>(file_size.QuadPart) > bytes.size()) {
        (void)CloseHandle(file);
        return SACCADE_ERROR_CAPACITY;
    }
    DWORD size = 0;
    const BOOL read = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr);
    const BOOL closed = CloseHandle(file);
    if (read == 0 || closed == 0 || size == 0) return SACCADE_ERROR_BACKEND;
    const SaccadeResult imported = settings_.import_document({bytes.data(), size});
    return imported == SACCADE_OK ? settings_.commit() : imported;
}

SaccadeResult Application::export_settings_document() noexcept {
    std::array<uint8_t, saccade::application::settings_encoded_capacity> bytes{};
    size_t size = 0;
    const SaccadeResult exported = settings_.export_document({bytes.data(), bytes.size()}, &size);
    if (exported != SACCADE_OK) return exported;
    std::array<wchar_t, 32768> path{};
    (void)wcscpy_s(path.data(), path.size(), L"saccade-settings.bin");
    constexpr wchar_t filter[] = L"Saccade settings\0*.bin\0All files\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = settings_window_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"bin";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&dialog) == 0) return SACCADE_ERROR_CANCELLED;
    HANDLE file = CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SACCADE_ERROR_BACKEND;
    DWORD written = 0;
    const BOOL saved = WriteFile(file, bytes.data(), static_cast<DWORD>(size), &written, nullptr);
    const BOOL closed = CloseHandle(file);
    return saved != 0 && closed != 0 && written == size ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

SaccadeResult Application::show_binding_editor() noexcept {
    if (bindings_window_ != nullptr) {
        (void)SetForegroundWindow(bindings_window_);
        return SACCADE_OK;
    }
    bindings_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, bindings_class_name, L"Saccade keyboard bindings",
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 800, 760,
                                       settings_window_, nullptr, instance_, this);
    if (bindings_window_ == nullptr) return SACCADE_ERROR_BACKEND;
    EnableWindow(settings_window_, FALSE);
    ShowWindow(bindings_window_, SW_SHOW);
    (void)SetForegroundWindow(bindings_window_);
    return SACCADE_OK;
}

void Application::create_binding_view() noexcept {
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto item = [&](LPCWSTR klass, LPCWSTR text, DWORD style, int identifier, int x, int y, int width,
                    int height) noexcept -> HWND {
        HWND control =
            CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, bindings_window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance_, nullptr);
        if (control != nullptr) (void)SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    (void)item(L"STATIC", L"Command", SS_RIGHT, 0, 20, 22, 100, 24);
    HWND commands = item(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, binding_command, 135, 20, 620, 240);
    std::array<wchar_t, 128> text{};
    for (uint32_t index = 1; commands != nullptr && index <= saccade::application::binding_command_count; ++index) {
        const char* name = saccade::application::command_name(static_cast<Command>(index));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, text.data(), static_cast<int>(text.size())) !=
            0)
            (void)SendMessageW(commands, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.data()));
    }
    if (commands != nullptr) (void)SendMessageW(commands, CB_SETCURSEL, 0, 0);
    (void)item(L"STATIC", L"Physical key", SS_RIGHT, 0, 20, 62, 100, 24);
    HWND keys = item(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, binding_key, 135, 60, 220, 240);
    for (const auto& key : saccade::application::binding_keys()) {
        if (keys != nullptr && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key.name, -1, text.data(),
                                                   static_cast<int>(text.size())) != 0)
            (void)SendMessageW(keys, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.data()));
    }
    if (keys != nullptr) (void)SendMessageW(keys, CB_SETCURSEL, 0, 0);
    (void)item(L"BUTTON", L"Control", BS_AUTOCHECKBOX, binding_control, 370, 60, 90, 25);
    (void)item(L"BUTTON", L"Alt", BS_AUTOCHECKBOX, binding_alt, 465, 60, 55, 25);
    (void)item(L"BUTTON", L"Shift", BS_AUTOCHECKBOX, binding_shift, 525, 60, 70, 25);
    (void)item(L"BUTTON", L"Meta", BS_AUTOCHECKBOX, binding_meta, 600, 60, 70, 25);
    (void)item(L"BUTTON", L"Session", BS_AUTOCHECKBOX, binding_session_only, 675, 60, 80, 25);

    constexpr int keyboard_x = 20;
    constexpr int keyboard_y = 105;
    constexpr int keyboard_width = 740;
    constexpr int keyboard_height = 240;
    saccade::application::BindingKeyboardLayout layout{};
    if (saccade::application::layout_binding_keyboard(keyboard_width, keyboard_height, 4, &layout) == SACCADE_OK) {
        const auto& binding_keys = saccade::application::binding_keys();
        for (uint32_t layout_index = 0; layout_index < layout.key_count; ++layout_index) {
            const auto& rect = layout.keys[layout_index];
            size_t key_index = binding_keys.size();
            for (size_t candidate = 0; candidate < binding_keys.size(); ++candidate) {
                if (binding_keys[candidate].usage == rect.usage) {
                    key_index = candidate;
                    break;
                }
            }
            if (key_index == binding_keys.size()) continue;
            (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, binding_keys[key_index].name, -1, text.data(),
                                      static_cast<int>(text.size()));
            (void)item(L"BUTTON", text.data(), BS_PUSHBUTTON, binding_visual_first + static_cast<int>(key_index),
                       keyboard_x + rect.x, keyboard_y + rect.y, rect.width, rect.height);
        }
    }

    (void)item(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, binding_list, 20, 365, 740, 270);
    (void)item(L"BUTTON", L"Set", BS_DEFPUSHBUTTON, binding_set, 440, 650, 100, 30);
    (void)item(L"BUTTON", L"Remove", BS_PUSHBUTTON, binding_remove, 550, 650, 100, 30);
    (void)item(L"BUTTON", L"Done", BS_PUSHBUTTON, binding_done, 660, 650, 100, 30);
    refresh_binding_list();
    load_selected_binding();
}

void Application::load_selected_binding() noexcept {
    const LRESULT command_index = SendDlgItemMessageW(bindings_window_, binding_command, CB_GETCURSEL, 0, 0);
    if (command_index == CB_ERR) return;

    const Command command = static_cast<Command>(command_index + 1);
    const auto& staged = settings_.staged();
    uint32_t binding_index = UINT32_MAX;
    const bool assigned = saccade::application::find_binding(staged, command, &binding_index) == SACCADE_OK;
    const saccade::application::HotkeyBinding* binding = assigned ? &staged.bindings[binding_index] : nullptr;

    size_t key_index = 0;
    if (binding != nullptr) {
        const auto& keys = saccade::application::binding_keys();
        for (; key_index < keys.size(); ++key_index) {
            if (keys[key_index].usage == binding->physical_key) break;
        }
        if (key_index == keys.size()) key_index = 0;
    }

    (void)SendDlgItemMessageW(bindings_window_, binding_key, CB_SETCURSEL, key_index, 0);
    auto set_checked = [&](int identifier, uint32_t flag) noexcept {
        const bool enabled = binding != nullptr && (binding->modifiers & flag) != 0;
        (void)SendDlgItemMessageW(bindings_window_, identifier, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    set_checked(binding_control, SACCADE_INPUT_MODIFIER_CONTROL);
    set_checked(binding_alt, SACCADE_INPUT_MODIFIER_ALT);
    set_checked(binding_shift, SACCADE_INPUT_MODIFIER_SHIFT);
    set_checked(binding_meta, SACCADE_INPUT_MODIFIER_META);

    HWND session = GetDlgItem(bindings_window_, binding_session_only);
    const bool session_available = command != Command::suspend_toggle;
    if (session != nullptr) {
        (void)SendMessageW(session, BM_SETCHECK,
                           session_available && binding != nullptr &&
                                   (binding->flags & saccade::application::hotkey_session_only) != 0
                               ? BST_CHECKED
                               : BST_UNCHECKED,
                           0);
        EnableWindow(session, session_available ? TRUE : FALSE);
    }
}

void Application::refresh_binding_list() noexcept {
    HWND list = GetDlgItem(bindings_window_, binding_list);
    if (list == nullptr) return;
    (void)SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::array<wchar_t, 128> command_text{};
    std::array<wchar_t, 32> key_text{};
    std::array<wchar_t, 256> line{};
    const auto& settings = settings_.staged();
    const auto& keys = saccade::application::binding_keys();
    for (size_t key_index = 0; key_index < keys.size(); ++key_index) {
        bool assigned = false;
        for (uint32_t binding_index = 0; binding_index < settings.binding_count; ++binding_index) {
            assigned = assigned || settings.bindings[binding_index].physical_key == keys[key_index].usage;
        }
        std::array<wchar_t, 32> title{};
        (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, keys[key_index].name, -1, title.data(),
                                  static_cast<int>(title.size()));
        if (assigned && std::wcslen(title.data()) + 2U < title.size())
            (void)wcscat_s(title.data(), title.size(), L" *");
        (void)SetWindowTextW(GetDlgItem(bindings_window_, binding_visual_first + static_cast<int>(key_index)),
                             title.data());
    }
    for (uint32_t index = 0; index < settings.binding_count; ++index) {
        const auto& binding = settings.bindings[index];
        const char* key_name = "?";
        for (const auto& key : saccade::application::binding_keys())
            if (key.usage == binding.physical_key) {
                key_name = key.name;
                break;
            }
        (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, saccade::application::command_name(binding.command),
                                  -1, command_text.data(), static_cast<int>(command_text.size()));
        (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key_name, -1, key_text.data(),
                                  static_cast<int>(key_text.size()));
        (void)swprintf_s(line.data(), line.size(), L"%ls: %ls%ls%ls%ls%ls%ls", command_text.data(),
                         (binding.modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0 ? L"Ctrl+" : L"",
                         (binding.modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0 ? L"Alt+" : L"",
                         (binding.modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0 ? L"Shift+" : L"",
                         (binding.modifiers & SACCADE_INPUT_MODIFIER_META) != 0 ? L"Meta+" : L"", key_text.data(),
                         (binding.flags & saccade::application::hotkey_session_only) != 0 ? L" [Session]" : L"");
        (void)SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.data()));
    }
}

SaccadeResult Application::modify_binding(bool remove) noexcept {
    const LRESULT command_index = SendDlgItemMessageW(bindings_window_, binding_command, CB_GETCURSEL, 0, 0);
    if (command_index == CB_ERR) return SACCADE_ERROR_INVALID_ARGUMENT;
    const Command command = static_cast<Command>(command_index + 1);
    auto staged = settings_.staged();
    SaccadeResult result = SACCADE_OK;
    if (remove) {
        result = saccade::application::remove_binding(&staged, command);
        if (result == SACCADE_ERROR_NOT_FOUND) return SACCADE_OK;
    } else {
        const LRESULT key_index = SendDlgItemMessageW(bindings_window_, binding_key, CB_GETCURSEL, 0, 0);
        if (key_index == CB_ERR) return SACCADE_ERROR_INVALID_ARGUMENT;
        uint32_t modifiers = 0;
        if (SendDlgItemMessageW(bindings_window_, binding_control, BM_GETCHECK, 0, 0) == BST_CHECKED)
            modifiers |= SACCADE_INPUT_MODIFIER_CONTROL;
        if (SendDlgItemMessageW(bindings_window_, binding_alt, BM_GETCHECK, 0, 0) == BST_CHECKED)
            modifiers |= SACCADE_INPUT_MODIFIER_ALT;
        if (SendDlgItemMessageW(bindings_window_, binding_shift, BM_GETCHECK, 0, 0) == BST_CHECKED)
            modifiers |= SACCADE_INPUT_MODIFIER_SHIFT;
        if (SendDlgItemMessageW(bindings_window_, binding_meta, BM_GETCHECK, 0, 0) == BST_CHECKED)
            modifiers |= SACCADE_INPUT_MODIFIER_META;
        uint32_t flags = command == Command::suspend_toggle ? saccade::application::hotkey_always_active : 0;
        if (SendDlgItemMessageW(bindings_window_, binding_session_only, BM_GETCHECK, 0, 0) == BST_CHECKED)
            flags |= saccade::application::hotkey_session_only;
        const uint32_t physical_key = saccade::application::binding_keys()[static_cast<size_t>(key_index)].usage;
        uint16_t logical_symbol = saccade::platform::windows::logical_symbol_from_hid_usage(physical_key);
        if (logical_symbol == 0) logical_symbol = saccade::application::default_logical_symbol(physical_key);
        saccade::application::BindingConflict conflict{};
        result = saccade::application::set_binding(
            &staged, {command, physical_key, modifiers, static_cast<uint16_t>(flags), logical_symbol}, &conflict);
        if (result == SACCADE_ERROR_ALREADY_EXISTS) {
            std::array<wchar_t, 128> owner{};
            (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      saccade::application::command_name(conflict.command), -1, owner.data(),
                                      static_cast<int>(owner.size()));
            std::array<wchar_t, 192> message{};
            (void)swprintf_s(message.data(), message.size(), L"Key is assigned to %ls.", owner.data());
            MessageBoxW(bindings_window_, message.data(), application_name, MB_OK | MB_ICONWARNING);
            return result;
        }
    }
    if (result == SACCADE_OK) result = settings_.stage(staged);
    if (result == SACCADE_OK) {
        refresh_binding_list();
        load_selected_binding();
    }
    return result;
}

SaccadeResult Application::stage_settings_view() noexcept {
    saccade::application::SettingsDocument staged = settings_.staged();
    auto selection = [&](int identifier, uint32_t* output) noexcept {
        const LRESULT value = SendDlgItemMessageW(settings_window_, identifier, CB_GETCURSEL, 0, 0);
        if (value == CB_ERR) return false;
        *output = static_cast<uint32_t>(value);
        return true;
    };
    auto unsigned_number = [&](int identifier, uint64_t maximum, uint64_t* output) noexcept {
        std::array<wchar_t, 128> text{};
        const int size = GetDlgItemTextW(settings_window_, identifier, text.data(), static_cast<int>(text.size()));
        if (size <= 0 || text[0] == L'-') return false;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(text.data(), &end, 0);
        if (errno == ERANGE || end == text.data() || *end != L'\0' || value > maximum) return false;
        *output = static_cast<uint64_t>(value);
        return true;
    };
    auto signed_number = [&](int identifier, int32_t* output) noexcept {
        std::array<wchar_t, 128> text{};
        const int size = GetDlgItemTextW(settings_window_, identifier, text.data(), static_cast<int>(text.size()));
        if (size <= 0) return false;
        errno = 0;
        wchar_t* end = nullptr;
        const long long value = std::wcstoll(text.data(), &end, 0);
        if (errno == ERANGE || end == text.data() || *end != L'\0' || value < INT32_MIN || value > INT32_MAX)
            return false;
        *output = static_cast<int32_t>(value);
        return true;
    };
    auto utf8 = [&](int identifier, auto* output) noexcept {
        std::array<wchar_t, 256> text{};
        const int size = GetDlgItemTextW(settings_window_, identifier, text.data(), static_cast<int>(text.size()));
        if (size < 0) return false;
        output->fill(0);
        return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), -1, output->data(),
                                   static_cast<int>(output->size()), nullptr, nullptr) != 0;
    };

    std::array<wchar_t, saccade::interaction::maximum_hint_alphabet + 1> alphabet{};
    const int alphabet_count =
        GetDlgItemTextW(settings_window_, settings_hint_alphabet, alphabet.data(), static_cast<int>(alphabet.size()));
    if (alphabet_count <= 0 || alphabet_count > static_cast<int>(staged.hints.alphabet.size()))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    std::array<uint16_t, saccade::interaction::maximum_hint_alphabet> symbols{};
    for (int index = 0; index < alphabet_count; ++index)
        symbols[static_cast<size_t>(index)] = static_cast<uint16_t>(alphabet[index]);
    if (saccade::application::set_hint_alphabet(&staged.hints, symbols.data(), static_cast<uint32_t>(alphabet_count)) !=
        SACCADE_OK) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (!utf8(settings_hint_language, &staged.hints.language) ||
        !utf8(settings_font_family, &staged.appearance.font_family))
        return SACCADE_ERROR_INVALID_ARGUMENT;

    uint32_t selected = 0;
    uint64_t value = 0;
    if (!selection(settings_hint_priority, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.hints.priority = static_cast<saccade::interaction::HintPriority>(selected);
    if (!selection(settings_hint_placement, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.hints.placement = static_cast<saccade::application::HintPlacement>(selected);
    if (!selection(settings_hint_sorting, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.hints.sorting = static_cast<saccade::application::HintSorting>(selected);
    if (!selection(settings_source, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.source = static_cast<saccade::application::TargetSource>(selected);
    if (!unsigned_number(settings_confidence, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.confidence_q16 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_text_confidence, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.text_sensitivity_q16 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_duplicate_iou, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.duplicate_iou_q16 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_minimum_width, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.minimum_width_q8 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_minimum_height, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.minimum_height_q8 = static_cast<uint16_t>(value);
    if (!selection(settings_merge_policy, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.detector.merge_policy = static_cast<saccade::application::MergePolicy>(selected);
    if (!unsigned_number(settings_grid_rows, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.grid.rows = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_grid_columns, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.grid.columns = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_grid_margin_x, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.grid.margin_x_q8 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_grid_margin_y, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.grid.margin_y_q8 = static_cast<uint16_t>(value);

    if (!selection(settings_scope, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.scope = static_cast<saccade::application::TargetScope>(selected);
    if (!unsigned_number(settings_monitor, UINT64_MAX, &staged.monitor_stable_id))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!selection(settings_final_pointer, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.pointer.final_position = static_cast<saccade::application::FinalPointerPosition>(selected);
    if (!unsigned_number(settings_pointer_movement, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.pointer.movement_duration_ms = static_cast<uint32_t>(value);
    if (!signed_number(settings_anchor_x, &staged.pointer.anchor_x_q8) ||
        !signed_number(settings_anchor_y, &staged.pointer.anchor_y_q8))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!selection(settings_initial_mode, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.initial_mode = static_cast<saccade::interaction::SelectionMode>(selected + 1U);
    if (!unsigned_number(settings_timeout, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.timeout_ms = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_hold_duration, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.hold_duration_ms = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_drag_duration, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.drag_duration_ms = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_scroll_duration, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.scroll_duration_ms = static_cast<uint32_t>(value);
    if (!signed_number(settings_scroll_vertical, &staged.actions.scroll_vertical_q8) ||
        !signed_number(settings_scroll_horizontal, &staged.actions.scroll_horizontal_q8))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.actions.click_modifiers = 0;
    if (SendDlgItemMessageW(settings_window_, settings_click_control, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_CONTROL;
    if (SendDlgItemMessageW(settings_window_, settings_click_alt, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_ALT;
    if (SendDlgItemMessageW(settings_window_, settings_click_shift, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_SHIFT;
    if (SendDlgItemMessageW(settings_window_, settings_click_meta, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.actions.click_modifiers |= SACCADE_INPUT_MODIFIER_META;

    if (!selection(settings_placement, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.placement = static_cast<saccade::application::HintPlacement>(selected);
    if (!selection(settings_theme, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.theme = static_cast<saccade::application::Theme>(selected);
    if (!unsigned_number(settings_font_size, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.font_size_q8 = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_font_weight, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.font_weight = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_label_color, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.label_rgba = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_background_color, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.background_rgba = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_outline_color, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.outline_rgba = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_glow_color, UINT32_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.glow_rgba = static_cast<uint32_t>(value);
    if (!unsigned_number(settings_outline_width, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.outline_width_q8 = static_cast<uint16_t>(value);
    if (!unsigned_number(settings_glow_radius, UINT16_MAX, &value)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.appearance.glow_radius_q8 = static_cast<uint16_t>(value);

    if (!selection(settings_compute, &selected)) return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.compute.policy = static_cast<saccade::application::ComputePolicy>(selected);
    if (!unsigned_number(settings_compute_device, UINT64_MAX, &staged.compute.device_stable_id))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    staged.flags = 0;
    if (SendDlgItemMessageW(settings_window_, settings_animate, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.flags |= saccade::application::settings_animate_overlay;
    if (SendDlgItemMessageW(settings_window_, settings_reduced_motion, BM_GETCHECK, 0, 0) == BST_CHECKED)
        staged.flags |= saccade::application::settings_reduced_motion;
    return settings_.stage(staged);
}

SaccadeResult Application::reset_settings_view_page() noexcept {
    SaccadeResult result = stage_settings_view();
    if (result != SACCADE_OK) return result;
    const LRESULT page = SendDlgItemMessageW(settings_window_, settings_reset_page, CB_GETCURSEL, 0, 0);
    if (page == CB_ERR) return SACCADE_ERROR_INVALID_ARGUMENT;
    result = settings_.reset_page(static_cast<saccade::application::SettingsPage>(page));
    return result == SACCADE_OK ? settings_.commit() : result;
}

SaccadeResult Application::commit_settings(bool defaults) noexcept {
    if (defaults) {
        const SaccadeResult reset = settings_.reset_all();
        return reset == SACCADE_OK ? settings_.commit() : reset;
    }
    const SaccadeResult staged = stage_settings_view();
    return staged == SACCADE_OK ? settings_.commit() : staged;
}

SaccadeResult Application::restart(void* context) noexcept {
    return static_cast<Application*>(context)->request_restart();
}

SaccadeResult Application::quit(void* context) noexcept {
    auto* app = static_cast<Application*>(context);
    return PostMessageW(app->window_, WM_CLOSE, 0, 0) != 0 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

uint64_t Application::timestamp_ns() const noexcept {
    LARGE_INTEGER counter{};
    if (counter_frequency_ == 0 || QueryPerformanceCounter(&counter) == 0 || counter.QuadPart <= 0) return 1;
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    constexpr uint64_t ns_per_second = UINT64_C(1'000'000'000);
    return (ticks / counter_frequency_) * ns_per_second +
           (ticks % counter_frequency_) * ns_per_second / counter_frequency_;
}

SaccadeResult Application::arm_runtime_timer(uint64_t now_ns) noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (runtime_timer_ == nullptr || now_ns == 0) return SACCADE_ERROR_STATE;
    do {
        next_runtime_tick_ns_ += runtime_tick_period_ns;
    } while (next_runtime_tick_ns_ <= now_ns);
    const uint64_t delay_ns = next_runtime_tick_ns_ - now_ns;
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -static_cast<int64_t>((delay_ns + waitable_timer_unit_ns - 1U) / waitable_timer_unit_ns);
    return SetWaitableTimerEx(runtime_timer_, &due_time, 0, nullptr, nullptr, nullptr, 0) != 0 ? SACCADE_OK
                                                                                               : SACCADE_ERROR_BACKEND;
#else
    (void)now_ns;
    return SACCADE_ERROR_UNSUPPORTED;
#endif
}

void Application::update_tray() noexcept {
    if (!tray_added_) return;
    const wchar_t* text = host_.suspended()      ? L"Saccade - suspended"
                          : scene_incomplete_    ? L"Saccade - partial target coverage"
                          : fault_ == SACCADE_OK ? L"Saccade"
                                                 : L"Saccade - attention required";
    (void)wcscpy_s(tray_.szTip, text);
    tray_.uFlags = NIF_TIP;
    (void)Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

SaccadeResult Application::initialize(HINSTANCE instance, int show_command) noexcept {
    (void)show_command;
    if (initialized_ || owner_ != nullptr) return SACCADE_ERROR_ALREADY_EXISTS;
    if (!saccade::platform::windows::operating_system_supported()) return SACCADE_ERROR_UNSUPPORTED;
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart <= 0) return SACCADE_ERROR_BACKEND;
    counter_frequency_ = static_cast<uint64_t>(frequency.QuadPart);
    previous_dpi_context_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous_dpi_context_ == nullptr) return SACCADE_ERROR_BACKEND;
    instance_ = instance;
    owner_ = this;

    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance_;
    klass.hIcon = LoadIconW(instance_, application_icon());
    klass.hIconSm = klass.hIcon;
    klass.lpszClassName = window_class_name;
    if (RegisterClassExW(&klass) == 0) {
        owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    WNDCLASSEXW settings_class{};
    settings_class.cbSize = sizeof(settings_class);
    settings_class.lpfnWndProc = settings_proc;
    settings_class.hInstance = instance_;
    settings_class.hIcon = LoadIconW(instance_, application_icon());
    settings_class.hIconSm = settings_class.hIcon;
    settings_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    settings_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    settings_class.lpszClassName = settings_class_name;
    if (RegisterClassExW(&settings_class) == 0) {
        (void)UnregisterClassW(window_class_name, instance_);
        owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    WNDCLASSEXW bindings_class = settings_class;
    bindings_class.lpfnWndProc = bindings_proc;
    bindings_class.lpszClassName = bindings_class_name;
    if (RegisterClassExW(&bindings_class) == 0) {
        (void)UnregisterClassW(settings_class_name, instance_);
        (void)UnregisterClassW(window_class_name, instance_);
        owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    WNDCLASSEXW diagnostics_class = settings_class;
    diagnostics_class.lpfnWndProc = diagnostics_proc;
    diagnostics_class.lpszClassName = diagnostics_class_name;
    if (RegisterClassExW(&diagnostics_class) == 0) {
        (void)UnregisterClassW(bindings_class_name, instance_);
        (void)UnregisterClassW(settings_class_name, instance_);
        (void)UnregisterClassW(window_class_name, instance_);
        owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class_name, application_name, WS_POPUP, 0, 0,
                              0, 0, nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        (void)UnregisterClassW(diagnostics_class_name, instance_);
        (void)UnregisterClassW(bindings_class_name, instance_);
        (void)UnregisterClassW(settings_class_name, instance_);
        (void)UnregisterClassW(window_class_name, instance_);
        owner_ = nullptr;
        return SACCADE_ERROR_BACKEND;
    }
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) == 0) return SACCADE_ERROR_BACKEND;
    session_notifications_ = true;
#endif

    const saccade::application::DesktopHostCallbacks callbacks{this,    dispatch,      set_suspended, neutralize,
                                                               observe, open_settings, restart,       quit};
    SaccadeResult result = host_.initialize(callbacks);
    if (result != SACCADE_OK) return result;
    host_initialized_ = true;
    result = hotkeys_.initialize({&host_, saccade::application::dispatch_desktop_command,
                                  saccade::application::observe_desktop_input, route_key, observe_command});
    if (result != SACCADE_OK) return result;
    hotkeys_initialized_ = true;
    saccade::application::SettingsDocument initial = saccade::application::default_settings();
    (void)load_settings(&initial);
    result = settings_.initialize(initial, {this, apply_settings});
    if (result != SACCADE_OK) return result;
    settings_initialized_ = true;
#if defined(SACCADE_HAS_WINDOWS_ML)
    result = runtime_scheduling_.initialize();
    if (result != SACCADE_OK) return result;
#endif
    result = initialize_pipeline();
    if (result != SACCADE_OK) {
        fault_ = result;
#if defined(SACCADE_HAS_WINDOWS_ML)
        if (result == SACCADE_ERROR_BACKEND) pipeline_recovery_.start(timestamp_ns());
        if (result == SACCADE_ERROR_STATE && pipeline_cleanup_required_) (void)request_restart();
#endif
    }

    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = window_;
    tray_.uID = tray_identifier;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    tray_.uCallbackMessage = tray_message;
    tray_.hIcon = LoadIconW(instance_, application_icon());
    (void)wcscpy_s(tray_.szTip, application_name);
    if (Shell_NotifyIconW(NIM_ADD, &tray_) == 0) return SACCADE_ERROR_BACKEND;
    tray_added_ = true;
    tray_.uVersion = NOTIFYICON_VERSION_4;
    (void)Shell_NotifyIconW(NIM_SETVERSION, &tray_);
#if defined(SACCADE_HAS_WINDOWS_ML)
    runtime_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (runtime_timer_ == nullptr) return SACCADE_ERROR_BACKEND;
    next_runtime_tick_ns_ = timestamp_ns();
    if (arm_runtime_timer(next_runtime_tick_ns_) != SACCADE_OK) return SACCADE_ERROR_BACKEND;
#endif
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult Application::initialize_pipeline() noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (!saccade::apps::model_trust::configured) return SACCADE_ERROR_NOT_FOUND;
    if (verifier_initialized_ || pipeline_initialized_ || pipeline_cleanup_required_ || agent_pipe_initialized_)
        return SACCADE_ERROR_STATE;
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length == executable.size()) return SACCADE_ERROR_BACKEND;
    size_t separator = length;
    while (separator != 0 && executable[separator - 1U] != L'\\' && executable[separator - 1U] != L'/')
        --separator;
    if (separator == 0) return SACCADE_ERROR_BACKEND;
    executable[separator - 1U] = L'\0';
    const int directory_size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executable.data(), -1, shader_directory_.data(),
                            static_cast<int>(shader_directory_.size()), nullptr, nullptr);
    constexpr wchar_t artifact_name[] = L"\\saccade.model";
    const size_t artifact_units = std::size(artifact_name);
    if (directory_size == 0 || separator + artifact_units > executable.size()) return SACCADE_ERROR_CAPACITY;
    std::memcpy(executable.data() + separator - 1U, artifact_name, artifact_units * sizeof(wchar_t));
    const int artifact_size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executable.data(), -1, artifact_path_.data(),
                            static_cast<int>(artifact_path_.size()), nullptr, nullptr);
    if (artifact_size == 0) return SACCADE_ERROR_BACKEND;
    SaccadeResult result = verifier_.initialize(saccade::apps::model_trust::public_key);
    if (result != SACCADE_OK) return result;
    verifier_initialized_ = true;
    result = pipeline_.initialize({artifact_path_.data(), shader_directory_.data(), &settings_.current(),
                                   verifier_.descriptor(), this, nullptr, timestamp_ns()});
    if (result == SACCADE_OK) {
        pipeline_initialized_ = true;
        constexpr SaccadeAgentCapabilityBits agent_capabilities =
            SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER | SACCADE_AGENT_CAPABILITY_KEYBOARD |
            SACCADE_AGENT_CAPABILITY_WINDOW;
        result = agent_pipe_.initialize({this, process_agent, neutralize, nullptr, agent_capabilities},
                                        &agent_pipe_storage_);
        if (result == SACCADE_OK) agent_pipe_initialized_ = true;
    }
    if (result != SACCADE_OK) {
        SaccadeResult cleanup = SACCADE_OK;
        if (agent_pipe_initialized_) {
            const SaccadeResult stopped = agent_pipe_.shutdown();
            if (stopped == SACCADE_OK)
                agent_pipe_initialized_ = false;
            else
                cleanup = stopped;
        }
        const SaccadeResult pipeline_stopped = pipeline_.shutdown();
        pipeline_initialized_ = false;
        if (pipeline_stopped == SACCADE_OK)
            pipeline_cleanup_required_ = false;
        else {
            pipeline_cleanup_required_ = true;
            if (cleanup == SACCADE_OK) cleanup = pipeline_stopped;
        }
        if (verifier_initialized_) {
            const SaccadeResult stopped = verifier_.shutdown();
            if (stopped == SACCADE_OK)
                verifier_initialized_ = false;
            else if (cleanup == SACCADE_OK)
                cleanup = stopped;
        }
        if (cleanup != SACCADE_OK) result = SACCADE_ERROR_STATE;
    }
    return result;
#else
    return SACCADE_ERROR_UNSUPPORTED;
#endif
}

SaccadeResult Application::shutdown_pipeline() noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    SaccadeResult result = SACCADE_OK;
    if (agent_pipe_initialized_) {
        const SaccadeResult stopped = agent_pipe_.shutdown();
        if (stopped == SACCADE_OK)
            agent_pipe_initialized_ = false;
        else
            result = stopped;
    }
    if (pipeline_initialized_ || pipeline_cleanup_required_) {
        const SaccadeResult stopped = pipeline_.shutdown();
        if (stopped == SACCADE_OK) {
            pipeline_initialized_ = false;
            pipeline_cleanup_required_ = false;
        } else if (result == SACCADE_OK) {
            result = stopped;
        }
    }
    if (verifier_initialized_) {
        const SaccadeResult stopped = verifier_.shutdown();
        if (stopped == SACCADE_OK)
            verifier_initialized_ = false;
        else if (result == SACCADE_OK)
            result = stopped;
    }
    return result;
#else
    return SACCADE_OK;
#endif
}

void Application::begin_pipeline_recovery(SaccadeResult failure, uint64_t now_ns) noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    fault_ = failure;
    const SaccadeResult stopped = shutdown_pipeline();
    if (stopped == SACCADE_OK) {
        pipeline_recovery_.start(now_ns);
    } else {
        fault_ = stopped;
        pipeline_recovery_.complete();
        (void)request_restart();
    }
    update_tray();
#else
    (void)failure;
    (void)now_ns;
#endif
}

void Application::advance_pipeline() noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    const uint64_t now_ns = timestamp_ns();
    if (!pipeline_initialized_) {
        if (!pipeline_recovery_.due(now_ns)) return;
        const SaccadeResult recovered = initialize_pipeline();
        if (recovered == SACCADE_OK) {
            pipeline_recovery_.complete();
            fault_ = SACCADE_OK;
        } else if (recovered == SACCADE_ERROR_STATE) {
            pipeline_recovery_.complete();
            fault_ = recovered;
            (void)request_restart();
        } else {
            fault_ = recovered;
            pipeline_recovery_.retry(now_ns);
        }
        update_tray();
        return;
    }
    saccade::platform::windows::DesktopPipelineAdvance output{};
    const SaccadeResult result = pipeline_.advance(now_ns, &output);
    if (result != SACCADE_OK && result != SACCADE_ERROR_PERMISSION && result != SACCADE_ERROR_NOT_FOUND &&
        result != SACCADE_ERROR_BUSY) {
        if (result == SACCADE_ERROR_BACKEND)
            begin_pipeline_recovery(result, now_ns);
        else {
            fault_ = result;
            update_tray();
        }
        return;
    }
    if (output.runtime.scene.scene_published) {
        const bool incomplete = (output.runtime.scene.packet_flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0;
        if (scene_incomplete_ != incomplete) {
            scene_incomplete_ = incomplete;
            update_tray();
        }
    }
    if (agent_pipe_initialized_) {
        const SaccadeResult agent_result = agent_pipe_.advance(now_ns);
        if (agent_result != SACCADE_OK) {
            fault_ = agent_result;
            update_tray();
        }
    }
#endif
}

SaccadeResult Application::set_input_available(bool available) noexcept {
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (!pipeline_initialized_) return SACCADE_OK;
    const uint64_t now_ns = timestamp_ns();
    const SaccadeResult result = pipeline_.set_input_available(available, now_ns);
    if (result != SACCADE_OK) {
        if (result == SACCADE_ERROR_BACKEND)
            begin_pipeline_recovery(result, now_ns);
        else {
            fault_ = result;
            update_tray();
        }
    }
    return result;
#else
    (void)available;
    return SACCADE_OK;
#endif
}

void Application::show_menu() noexcept {
    POINT point{};
    if (GetCursorPos(&point) == 0) return;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    (void)AppendMenuW(menu, MF_STRING, menu_settings, L"Settings...");
    (void)AppendMenuW(menu, MF_STRING, menu_diagnostics, L"Diagnostics...");
    (void)AppendMenuW(menu, MF_STRING | (host_.suspended() ? MF_CHECKED : 0), menu_suspend, L"Suspend hotkeys");
    if (fault_ != SACCADE_OK) {
        (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        (void)AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Targeting runtime is not connected");
    }
    if (scene_incomplete_) {
        (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        (void)AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Partial target coverage");
    }
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, menu_restart, L"Restart");
    (void)AppendMenuW(menu, MF_STRING, menu_quit, L"Quit Saccade");
    (void)SetForegroundWindow(window_);
    (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x, point.y, 0, window_,
                         nullptr);
    (void)DestroyMenu(menu);
}

void Application::show_diagnostics() noexcept {
    if (diagnostics_window_ != nullptr) {
        refresh_diagnostics();
        (void)SetForegroundWindow(diagnostics_window_);
        return;
    }
    diagnostics_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, diagnostics_class_name, L"Saccade Debugger",
                                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, CW_USEDEFAULT,
                                          CW_USEDEFAULT, 900, 620, window_, nullptr, instance_, this);
    if (diagnostics_window_ == nullptr) return;
    ShowWindow(diagnostics_window_, SW_SHOW);
    (void)SetForegroundWindow(diagnostics_window_);
}

void Application::create_diagnostics_view() noexcept {
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND views = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 16, 16, 852, 180,
                                 diagnostics_window_, reinterpret_cast<HMENU>(diagnostics_view), instance_, nullptr);
    constexpr const wchar_t* names[]{
        L"Overview",      L"Displays", L"Runtime", L"Overlay / GPU", L"Memory", L"Trace", L"Frames / Transforms",
        L"Scene / Fusion"};
    for (const wchar_t* name : names)
        (void)SendMessageW(views, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    (void)SendMessageW(views, CB_SETCURSEL, 0, 0);
    (void)SendMessageW(views, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    HWND output =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 16, 52, 852,
                        468, diagnostics_window_, reinterpret_cast<HMENU>(diagnostics_text), instance_, nullptr);
    (void)SendMessageW(output, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);
    constexpr const wchar_t* button_names[]{L"Capture Scene", L"Dry Run", L"Replay", L"Clear"};
    constexpr int button_ids[]{diagnostics_capture, diagnostics_dry_run, diagnostics_replay, diagnostics_clear};
    for (uint32_t index = 0; index < 4; ++index) {
        HWND button =
            CreateWindowExW(0, L"BUTTON", button_names[index], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            16 + static_cast<int>(index) * 126, 532, 116, 28, diagnostics_window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(button_ids[index])), instance_, nullptr);
        (void)SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    HWND faults = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 520, 532, 160, 180,
                                  diagnostics_window_, reinterpret_cast<HMENU>(diagnostics_fault), instance_, nullptr);
    constexpr const wchar_t* fault_names[]{L"Capture", L"Inference", L"Scene", L"Overlay", L"Input"};
    for (const wchar_t* name : fault_names)
        (void)SendMessageW(faults, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    (void)SendMessageW(faults, CB_SETCURSEL, 0, 0);
    (void)SendMessageW(faults, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    HWND arm = CreateWindowExW(0, L"BUTTON", L"Arm Fault", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 690, 532, 178, 28,
                               diagnostics_window_, reinterpret_cast<HMENU>(diagnostics_arm_fault), instance_, nullptr);
    (void)SendMessageW(arm, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    RECT client{};
    if (GetClientRect(diagnostics_window_, &client) != 0)
        layout_diagnostics_view(client.right - client.left, client.bottom - client.top);
    (void)SetTimer(diagnostics_window_, diagnostics_timer_identifier, 500, nullptr);
    refresh_diagnostics();
}

void Application::layout_diagnostics_view(int32_t width, int32_t height) noexcept {
    saccade::application::DebuggerLayout layout{};
    if (saccade::application::make_debugger_layout(width, height, &layout) != SACCADE_OK) return;

    const auto move = [this](int control, const saccade::application::DebuggerLayoutRect& rect,
                             int32_t control_height) noexcept {
        (void)MoveWindow(GetDlgItem(diagnostics_window_, control), rect.x, rect.y, rect.width, control_height, TRUE);
    };
    move(diagnostics_view, layout.views, 180);
    move(diagnostics_text, layout.content, layout.content.height);
    constexpr int buttons[]{diagnostics_capture, diagnostics_dry_run, diagnostics_replay, diagnostics_clear};
    for (uint32_t index = 0; index < layout.actions.size(); ++index)
        move(buttons[index], layout.actions[index], layout.actions[index].height);
    move(diagnostics_fault, layout.fault, 180);
    move(diagnostics_arm_fault, layout.arm_fault, layout.arm_fault.height);
}

void Application::refresh_diagnostics() noexcept {
    std::array<wchar_t, 32768> text{};
#if defined(SACCADE_HAS_WINDOWS_ML)
    saccade::platform::windows::DesktopPipelineDiagnostics diagnostics{};
    const SaccadeResult result = pipeline_initialized_ ? pipeline_.read_diagnostics(&diagnostics) : SACCADE_ERROR_STATE;
    if (result == SACCADE_OK) {
        const LRESULT selected = SendDlgItemMessageW(diagnostics_window_, diagnostics_view, CB_GETCURSEL, 0, 0);
        const auto view = selected == CB_ERR ? saccade::application::DebuggerView::overview
                                             : static_cast<saccade::application::DebuggerView>(selected);
        size_t cursor = 0;
        auto append = [&](const wchar_t* format, auto... arguments) noexcept {
            if (cursor >= text.size()) return;
            const int written = swprintf_s(text.data() + cursor, text.size() - cursor, format, arguments...);
            if (written > 0) cursor += static_cast<size_t>(written);
        };
        if (debugger_operation_[0] != L'\0') append(L"%ls\r\n\r\n", debugger_operation_.data());
        const uint64_t inference_device =
            diagnostics.inference_memory.device_owned + diagnostics.inference_memory.device_imported;
        const saccade::application::DebugTraceSnapshot& trace = diagnostics.runtime.trace;
        const saccade::application::DebugTraceEvent* latest =
            trace.count == 0 ? nullptr : &trace.events[trace.count - 1U];
        if (view == saccade::application::DebuggerView::overview) {
            append(
                L"Permissions  capture %ls  accessibility %ls  input %ls\r\n"
                L"Surface  disposition %u  reasons 0x%x\r\n"
                L"Displays %u  topology %llu  source %u  scope %u  compute %u\r\n"
                L"Model %016llx  provider %016llx  device %016llx  precision 0x%x\r\n"
                L"Scene %llu  targets %u  flags 0x%x  partial %ls\r\n"
                L"Frames offered %llu  replaced %llu  stale %llu  scene batches %llu\r\n"
                L"Capture acquired %llu  empty %llu  capture failures %llu\r\n"
                L"Overlay ticks %llu  rendered %llu  presented %llu  busy %llu\r\n"
                L"Memory inference device %llu  framework %llu  capture %llu  overlay %llu\r\n"
                L"Failures pipeline %llu  runtime %llu  neural %llu  scene %llu\r\n"
                L"Trace events %u  overwritten %llu  latest #%llu code %u result %d argument %llu",
                (diagnostics.permissions & saccade::platform::windows::diagnostic_capture_permission) != 0 ? L"yes"
                                                                                                           : L"no",
                (diagnostics.permissions & saccade::platform::windows::diagnostic_accessibility_permission) != 0
                    ? L"yes"
                    : L"no",
                (diagnostics.permissions & saccade::platform::windows::diagnostic_input_permission) != 0 ? L"yes"
                                                                                                         : L"no",
                static_cast<uint32_t>(diagnostics.surface), diagnostics.surface_reason_bits, diagnostics.display_count,
                static_cast<unsigned long long>(diagnostics.topology_epoch), static_cast<uint32_t>(diagnostics.source),
                static_cast<uint32_t>(diagnostics.scope), static_cast<uint32_t>(diagnostics.compute),
                static_cast<unsigned long long>(diagnostics.model.model_stable_id),
                static_cast<unsigned long long>(diagnostics.model.provider_stable_id),
                static_cast<unsigned long long>(diagnostics.model.device_stable_id), diagnostics.model.precision_bits,
                static_cast<unsigned long long>(diagnostics.runtime.scene_status.scene_epoch),
                diagnostics.runtime.scene_status.target_count, diagnostics.runtime.scene_status.packet_flags,
                (diagnostics.runtime.scene_status.packet_flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0 ? L"yes"
                                                                                                        : L"no",
                static_cast<unsigned long long>(diagnostics.runtime.neural.frames_offered),
                static_cast<unsigned long long>(diagnostics.runtime.neural.frames_replaced),
                static_cast<unsigned long long>(diagnostics.runtime.neural.frames_stale),
                static_cast<unsigned long long>(diagnostics.runtime.neural.batches_published),
                static_cast<unsigned long long>(diagnostics.capture.frames_acquired),
                static_cast<unsigned long long>(diagnostics.capture.empty_acquires),
                static_cast<unsigned long long>(diagnostics.capture.failures),
                static_cast<unsigned long long>(diagnostics.overlay.ticks),
                static_cast<unsigned long long>(diagnostics.overlay.rendered),
                static_cast<unsigned long long>(diagnostics.overlay.presented),
                static_cast<unsigned long long>(diagnostics.overlay.busy),
                static_cast<unsigned long long>(inference_device),
                static_cast<unsigned long long>(diagnostics.inference_memory.framework_opaque),
                static_cast<unsigned long long>(diagnostics.capture_memory.high_water_bytes),
                static_cast<unsigned long long>(diagnostics.overlay.known_memory_bytes),
                static_cast<unsigned long long>(diagnostics.pipeline.failures),
                static_cast<unsigned long long>(diagnostics.runtime.runtime.failures),
                static_cast<unsigned long long>(diagnostics.runtime.neural.failures),
                static_cast<unsigned long long>(diagnostics.runtime.scene.failures), trace.count,
                static_cast<unsigned long long>(trace.overwritten),
                static_cast<unsigned long long>(latest == nullptr ? 0 : latest->sequence),
                latest == nullptr ? 0 : static_cast<uint32_t>(latest->code), latest == nullptr ? 0 : latest->result,
                static_cast<unsigned long long>(latest == nullptr ? 0 : latest->argument));
        } else if (view == saccade::application::DebuggerView::displays) {
            for (uint32_t index = 0; index < diagnostics.display_count; ++index) {
                const auto& display = diagnostics.displays[index].display;
                append(L"Display %u  id %016llx  %ux%u @ %u Hz  rotation %u  flags 0x%x\r\n"
                       L"  desktop (%d,%d) %dx%d  work (%d,%d) %dx%d\r\n",
                       index, static_cast<unsigned long long>(display.display_id), display.backing_width,
                       display.backing_height, display.maximum_fps, static_cast<uint32_t>(display.rotation),
                       display.flags, display.desktop_bounds.x, display.desktop_bounds.y, display.desktop_bounds.width,
                       display.desktop_bounds.height, display.work_bounds.x, display.work_bounds.y,
                       display.work_bounds.width, display.work_bounds.height);
            }
        } else if (view == saccade::application::DebuggerView::runtime) {
            const auto& runtime = diagnostics.runtime;
            const uint64_t average_batch_ns =
                runtime.neural.batches_published == 0
                    ? 0
                    : runtime.neural.batch_latency_total_ns / runtime.neural.batches_published;
            const uint64_t average_full_scope_ns =
                runtime.neural.batches_published == 0
                    ? 0
                    : runtime.neural.full_scope_latency_total_ns / runtime.neural.batches_published;
            append(L"Frames offered %llu  replaced %llu  stale %llu\r\n"
                   L"Batches started %llu  published %llu  sources completed %llu  failed %llu\r\n"
                   L"Batch latency avg %llu ns  max %llu ns  missed %llu\r\n"
                   L"Full scope avg %llu ns  max %llu ns  missed %llu\r\n"
                   L"Scene advances %llu  neural %llu  fused %llu  targets %llu\r\n"
                   L"Semantic partial %llu  partial publications %llu  text truncations %llu\r\n"
                   L"Commands %llu  symbols %llu  overlay compositions %llu\r\n",
                   static_cast<unsigned long long>(runtime.neural.frames_offered),
                   static_cast<unsigned long long>(runtime.neural.frames_replaced),
                   static_cast<unsigned long long>(runtime.neural.frames_stale),
                   static_cast<unsigned long long>(runtime.neural.batches_started),
                   static_cast<unsigned long long>(runtime.neural.batches_published),
                   static_cast<unsigned long long>(runtime.neural.sources_completed),
                   static_cast<unsigned long long>(runtime.neural.sources_failed),
                   static_cast<unsigned long long>(average_batch_ns),
                   static_cast<unsigned long long>(runtime.neural.batch_latency_max_ns),
                   static_cast<unsigned long long>(runtime.neural.batch_deadlines_missed),
                   static_cast<unsigned long long>(average_full_scope_ns),
                   static_cast<unsigned long long>(runtime.neural.full_scope_latency_max_ns),
                   static_cast<unsigned long long>(runtime.neural.full_scope_deadlines_missed),
                   static_cast<unsigned long long>(runtime.scene.advances),
                   static_cast<unsigned long long>(runtime.scene.neural_updates),
                   static_cast<unsigned long long>(runtime.scene.fused_publications),
                   static_cast<unsigned long long>(runtime.scene.targets_published),
                   static_cast<unsigned long long>(runtime.scene.semantic_incomplete),
                   static_cast<unsigned long long>(runtime.scene.incomplete_publications),
                   static_cast<unsigned long long>(runtime.scene.text_truncated_publications),
                   static_cast<unsigned long long>(runtime.runtime.commands),
                   static_cast<unsigned long long>(runtime.runtime.symbols),
                   static_cast<unsigned long long>(runtime.runtime.overlay_compositions));
        } else if (view == saccade::application::DebuggerView::overlay_gpu) {
            for (uint32_t index = 0; index < diagnostics.display_count; ++index) {
                const auto& display = diagnostics.displays[index];
                append(L"Display %016llx  attempts %llu  rendered %llu  presented %llu  busy %llu\r\n"
                       L"  GPU slots %u  target cap %u  instance cap %u\r\n"
                       L"  submissions %llu  busy %llu  static %llu  active %llu  draw calls %llu  failures %llu\r\n",
                       static_cast<unsigned long long>(display.display.display_id),
                       static_cast<unsigned long long>(display.overlay.presentation_attempts),
                       static_cast<unsigned long long>(display.overlay.rendered_frames),
                       static_cast<unsigned long long>(display.overlay.presented_frames),
                       static_cast<unsigned long long>(display.overlay.busy_frames), display.gpu.slot_count,
                       display.gpu.target_capacity, display.gpu.instance_capacity,
                       static_cast<unsigned long long>(display.gpu.submissions),
                       static_cast<unsigned long long>(display.gpu.busy_submissions),
                       static_cast<unsigned long long>(display.gpu.static_dispatches),
                       static_cast<unsigned long long>(display.gpu.active_dispatches),
                       static_cast<unsigned long long>(display.gpu.draw_calls),
                       static_cast<unsigned long long>(display.gpu.failures));
            }
        } else if (view == saccade::application::DebuggerView::memory) {
            append(L"Inference device %llu  host %llu  framework %llu  high water %llu\r\n"
                   L"Capture high water %llu  overlay known %llu\r\n",
                   static_cast<unsigned long long>(inference_device),
                   static_cast<unsigned long long>(diagnostics.inference_memory.host_committed),
                   static_cast<unsigned long long>(diagnostics.inference_memory.framework_opaque),
                   static_cast<unsigned long long>(diagnostics.inference_memory.high_water_bytes),
                   static_cast<unsigned long long>(diagnostics.capture_memory.high_water_bytes),
                   static_cast<unsigned long long>(diagnostics.overlay.known_memory_bytes));
            for (uint32_t index = 0; index < diagnostics.display_count; ++index)
                append(L"Display %016llx  surface %llu  swapchain %llu  total %llu\r\n",
                       static_cast<unsigned long long>(diagnostics.displays[index].display.display_id),
                       static_cast<unsigned long long>(diagnostics.displays[index].memory.surface_host_bytes),
                       static_cast<unsigned long long>(diagnostics.displays[index].memory.swapchain_bytes_estimate),
                       static_cast<unsigned long long>(diagnostics.displays[index].memory.total_known_and_estimated));
        } else if (view == saccade::application::DebuggerView::trace) {
            append(L"Events %u  overwritten %llu  next %llu\r\n\r\n", trace.count,
                   static_cast<unsigned long long>(trace.overwritten),
                   static_cast<unsigned long long>(trace.next_sequence));
            for (uint32_t index = 0; index < trace.count; ++index) {
                const auto& event = trace.events[index];
                append(L"#%llu  time %llu  code %u  result %d  flags 0x%x  argument %llu\r\n",
                       static_cast<unsigned long long>(event.sequence),
                       static_cast<unsigned long long>(event.timestamp_ns), static_cast<uint32_t>(event.code),
                       event.result, event.flags, static_cast<unsigned long long>(event.argument));
            }
        } else if (view == saccade::application::DebuggerView::frames_transforms) {
            const auto& frames = diagnostics.debugger_frames_transforms;
            append(L"Scene %llu  frame %llu  model %llu  session %llu\r\n"
                   L"Transform %llu  topology %llu  source %llu  targets %u\r\n"
                   L"Bytes %llu  captured %llu ns  transforms %u\r\n\r\n",
                   static_cast<unsigned long long>(frames.frame.scene.scene_epoch),
                   static_cast<unsigned long long>(frames.frame.scene.frame_id),
                   static_cast<unsigned long long>(frames.frame.scene.model_epoch),
                   static_cast<unsigned long long>(frames.frame.scene.session_epoch),
                   static_cast<unsigned long long>(frames.frame.scene.transform_epoch),
                   static_cast<unsigned long long>(frames.frame.scene.topology_epoch),
                   static_cast<unsigned long long>(frames.frame.scene.source_id), frames.frame.scene.target_count,
                   static_cast<unsigned long long>(frames.frame.byte_size),
                   static_cast<unsigned long long>(frames.frame.timestamp_ns), frames.transform_count);
            for (uint32_t index = 0; index < frames.transform_count; ++index) {
                const auto& record = frames.transforms[index];
                const auto& transform = record.transform;
                append(L"Transform %u  source %016llx  display %016llx  epoch %llu\r\n"
                       L"  space %u -> %u  rotation %u  flags 0x%x\r\n"
                       L"  source (%d,%d) %dx%d  destination (%d,%d) %dx%d\r\n",
                       index, static_cast<unsigned long long>(record.source_id),
                       static_cast<unsigned long long>(record.display_id),
                       static_cast<unsigned long long>(transform.epoch), static_cast<uint32_t>(transform.source_space),
                       static_cast<uint32_t>(transform.destination_space), static_cast<uint32_t>(transform.rotation),
                       transform.flags, transform.source.x, transform.source.y, transform.source.width,
                       transform.source.height, transform.destination.x, transform.destination.y,
                       transform.destination.width, transform.destination.height);
            }
        } else {
            const auto& scene = diagnostics.debugger_scene_fusion;
            const auto& targets = scene.targets;
            const auto& fusion = scene.fusion;
            append(L"Scene %llu  frame %llu  targets %u  samples %u  omitted %u\r\n"
                   L"Inputs %u  candidates %llu  written %u  dropped %llu\r\n"
                   L"Buckets %llu  overlap tests %llu  duplicates %llu  safety merges %llu\r\n"
                   L"Sources neural %u  accessibility %u  pixel %u  grid %u  fused %u\r\n"
                   L"State actionable %u  disabled %u  occluded %u  secure %u  approximate %u\r\n"
                   L"Text bytes %u  redacted %u  truncated %u  capabilities 0x%x\r\n\r\n",
                   static_cast<unsigned long long>(scene.scene.scene_epoch),
                   static_cast<unsigned long long>(scene.scene.frame_id), targets.target_count, scene.sample_count,
                   scene.samples_omitted, scene.fusion_input_count,
                   static_cast<unsigned long long>(fusion.candidates_read), fusion.targets_written,
                   static_cast<unsigned long long>(fusion.capacity_drops),
                   static_cast<unsigned long long>(fusion.bucket_visits),
                   static_cast<unsigned long long>(fusion.overlap_tests),
                   static_cast<unsigned long long>(fusion.duplicates_merged),
                   static_cast<unsigned long long>(fusion.safety_merges), targets.neural, targets.accessibility,
                   targets.pixel, targets.grid, targets.fused, targets.actionable, targets.disabled, targets.occluded,
                   targets.secure, targets.approximate, targets.text_bytes, targets.text_redacted,
                   targets.text_truncated, targets.capability_bits);
            for (uint32_t index = 0; index < scene.sample_count; ++index) {
                const auto& target = scene.samples[index];
                append(L"#%u  id %016llx  role %u  source 0x%x  confidence %u  flags 0x%x\r\n"
                       L"  bounds (%d,%d) %dx%d  safe (%d,%d)  window %016llx  order %u\r\n",
                       index, static_cast<unsigned long long>(target.target_id), target.role, target.source_bits,
                       target.confidence_q16, target.flags, target.bounds.x, target.bounds.y, target.bounds.width,
                       target.bounds.height, target.safe_point.x, target.safe_point.y,
                       static_cast<unsigned long long>(target.window_id), target.order);
            }
        }
    } else {
        (void)swprintf_s(text.data(), text.size(),
                         L"Runtime unavailable\r\nResult %d\r\nStage %u\r\nRecovery %ls\r\nAttempt %u\r\nNext %llu ns",
                         fault_, static_cast<uint32_t>(pipeline_.last_stage()),
                         pipeline_recovery_.pending() ? L"pending" : L"inactive", pipeline_recovery_.attempt(),
                         static_cast<unsigned long long>(pipeline_recovery_.next_attempt_ns()));
    }
#else
    (void)wcscpy_s(text.data(), text.size(), L"Runtime unavailable in this build.");
#endif
    if (diagnostics_window_ != nullptr) (void)SetDlgItemTextW(diagnostics_window_, diagnostics_text, text.data());
}

SaccadeResult Application::request_restart() noexcept {
    restart_requested_ = true;
    if (PostMessageW(window_, WM_CLOSE, 0, 0) != 0) return SACCADE_OK;
    restart_requested_ = false;
    return SACCADE_ERROR_BACKEND;
}

SaccadeResult Application::launch_replacement() noexcept {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length == executable.size()) return SACCADE_ERROR_BACKEND;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.data(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) ==
        0) {
        return SACCADE_ERROR_BACKEND;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return SACCADE_OK;
}

void Application::shutdown() noexcept {
    if (diagnostics_window_ != nullptr) {
        DestroyWindow(diagnostics_window_);
        diagnostics_window_ = nullptr;
    }
    if (bindings_window_ != nullptr) {
        DestroyWindow(bindings_window_);
        bindings_window_ = nullptr;
    }
    if (settings_window_ != nullptr) {
        if (settings_.editing()) (void)settings_.cancel();
        DestroyWindow(settings_window_);
        settings_window_ = nullptr;
    }
#if defined(SACCADE_HAS_WINDOWS_ML)
    if (runtime_timer_ != nullptr) {
        (void)CancelWaitableTimer(runtime_timer_);
        (void)CloseHandle(runtime_timer_);
        runtime_timer_ = nullptr;
        next_runtime_tick_ns_ = 0;
    }
    if (session_notifications_) {
        (void)WTSUnRegisterSessionNotification(window_);
        session_notifications_ = false;
    }
#endif
    if (tray_added_) {
        tray_.uFlags = 0;
        (void)Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_added_ = false;
    }
    if (hotkeys_initialized_) {
        (void)hotkeys_.shutdown();
        hotkeys_initialized_ = false;
    }
#if defined(SACCADE_HAS_WINDOWS_ML)
    pipeline_recovery_.complete();
    (void)shutdown_pipeline();
    if (runtime_scheduling_.initialized()) (void)runtime_scheduling_.shutdown();
#endif
    if (settings_initialized_) {
        (void)settings_.shutdown();
        settings_initialized_ = false;
    }
    if (host_initialized_) {
        (void)host_.shutdown();
        host_initialized_ = false;
    }
    initialized_ = false;
    if (previous_dpi_context_ != nullptr) {
        (void)SetThreadDpiAwarenessContext(previous_dpi_context_);
        previous_dpi_context_ = nullptr;
    }
}

LRESULT CALLBACK Application::diagnostics_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app != nullptr) app->diagnostics_window_ = window;
    }
    if (app == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        app->create_diagnostics_view();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == diagnostics_view && HIWORD(wparam) == CBN_SELCHANGE) {
            app->refresh_diagnostics();
            return 0;
        }
        if (LOWORD(wparam) >= diagnostics_capture && LOWORD(wparam) <= diagnostics_clear) {
            SaccadeResult result = SACCADE_ERROR_UNSUPPORTED;
            uint64_t plan_bytes = 0;
            uint32_t command_count = 0;
#if defined(SACCADE_HAS_WINDOWS_ML)
            saccade::application::DebuggerPlanView plan{};
            if (LOWORD(wparam) == diagnostics_capture)
                result = app->pipeline_.debug_capture_scene();
            else if (LOWORD(wparam) == diagnostics_dry_run)
                result = app->pipeline_.debug_dry_run(app->timestamp_ns(), &plan);
            else if (LOWORD(wparam) == diagnostics_replay)
                result = app->pipeline_.debug_replay(&plan);
            else
                result = app->pipeline_.debug_clear();
            plan_bytes = plan.bytes.size;
            command_count = plan.plan.header == nullptr ? 0 : plan.plan.header->command_count;
#endif
            (void)swprintf_s(app->debugger_operation_.data(), app->debugger_operation_.size(),
                             L"Operation %u: result %d  bytes %llu  commands %u", LOWORD(wparam), result,
                             static_cast<unsigned long long>(plan_bytes), command_count);
            app->refresh_diagnostics();
            return 0;
        }
        if (LOWORD(wparam) == diagnostics_arm_fault) {
            SaccadeResult result = SACCADE_ERROR_UNSUPPORTED;
            const LRESULT selected = SendDlgItemMessageW(window, diagnostics_fault, CB_GETCURSEL, 0, 0);
#if defined(SACCADE_HAS_WINDOWS_ML)
            if (selected != CB_ERR)
                result = app->pipeline_.debug_arm_fault(static_cast<saccade::application::DebugFaultPoint>(selected), 1,
                                                        SACCADE_ERROR_BACKEND);
#endif
            (void)swprintf_s(app->debugger_operation_.data(), app->debugger_operation_.size(),
                             L"Arm fault %lld: result %d", static_cast<long long>(selected), result);
            app->refresh_diagnostics();
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_TIMER:
        if (wparam == diagnostics_timer_identifier) app->refresh_diagnostics();
        return 0;
    case WM_GETMINMAXINFO: {
        RECT minimum{0, 0, saccade::application::debugger_minimum_width, saccade::application::debugger_minimum_height};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD extended_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        if (AdjustWindowRectExForDpi(&minimum, style, FALSE, extended_style, GetDpiForWindow(window)) != 0) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = minimum.right - minimum.left;
            limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
        }
        return 0;
    }
    case WM_SIZE:
        app->layout_diagnostics_view(static_cast<int32_t>(LOWORD(lparam)), static_cast<int32_t>(HIWORD(lparam)));
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        (void)KillTimer(window, diagnostics_timer_identifier);
        app->diagnostics_window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK Application::bindings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app != nullptr) app->bindings_window_ = window;
    }
    if (app == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        app->create_binding_view();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == binding_command && HIWORD(wparam) == CBN_SELCHANGE) {
            app->load_selected_binding();
            return 0;
        }
        if (LOWORD(wparam) >= binding_visual_first && LOWORD(wparam) < binding_visual_first + binding_visual_count) {
            const LRESULT key_index = LOWORD(wparam) - binding_visual_first;
            (void)SendDlgItemMessageW(window, binding_key, CB_SETCURSEL, key_index, 0);
            return 0;
        }
        if (LOWORD(wparam) == binding_set || LOWORD(wparam) == binding_remove) {
            const SaccadeResult result = app->modify_binding(LOWORD(wparam) == binding_remove);
            if (result != SACCADE_OK && result != SACCADE_ERROR_ALREADY_EXISTS)
                MessageBoxW(window, L"Binding was not changed.", application_name, MB_OK | MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wparam) == binding_done) {
            const SaccadeResult committed = app->settings_.commit();
            if (committed == SACCADE_OK) {
                DestroyWindow(window);
                if (app->settings_window_ != nullptr) DestroyWindow(app->settings_window_);
            } else {
                MessageBoxW(window, L"Bindings were not applied.", application_name, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        app->bindings_window_ = nullptr;
        if (app->settings_window_ != nullptr) {
            EnableWindow(app->settings_window_, TRUE);
            (void)SetForegroundWindow(app->settings_window_);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK Application::settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app != nullptr) app->settings_window_ = window;
    }
    if (app == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        app->create_settings_view();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case settings_apply:
        case settings_defaults: {
            const SaccadeResult committed = app->commit_settings(LOWORD(wparam) == settings_defaults);
            if (committed == SACCADE_OK) {
                DestroyWindow(window);
            } else {
                MessageBoxW(window, L"Settings were not applied.", application_name, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case settings_reset_page_button: {
            const SaccadeResult reset = app->reset_settings_view_page();
            if (reset == SACCADE_OK) {
                DestroyWindow(window);
            } else {
                MessageBoxW(window, L"Settings page was not reset.", application_name, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case settings_import: {
            const SaccadeResult imported = app->import_settings_document();
            if (imported == SACCADE_OK)
                DestroyWindow(window);
            else if (imported != SACCADE_ERROR_CANCELLED)
                MessageBoxW(window, L"Settings were not imported.", application_name, MB_OK | MB_ICONERROR);
            return 0;
        }
        case settings_export: {
            const SaccadeResult exported = app->export_settings_document();
            if (exported == SACCADE_OK) {
                (void)app->settings_.cancel();
                DestroyWindow(window);
            } else if (exported != SACCADE_ERROR_CANCELLED) {
                MessageBoxW(window, L"Settings were not exported.", application_name, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case settings_bindings: {
            const SaccadeResult opened = app->show_binding_editor();
            if (opened != SACCADE_OK)
                MessageBoxW(window, L"Bindings could not be opened.", application_name, MB_OK | MB_ICONERROR);
            return 0;
        }
        case settings_cancel:
            if (app->settings_.editing()) (void)app->settings_.cancel();
            DestroyWindow(window);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    case WM_MOUSEWHEEL: {
        const int lines = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        const UINT command = lines > 0 ? SB_LINEUP : SB_LINEDOWN;
        for (int line = 0; line < std::abs(lines); ++line)
            SendMessageW(window, WM_VSCROLL, MAKEWPARAM(command, 0), 0);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_ALL;
        if (GetScrollInfo(window, SB_VERT, &scroll) == 0) return 0;
        int position = app->settings_scroll_y_;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            position -= settings_row_height;
            break;
        case SB_LINEDOWN:
            position += settings_row_height;
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(scroll.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(scroll.nPage);
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            position = scroll.nTrackPos;
            break;
        default:
            return 0;
        }
        position = std::clamp(position, 0, app->settings_scroll_max_);
        if (position == app->settings_scroll_y_) return 0;
        const int delta = app->settings_scroll_y_ - position;
        app->settings_scroll_y_ = position;
        (void)ScrollWindowEx(window, 0, delta, nullptr, nullptr, nullptr, nullptr,
                             SW_ERASE | SW_INVALIDATE | SW_SCROLLCHILDREN);
        scroll.fMask = SIF_POS;
        scroll.nPos = position;
        (void)SetScrollInfo(window, SB_VERT, &scroll, TRUE);
        return 0;
    }
    case WM_CLOSE:
        if (app->settings_.editing()) (void)app->settings_.cancel();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        app->settings_window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK Application::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    Application* app = owner_;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (app == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case tray_message:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT)
            app->show_menu();
        return 0;
    case WM_COMMAND: {
        if (LOWORD(wparam) == menu_diagnostics) {
            app->show_diagnostics();
            return 0;
        }
        Command command = Command::open_settings;
        switch (LOWORD(wparam)) {
        case menu_settings:
            command = Command::open_settings;
            break;
        case menu_suspend:
            command = Command::suspend_toggle;
            break;
        case menu_restart:
            command = Command::restart;
            break;
        case menu_quit:
            command = Command::quit;
            break;
        default:
            return 0;
        }
        (void)app->host_.dispatch(CommandEvent{app->timestamp_ns(), command});
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
#if defined(SACCADE_HAS_WINDOWS_ML)
    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        if (app->pipeline_initialized_) {
            const SaccadeResult result = app->pipeline_.refresh_topology();
            if (result != SACCADE_OK) {
                if (result == SACCADE_ERROR_BACKEND)
                    app->begin_pipeline_recovery(result, app->timestamp_ns());
                else {
                    app->fault_ = result;
                    app->update_tray();
                }
            }
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            (void)app->set_input_available(false);
        } else if (wparam == PBT_APMRESUMEAUTOMATIC && app->pipeline_initialized_) {
            (void)app->set_input_available(true);
            if (app->pipeline_initialized_) {
                const SaccadeResult result = app->pipeline_.refresh_topology();
                if (result == SACCADE_ERROR_BACKEND)
                    app->begin_pipeline_recovery(result, app->timestamp_ns());
                else if (result != SACCADE_OK) {
                    app->fault_ = result;
                    app->update_tray();
                }
            }
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT || wparam == WTS_REMOTE_DISCONNECT) {
            (void)app->set_input_available(false);
        } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT || wparam == WTS_REMOTE_CONNECT) {
            (void)app->set_input_available(true);
        }
        return 0;
#endif
    case WM_ENDSESSION:
        if (wparam != 0) app->shutdown();
        return 0;
    case WM_DESTROY:
        app->shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

int Application::run(HINSTANCE instance, int show_command) noexcept {
    const SaccadeResult started = initialize(instance, show_command);
    if (started != SACCADE_OK) {
        shutdown();
        return static_cast<int>(started);
    }
    MSG message{};
#if defined(SACCADE_HAS_WINDOWS_ML)
    bool running = true;
    while (running) {
        const DWORD waited =
            MsgWaitForMultipleObjectsEx(1, &runtime_timer_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waited == WAIT_OBJECT_0) {
            advance_pipeline();
            if (arm_runtime_timer(timestamp_ns()) != SACCADE_OK) {
                message.wParam = static_cast<WPARAM>(SACCADE_ERROR_BACKEND);
                break;
            }
            continue;
        }
        if (waited != WAIT_OBJECT_0 + 1U) {
            message.wParam = static_cast<WPARAM>(SACCADE_ERROR_BACKEND);
            break;
        }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
#else
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
    shutdown();
    if (restart_requested_) {
        const SaccadeResult restarted = launch_replacement();
        if (restarted != SACCADE_OK) return static_cast<int>(restarted);
    }
    return static_cast<int>(message.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    static Application application;
    return application.run(instance, show_command);
}

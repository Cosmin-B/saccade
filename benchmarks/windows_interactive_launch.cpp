#include "core/stack_string_builder.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using saccade::core::StackStringBuilder;

enum class ToolResult : int {
    success,
    usage,
    output_directory_failed,
    desktop_process_not_found,
    process_open_failed,
    token_open_failed,
    token_duplicate_failed,
    command_line_too_long,
    log_open_failed,
    launch_failed,
    wait_failed,
    completion_write_failed,
    child_failed
};

template <size_t Capacity> class WideStackStringBuilder final {
  public:
    bool append(const wchar_t* text) noexcept {
        if (text == nullptr) return false;
        while (*text != L'\0') {
            if (!append(*text)) return false;
            ++text;
        }
        return true;
    }

    bool append(wchar_t value) noexcept {
        if (size_ == Capacity) return false;
        storage_[size_++] = value;
        storage_[size_] = L'\0';
        return true;
    }

    bool append_quoted(const wchar_t* text) noexcept {
        if (!append(L'"')) return false;
        while (text != nullptr && *text != L'\0') {
            if (*text == L'"' && !append(L'\\')) return false;
            if (!append(*text)) return false;
            ++text;
        }
        return append(L'"');
    }

    [[nodiscard]] wchar_t* data() noexcept { return storage_.data(); }

  private:
    std::array<wchar_t, Capacity + 1> storage_{};
    size_t size_ = 0;
};

int result(ToolResult value) noexcept {
    return static_cast<int>(value);
}

void emit_win32_failure(const char* operation, DWORD error) noexcept {
    StackStringBuilder<256> text;
    if (!text.append("win32_failure operation=") || !text.append(operation) || !text.append(" error=") ||
        !text.append_unsigned(error) || !text.append('\n'))
        return;
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

bool parse_session(const wchar_t* text, DWORD* output) noexcept {
    if (text == nullptr || output == nullptr || *text == L'\0') return false;
    DWORD value = 0;
    while (*text != L'\0') {
        if (*text < L'0' || *text > L'9') return false;
        const DWORD digit = static_cast<DWORD>(*text - L'0');
        if (value > (MAXDWORD - digit) / 10U) return false;
        value = value * 10U + digit;
        ++text;
    }
    *output = value;
    return true;
}

DWORD find_desktop_process(DWORD session_id) noexcept {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD process_id = 0;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            DWORD process_session = 0;
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") == 0 &&
                ProcessIdToSessionId(entry.th32ProcessID, &process_session) != FALSE && process_session == session_id) {
                process_id = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return process_id;
}

bool make_path(const wchar_t* directory, const wchar_t* name, WideStackStringBuilder<1024>* output) noexcept {
    return output->append(directory) && output->append(L'\\') && output->append(name);
}

bool write_completion(const wchar_t* directory, DWORD exit_code) noexcept {
    WideStackStringBuilder<1024> path;
    if (!make_path(directory, L"complete.txt", &path)) return false;
    const HANDLE file =
        CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    StackStringBuilder<32> text;
    DWORD written = 0;
    const bool ready = text.append_unsigned(exit_code) && text.append('\n');
    const bool saved =
        ready && WriteFile(file, text.view().data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
        written == static_cast<DWORD>(text.size());
    const bool closed = CloseHandle(file) != FALSE;
    return saved && closed;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4 || argc > 7) return result(ToolResult::usage);
    DWORD session_id = 0;
    if (!parse_session(argv[1], &session_id)) return result(ToolResult::usage);
    if (CreateDirectoryW(argv[3], nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) {
        return result(ToolResult::output_directory_failed);
    }

    const DWORD desktop_process_id = find_desktop_process(session_id);
    if (desktop_process_id == 0) {
        return result(ToolResult::desktop_process_not_found);
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, desktop_process_id);
    if (process == nullptr) return result(ToolResult::process_open_failed);
    HANDLE source_token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &source_token) == FALSE) {
        CloseHandle(process);
        return result(ToolResult::token_open_failed);
    }
    HANDLE primary_token = nullptr;
    if (DuplicateTokenEx(source_token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary_token) ==
        FALSE) {
        CloseHandle(source_token);
        CloseHandle(process);
        return result(ToolResult::token_duplicate_failed);
    }

    WideStackStringBuilder<4096> command_line;
    bool command_ready =
        command_line.append_quoted(argv[2]) && command_line.append(L' ') && command_line.append_quoted(argv[3]);
    for (int index = 4; command_ready && index < argc; ++index) {
        command_ready = command_line.append(L' ') && command_line.append_quoted(argv[index]);
    }
    if (!command_ready) {
        CloseHandle(primary_token);
        CloseHandle(source_token);
        CloseHandle(process);
        return result(ToolResult::command_line_too_long);
    }

    WideStackStringBuilder<1024> log_path;
    if (!make_path(argv[3], L"capture.log", &log_path)) {
        CloseHandle(primary_token);
        CloseHandle(source_token);
        CloseHandle(process);
        return result(ToolResult::command_line_too_long);
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE log = CreateFileW(log_path.data(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        CloseHandle(primary_token);
        CloseHandle(source_token);
        CloseHandle(process);
        return result(ToolResult::log_open_failed);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION child{};
    const BOOL launched =
        CreateProcessWithTokenW(primary_token, LOGON_WITH_PROFILE, argv[2], command_line.data(),
                                CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(primary_token);
    CloseHandle(source_token);
    CloseHandle(process);
    if (launched == FALSE) {
        const DWORD launch_error = GetLastError();
        CloseHandle(log);
        emit_win32_failure("create_process_with_token", launch_error);
        return result(ToolResult::launch_failed);
    }

    const DWORD waited = WaitForSingleObject(child.hProcess, INFINITE);
    DWORD child_exit_code = MAXDWORD;
    const bool exit_ready = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &child_exit_code) != FALSE;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CloseHandle(log);
    if (!exit_ready) return result(ToolResult::wait_failed);
    if (!write_completion(argv[3], child_exit_code)) {
        return result(ToolResult::completion_write_failed);
    }
    return child_exit_code == 0 ? result(ToolResult::success) : result(ToolResult::child_failed);
}

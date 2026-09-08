// Windows HAL: Win32 + DWM + shell scaling, with PEB-based command line reads.
#if defined(_WIN32)

#include <contextsnap/core/geometry.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/matching.hpp>
#include <contextsnap/hal/platform.hpp>

#include <windows.h>

#include <dwmapi.h>
#include <psapi.h>
#include <shellscalingapi.h>
#include <winternl.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace contextsnap::hal {
namespace {

using core::CaptureOptions;
using core::CursorState;
using core::MonitorInfo;
using core::ProcessInfo;
using core::Rect;
using core::Result;
using core::Status;
using core::WindowInfo;
using core::WindowState;

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

core::Error win32_error(const std::string& what) {
    const DWORD code = ::GetLastError();
    core::Error error;
    error.code = code == ERROR_ACCESS_DENIED ? core::ErrorCode::PermissionDenied
                                             : core::ErrorCode::Internal;
    error.message = what + " (win32 error " + std::to_string(code) + ")";
    error.context = "hal.windows";
    error.native_code = static_cast<std::int32_t>(code);
    return error;
}

Rect from_rect(const RECT& rect) {
    return Rect{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}

bool cloaked(HWND window) {
    DWORD value = 0;
    if (::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value)) == S_OK) {
        return value != 0;
    }
    return false;
}

/// Reads another process' argv and working directory out of its PEB.
/// This is the only reliable way on Windows; it needs PROCESS_VM_READ.
void read_peb_strings(HANDLE process, ProcessInfo& info) {
    using NtQueryInformationProcessFn =
        NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (query == nullptr) {
        return;
    }
    PROCESS_BASIC_INFORMATION basic{};
    ULONG returned = 0;
    if (query(process, 0 /* ProcessBasicInformation */, &basic, sizeof(basic), &returned) != 0 ||
        basic.PebBaseAddress == nullptr) {
        return;
    }
    PEB peb{};
    if (::ReadProcessMemory(process, basic.PebBaseAddress, &peb, sizeof(peb), nullptr) == 0) {
        return;
    }
    RTL_USER_PROCESS_PARAMETERS parameters{};
    if (::ReadProcessMemory(process, peb.ProcessParameters, &parameters, sizeof(parameters),
                            nullptr) == 0) {
        return;
    }

    const auto read_unicode = [&](const UNICODE_STRING& value) -> std::string {
        if (value.Length == 0 || value.Buffer == nullptr) {
            return {};
        }
        std::wstring buffer(value.Length / sizeof(wchar_t), L'\0');
        if (::ReadProcessMemory(process, value.Buffer, buffer.data(), value.Length, nullptr) == 0) {
            return {};
        }
        return to_utf8(buffer);
    };

    const std::string command_line = read_unicode(parameters.CommandLine);
    info.working_directory = read_unicode(parameters.CurrentDirectory.DosPath);

    // Minimal CommandLineToArgv equivalent: quotes group, everything else splits
    // on whitespace. Good enough to relaunch an application faithfully.
    std::string current;
    bool quoted = false;
    for (const char character : command_line) {
        if (character == '"') {
            quoted = !quoted;
        } else if (!quoted && (character == ' ' || character == '\t')) {
            if (!current.empty()) {
                info.command_line.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        info.command_line.push_back(current);
    }
}

ProcessInfo read_process(DWORD pid) {
    ProcessInfo info;
    info.pid = pid;
    const HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        return info;
    }
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (::QueryFullProcessImageNameW(process, 0, path, &size) != 0) {
        info.executable_path = to_utf8(std::wstring(path, size));
    }
    read_peb_strings(process, info);
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &token) != 0) {
        TOKEN_ELEVATION elevation{};
        DWORD length = sizeof(elevation);
        if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                                  &length) != 0) {
            elevated = elevation.TokenIsElevated != 0;
        }
        ::CloseHandle(token);
    }
    info.elevated = elevated != FALSE;
    info.app_id = core::matching::normalize_app_id(info.executable_path);
    info.single_instance = core::matching::is_single_instance_app(info.app_id);
    wchar_t user[256] = {};
    DWORD user_size = static_cast<DWORD>(std::size(user));
    if (::GetUserNameW(user, &user_size) != 0) {
        info.user = to_utf8(std::wstring(user, user_size == 0 ? 0 : user_size - 1));
    }
    ::CloseHandle(process);
    return info;
}

struct EnumerationContext {
    std::vector<HWND> windows;
};

BOOL CALLBACK collect_window(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<EnumerationContext*>(parameter);
    if (::IsWindowVisible(window) == 0 || cloaked(window)) {
        return TRUE;
    }
    if (::GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    const LONG_PTR extended = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;  // Tool palettes are not part of a restorable layout.
    }
    if (::GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;  // Owned dialogs follow their parent.
    }
    context->windows.push_back(window);
    return TRUE;
}

WindowState state_of(HWND window, const Rect& frame) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (::GetWindowPlacement(window, &placement) == 0) {
        return WindowState::Normal;
    }
    if (placement.showCmd == SW_SHOWMINIMIZED) {
        return WindowState::Minimized;
    }
    if (placement.showCmd == SW_SHOWMAXIMIZED) {
        return WindowState::Maximized;
    }
    // Borderless windows that exactly cover their monitor are fullscreen.
    const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) != 0) {
        const Rect bounds = from_rect(info.rcMonitor);
        if (frame.x <= bounds.x && frame.y <= bounds.y &&
            frame.width >= bounds.width && frame.height >= bounds.height) {
            return WindowState::Fullscreen;
        }
    }
    return WindowState::Normal;
}

class WindowsWindows final : public WindowProvider {
public:
    Result<std::vector<WindowInfo>> enumerate(const CaptureOptions& options) override {
        EnumerationContext context;
        if (::EnumWindows(&collect_window, reinterpret_cast<LPARAM>(&context)) == 0 &&
            context.windows.empty()) {
            return win32_error("EnumWindows failed");
        }
        const HWND foreground = ::GetForegroundWindow();
        std::vector<WindowInfo> windows;
        std::int32_t z = 0;
        for (const HWND handle : context.windows) {
            WindowInfo info;
            info.native_handle = reinterpret_cast<std::uint64_t>(handle);

            std::wstring title(static_cast<std::size_t>(::GetWindowTextLengthW(handle)) + 1, L'\0');
            const int length = ::GetWindowTextW(handle, title.data(),
                                                static_cast<int>(title.size()));
            info.title = to_utf8(title.substr(0, static_cast<std::size_t>(std::max(0, length))));

            wchar_t class_name[256] = {};
            ::GetClassNameW(handle, class_name, static_cast<int>(std::size(class_name)));
            info.window_class = to_utf8(class_name);

            // DWMWA_EXTENDED_FRAME_BOUNDS excludes the invisible resize border,
            // so restoring the value reproduces the visible layout exactly.
            RECT bounds{};
            if (::DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds,
                                        sizeof(bounds)) == S_OK) {
                info.frame = from_rect(bounds);
                info.captured_geometry_reliable = true;
            } else if (::GetWindowRect(handle, &bounds) != 0) {
                info.frame = from_rect(bounds);
            }
            RECT client{};
            if (::GetClientRect(handle, &client) != 0) {
                POINT origin{client.left, client.top};
                ::ClientToScreen(handle, &origin);
                info.client_area = Rect{origin.x, origin.y, client.right - client.left,
                                        client.bottom - client.top};
            }
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (::GetWindowPlacement(handle, &placement) != 0) {
                info.restored_frame = from_rect(placement.rcNormalPosition);
            } else {
                info.restored_frame = info.frame;
            }
            info.state = state_of(handle, info.frame);
            if (!options.include_minimized && info.state == WindowState::Minimized) {
                continue;
            }
            info.z_order = z++;
            info.focused = handle == foreground;
            info.always_on_top =
                (::GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            const LONG_PTR style = ::GetWindowLongPtrW(handle, GWL_STYLE);
            info.resizable = (style & WS_THICKFRAME) != 0;
            info.minimizable = (style & WS_MINIMIZEBOX) != 0;

            DWORD pid = 0;
            ::GetWindowThreadProcessId(handle, &pid);
            info.process = read_process(pid);
            info.browser = core::matching::detect_browser(info.process.app_id, info.window_class);

            const HMONITOR monitor = ::MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (::GetMonitorInfoW(monitor, &monitor_info) != 0) {
                info.monitor_id = to_utf8(monitor_info.szDevice);
            }
            windows.push_back(std::move(info));
        }
        return windows;
    }

    Result<WindowInfo> get(std::uint64_t handle) override {
        CaptureOptions options;
        options.include_minimized = true;
        auto all = enumerate(options);
        if (!all) {
            return all.error();
        }
        for (WindowInfo& window : all.value()) {
            if (window.native_handle == handle) {
                return window;
            }
        }
        return core::err::not_found("window is gone", "hal.windows");
    }

    Status set_frame(std::uint64_t handle, const Rect& frame) override {
        const HWND window = reinterpret_cast<HWND>(handle);
        if (::SetWindowPos(window, nullptr, frame.x, frame.y, frame.width, frame.height,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) == 0) {
            return win32_error("SetWindowPos failed");
        }
        return Status::success();
    }

    Status set_state(std::uint64_t handle, WindowState state) override {
        const HWND window = reinterpret_cast<HWND>(handle);
        int command = SW_SHOWNOACTIVATE;
        switch (state) {
            case WindowState::Minimized: command = SW_SHOWMINNOACTIVE; break;
            case WindowState::Maximized: command = SW_SHOWMAXIMIZED; break;
            case WindowState::Fullscreen: command = SW_SHOWMAXIMIZED; break;
            case WindowState::Hidden: command = SW_HIDE; break;
            default: command = SW_SHOWNOACTIVATE; break;
        }
        ::ShowWindow(window, command);
        return Status::success();
    }

    Status raise(std::uint64_t handle) override {
        if (::SetWindowPos(reinterpret_cast<HWND>(handle), HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) == 0) {
            return win32_error("raise failed");
        }
        return Status::success();
    }

    Status focus(std::uint64_t handle) override {
        const HWND window = reinterpret_cast<HWND>(handle);
        // Windows only lets the foreground process steal focus, so attach to the
        // current foreground thread for the duration of the call.
        const DWORD foreground_thread =
            ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
        const DWORD this_thread = ::GetCurrentThreadId();
        const bool attached =
            foreground_thread != 0 && foreground_thread != this_thread &&
            ::AttachThreadInput(this_thread, foreground_thread, TRUE) != 0;
        const BOOL result = ::SetForegroundWindow(window);
        if (attached) {
            ::AttachThreadInput(this_thread, foreground_thread, FALSE);
        }
        if (result == 0) {
            ::SetActiveWindow(window);
        }
        return Status::success();
    }

    Status set_always_on_top(std::uint64_t handle, bool enabled) override {
        if (::SetWindowPos(reinterpret_cast<HWND>(handle),
                           enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) == 0) {
            return win32_error("topmost change failed");
        }
        return Status::success();
    }

    Status close(std::uint64_t handle) override {
        ::PostMessageW(reinterpret_cast<HWND>(handle), WM_CLOSE, 0, 0);
        return Status::success();
    }

    Result<WindowInfo> wait_for_window(std::uint64_t pid, std::string_view title_hint,
                                       std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        CaptureOptions options;
        options.include_minimized = true;
        while (std::chrono::steady_clock::now() < deadline) {
            auto windows = enumerate(options);
            if (windows) {
                const WindowInfo* best = nullptr;
                double best_score = 0.0;
                for (const WindowInfo& window : windows.value()) {
                    if (pid != 0 && window.process.pid != pid) {
                        continue;
                    }
                    const double score =
                        title_hint.empty()
                            ? 1.0
                            : core::matching::title_similarity(window.title, title_hint);
                    if (score > best_score) {
                        best_score = score;
                        best = &window;
                    }
                }
                if (best != nullptr) {
                    return *best;
                }
            }
            ::Sleep(80);
        }
        return core::Error{core::ErrorCode::Timeout, "no window appeared in time", "hal.windows",
                          0};
    }
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == 0) {
        return TRUE;
    }
    MonitorInfo entry;
    entry.id = to_utf8(info.szDevice);
    entry.name = entry.id;
    entry.bounds = from_rect(info.rcMonitor);
    entry.work_area = from_rect(info.rcWork);
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // GetDpiForMonitor lives in Shcore, which is absent on Windows 7.
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(
        ::GetProcAddress(::LoadLibraryW(L"Shcore.dll"), "GetDpiForMonitor"));
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    if (get_dpi != nullptr) {
        get_dpi(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpi_x, &dpi_y);
    }
    entry.dpi = static_cast<std::int32_t>(dpi_x);
    entry.scale_factor = static_cast<double>(dpi_x) / 96.0;
    monitors->push_back(std::move(entry));
    return TRUE;
}

class WindowsDisplays final : public DisplayProvider {
public:
    Result<std::vector<MonitorInfo>> enumerate() override {
        std::vector<MonitorInfo> monitors;
        if (::EnumDisplayMonitors(nullptr, nullptr, &collect_monitor,
                                  reinterpret_cast<LPARAM>(&monitors)) == 0) {
            return win32_error("EnumDisplayMonitors failed");
        }
        return monitors;
    }

    Result<Rect> virtual_bounds() override {
        auto monitors = enumerate();
        if (!monitors) {
            return monitors.error();
        }
        return core::virtual_bounds(monitors.value());
    }
};

class WindowsCursor final : public CursorProvider {
public:
    Result<CursorState> query() override {
        POINT point{};
        if (::GetCursorPos(&point) == 0) {
            return win32_error("GetCursorPos failed");
        }
        CursorState cursor;
        cursor.x = point.x;
        cursor.y = point.y;
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info) != 0) {
            cursor.monitor_id = to_utf8(info.szDevice);
        }
        return cursor;
    }

    Status warp(std::int32_t x, std::int32_t y) override {
        if (::SetCursorPos(x, y) == 0) {
            return win32_error("SetCursorPos failed");
        }
        return Status::success();
    }
};

class WindowsProcesses final : public ProcessProvider {
public:
    Result<ProcessInfo> info(std::uint64_t pid) override {
        ProcessInfo process = read_process(static_cast<DWORD>(pid));
        if (process.executable_path.empty()) {
            return core::err::not_found("no process " + std::to_string(pid), "hal.windows");
        }
        return process;
    }

    Result<std::uint64_t> launch(const ProcessInfo& process) override {
        if (process.executable_path.empty()) {
            return core::err::invalid("process has no executable path", "hal.windows");
        }
        std::string command;
        if (process.command_line.empty()) {
            command = "\"" + process.executable_path + "\"";
        } else {
            for (const std::string& argument : process.command_line) {
                if (!command.empty()) {
                    command.push_back(' ');
                }
                command += argument.find(' ') == std::string::npos ? argument
                                                                   : ("\"" + argument + "\"");
            }
        }
        std::wstring wide_command = to_wide(command);
        const std::wstring directory = to_wide(process.working_directory);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr,
                             directory.empty() ? nullptr : directory.c_str(), &startup,
                             &information) == 0) {
            return win32_error("CreateProcess failed for " + process.executable_path);
        }
        ::CloseHandle(information.hThread);
        ::CloseHandle(information.hProcess);
        return static_cast<std::uint64_t>(information.dwProcessId);
    }

    bool is_running(std::uint64_t pid) override {
        const HANDLE process =
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (process == nullptr) {
            return false;
        }
        DWORD code = 0;
        const bool running =
            ::GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
        ::CloseHandle(process);
        return running;
    }

    Result<std::vector<std::uint64_t>> find_by_app_id(std::string_view app_id) override {
        std::vector<DWORD> pids(2048);
        DWORD needed = 0;
        if (::EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)),
                            &needed) == 0) {
            return win32_error("EnumProcesses failed");
        }
        std::vector<std::uint64_t> matches;
        const std::size_t count = needed / sizeof(DWORD);
        for (std::size_t i = 0; i < count; ++i) {
            const ProcessInfo process = read_process(pids[i]);
            if (!process.app_id.empty() && process.app_id == app_id) {
                matches.push_back(process.pid);
            }
        }
        return matches;
    }
};

/// Windows exposes only "is this window on the current desktop" publicly, so
/// virtual desktop restore is limited to keeping windows where they are.
class WindowsWorkspaces final : public WorkspaceProvider {
public:
    Result<std::vector<Workspace>> enumerate() override {
        return core::err::unsupported(
            "Windows does not expose a public API for enumerating virtual desktops",
            "hal.windows");
    }
    Result<std::string> current() override {
        return core::err::unsupported("no public virtual desktop API", "hal.windows");
    }
    Status activate(std::string_view) override {
        return core::err::unsupported("no public virtual desktop API", "hal.windows");
    }
    Status move_window(std::uint64_t, std::string_view) override {
        return core::err::unsupported("no public virtual desktop API", "hal.windows");
    }
};

class WindowsPlatform final : public Platform {
public:
    WindowsPlatform() {
        capabilities_.enumerate_windows = true;
        capabilities_.window_geometry = true;
        capabilities_.set_window_geometry = true;
        capabilities_.set_window_state = true;
        capabilities_.z_order = true;
        capabilities_.focus_control = true;
        capabilities_.cursor_query = true;
        capabilities_.cursor_warp = true;
        capabilities_.process_command_line = true;
        capabilities_.process_working_directory = true;
        capabilities_.launch_processes = true;
        capabilities_.window_thumbnails = true;  // DWM thumbnails via DwmRegisterThumbnail.
        capabilities_.virtual_desktops = false;
        capabilities_.move_between_desktops = false;
        capabilities_.limitations.emplace_back(
            "Virtual desktop assignment is not restored: Windows has no public API for it");
        capabilities_.limitations.emplace_back(
            "Reading another process' command line requires the same integrity level; "
            "elevated apps are captured without argv");
    }

    [[nodiscard]] core::PlatformKind kind() const noexcept override {
        return core::PlatformKind::Windows;
    }

    [[nodiscard]] core::SessionType session_type() const noexcept override {
        return core::SessionType::Win32;
    }

    [[nodiscard]] std::string name() const override { return "Windows (Win32/DWM)"; }

    [[nodiscard]] std::string os_version() const override {
        // GetVersionEx lies for unmanifested apps; read the real build instead.
        HKEY key = nullptr;
        std::string version = "Windows";
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ,
                            &key) == ERROR_SUCCESS) {
            wchar_t buffer[128] = {};
            DWORD size = sizeof(buffer);
            if (::RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
                version += " " + to_utf8(buffer);
            }
            size = sizeof(buffer);
            if (::RegQueryValueExW(key, L"CurrentBuild", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
                version += " (build " + to_utf8(buffer) + ")";
            }
            ::RegCloseKey(key);
        }
        return version;
    }

    [[nodiscard]] const Capabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    WindowProvider& windows() override { return windows_; }
    DisplayProvider& displays() override { return displays_; }
    CursorProvider& cursor() override { return cursor_; }
    ProcessProvider& processes() override { return processes_; }
    WorkspaceProvider& workspaces() override { return workspaces_; }

    [[nodiscard]] std::vector<std::string> missing_permissions() override { return {}; }

    [[nodiscard]] Status request_permissions() override {
        // Win32 needs no runtime grant for same-user window management.
        return Status::success();
    }

private:
    Capabilities capabilities_;
    WindowsWindows windows_;
    WindowsDisplays displays_;
    WindowsCursor cursor_;
    WindowsProcesses processes_;
    WindowsWorkspaces workspaces_;
};

}  // namespace

Result<std::shared_ptr<Platform>> create_windows_platform() {
    // Per-monitor DPI awareness keeps captured rectangles in physical pixels.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return std::static_pointer_cast<Platform>(std::make_shared<WindowsPlatform>());
}

}  // namespace contextsnap::hal

#endif  // _WIN32

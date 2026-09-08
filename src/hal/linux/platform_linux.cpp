// Linux HAL: X11 backend (full support) plus a Wayland-aware degraded mode.
//
// Wayland deliberately hides global window state from ordinary clients, so the
// honest behaviour is to report reduced capabilities and clear limitations
// rather than to pretend the placement worked. XWayland clients are still
// visible through the X11 code path when a display is reachable.
#if defined(__linux__)

#include <contextsnap/core/geometry.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/matching.hpp>
#include <contextsnap/hal/platform.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

#if CONTEXTSNAP_HAS_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#endif

namespace contextsnap::hal {
namespace {

namespace fs = std::filesystem;
using core::CaptureOptions;
using core::CursorState;
using core::MonitorInfo;
using core::ProcessInfo;
using core::Rect;
using core::Result;
using core::Status;
using core::WindowInfo;
using core::WindowState;

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<std::string> read_cmdline(std::uint64_t pid) {
    const std::string raw = read_file(fs::path("/proc") / std::to_string(pid) / "cmdline");
    std::vector<std::string> argv;
    std::string current;
    for (const char character : raw) {
        if (character == '\0') {
            if (!current.empty()) {
                argv.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        argv.push_back(current);
    }
    return argv;
}

ProcessInfo read_process(std::uint64_t pid) {
    ProcessInfo info;
    info.pid = pid;
    std::error_code ec;
    const fs::path root = fs::path("/proc") / std::to_string(pid);
    info.executable_path = fs::read_symlink(root / "exe", ec).string();
    info.working_directory = fs::read_symlink(root / "cwd", ec).string();
    info.command_line = read_cmdline(pid);
    if (info.executable_path.empty() && !info.command_line.empty()) {
        info.executable_path = info.command_line.front();
    }
    info.app_id = core::matching::normalize_app_id(info.executable_path);
    info.single_instance = core::matching::is_single_instance_app(info.app_id);
    if (const char* user = std::getenv("USER"); user != nullptr) {
        info.user = user;
    }
    info.elevated = ::geteuid() == 0;
    return info;
}

bool wayland_session() {
    const char* session = std::getenv("XDG_SESSION_TYPE");
    const char* display = std::getenv("WAYLAND_DISPLAY");
    return (session != nullptr && std::strcmp(session, "wayland") == 0) ||
           (display != nullptr && *display != '\0');
}

// --- X11 backend -------------------------------------------------------------
#if CONTEXTSNAP_HAS_X11

/// RAII wrapper around the X display connection.
class X11Display {
public:
    X11Display() : display_(::XOpenDisplay(nullptr)) {}
    ~X11Display() {
        if (display_ != nullptr) {
            ::XCloseDisplay(display_);
        }
    }
    X11Display(const X11Display&) = delete;
    X11Display& operator=(const X11Display&) = delete;

    [[nodiscard]] bool valid() const noexcept { return display_ != nullptr; }
    [[nodiscard]] Display* get() const noexcept { return display_; }
    [[nodiscard]] Window root() const { return DefaultRootWindow(display_); }

    [[nodiscard]] Atom atom(const char* name) const {
        return ::XInternAtom(display_, name, False);
    }

    /// Reads a window property; returns an empty vector when it is missing.
    [[nodiscard]] std::vector<unsigned char> property(Window window, const char* name,
                                                      Atom type, unsigned long& items) const {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long bytes_after = 0;
        unsigned char* data = nullptr;
        items = 0;
        if (::XGetWindowProperty(display_, window, atom(name), 0, 65536, False, type,
                                 &actual_type, &actual_format, &items, &bytes_after,
                                 &data) != Success ||
            data == nullptr) {
            return {};
        }
        const std::size_t width = actual_format == 32 ? sizeof(long)
                                  : actual_format == 16 ? 2
                                                        : 1;
        std::vector<unsigned char> out(data, data + (items * width));
        ::XFree(data);
        return out;
    }

    [[nodiscard]] std::string text_property(Window window, const char* name) const {
        unsigned long items = 0;
        std::vector<unsigned char> data = property(window, name, AnyPropertyType, items);
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

    [[nodiscard]] long cardinal(Window window, const char* name, long fallback) const {
        unsigned long items = 0;
        std::vector<unsigned char> data = property(window, name, XA_CARDINAL, items);
        if (data.size() < sizeof(long)) {
            return fallback;
        }
        long value = 0;
        std::memcpy(&value, data.data(), sizeof(long));
        return value;
    }

private:
    Display* display_{nullptr};
};

X11Display& display() {
    static X11Display instance;
    return instance;
}

std::vector<Window> client_list(const X11Display& x11) {
    unsigned long items = 0;
    std::vector<unsigned char> data = x11.property(x11.root(), "_NET_CLIENT_LIST_STACKING",
                                                   XA_WINDOW, items);
    if (data.empty()) {
        data = x11.property(x11.root(), "_NET_CLIENT_LIST", XA_WINDOW, items);
    }
    std::vector<Window> windows(items);
    for (unsigned long i = 0; i < items; ++i) {
        long value = 0;
        std::memcpy(&value, data.data() + (i * sizeof(long)), sizeof(long));
        windows[i] = static_cast<Window>(value);
    }
    return windows;
}

WindowState state_of(const X11Display& x11, Window window) {
    unsigned long items = 0;
    const std::vector<unsigned char> data = x11.property(window, "_NET_WM_STATE", XA_ATOM, items);
    bool horizontal = false;
    bool vertical = false;
    for (unsigned long i = 0; i < items; ++i) {
        long value = 0;
        std::memcpy(&value, data.data() + (i * sizeof(long)), sizeof(long));
        const Atom entry = static_cast<Atom>(value);
        if (entry == x11.atom("_NET_WM_STATE_HIDDEN")) return WindowState::Minimized;
        if (entry == x11.atom("_NET_WM_STATE_FULLSCREEN")) return WindowState::Fullscreen;
        if (entry == x11.atom("_NET_WM_STATE_MAXIMIZED_HORZ")) horizontal = true;
        if (entry == x11.atom("_NET_WM_STATE_MAXIMIZED_VERT")) vertical = true;
    }
    return horizontal && vertical ? WindowState::Maximized : WindowState::Normal;
}

Rect frame_of(const X11Display& x11, Window window) {
    XWindowAttributes attributes{};
    if (::XGetWindowAttributes(x11.get(), window, &attributes) == 0) {
        return {};
    }
    int x = 0;
    int y = 0;
    Window child = 0;
    ::XTranslateCoordinates(x11.get(), window, x11.root(), 0, 0, &x, &y, &child);
    Rect frame{x, y, attributes.width, attributes.height};

    // _NET_FRAME_EXTENTS gives the decoration size so the saved rectangle
    // matches what the user sees, mirroring DWM extended frame bounds.
    unsigned long items = 0;
    const std::vector<unsigned char> extents =
        x11.property(window, "_NET_FRAME_EXTENTS", XA_CARDINAL, items);
    if (items >= 4) {
        long values[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            std::memcpy(&values[i], extents.data() + (static_cast<std::size_t>(i) * sizeof(long)),
                        sizeof(long));
        }
        frame.x -= static_cast<std::int32_t>(values[0]);
        frame.y -= static_cast<std::int32_t>(values[2]);
        frame.width += static_cast<std::int32_t>(values[0] + values[1]);
        frame.height += static_cast<std::int32_t>(values[2] + values[3]);
    }
    return frame;
}

class X11Windows final : public WindowProvider {
public:
    Result<std::vector<WindowInfo>> enumerate(const CaptureOptions& options) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display is reachable", "hal.linux");
        }
        std::vector<WindowInfo> windows;
        std::int32_t z = 0;
        Window focused = 0;
        int revert = 0;
        ::XGetInputFocus(x11.get(), &focused, &revert);

        for (const Window window : client_list(x11)) {
            WindowInfo info;
            info.native_handle = static_cast<std::uint64_t>(window);
            info.title = x11.text_property(window, "_NET_WM_NAME");
            if (info.title.empty()) {
                char* legacy = nullptr;
                if (::XFetchName(x11.get(), window, &legacy) != 0 && legacy != nullptr) {
                    info.title = legacy;
                    ::XFree(legacy);
                }
            }
            XClassHint hint{};
            if (::XGetClassHint(x11.get(), window, &hint) != 0) {
                info.window_class = hint.res_class == nullptr ? "" : hint.res_class;
                if (hint.res_name != nullptr) ::XFree(hint.res_name);
                if (hint.res_class != nullptr) ::XFree(hint.res_class);
            }
            info.state = state_of(x11, window);
            if (!options.include_minimized && info.state == WindowState::Minimized) {
                continue;
            }
            info.frame = frame_of(x11, window);
            info.client_area = info.frame;
            info.restored_frame = info.frame;
            info.z_order = z++;
            info.focused = window == focused;
            info.captured_geometry_reliable = true;

            const long desktop = x11.cardinal(window, "_NET_WM_DESKTOP", -1);
            if (desktop >= 0) {
                info.workspace_id = "ws-" + std::to_string(desktop);
            }
            const long pid = x11.cardinal(window, "_NET_WM_PID", 0);
            if (pid > 0) {
                info.process = read_process(static_cast<std::uint64_t>(pid));
            }
            info.browser = core::matching::detect_browser(info.process.app_id, info.window_class);
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
        return core::err::not_found("window " + std::to_string(handle) + " is gone", "hal.linux");
    }

    Status set_frame(std::uint64_t handle, const Rect& frame) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        ::XMoveResizeWindow(x11.get(), static_cast<Window>(handle), frame.x, frame.y,
                            static_cast<unsigned int>(std::max(1, frame.width)),
                            static_cast<unsigned int>(std::max(1, frame.height)));
        ::XFlush(x11.get());
        return Status::success();
    }

    Status set_state(std::uint64_t handle, WindowState state) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        const Window window = static_cast<Window>(handle);
        if (state == WindowState::Minimized) {
            ::XIconifyWindow(x11.get(), window, DefaultScreen(x11.get()));
            ::XFlush(x11.get());
            return Status::success();
        }
        // Maximise/fullscreen go through the EWMH client message protocol.
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = x11.atom("_NET_WM_STATE");
        event.xclient.format = 32;
        event.xclient.data.l[0] = state == WindowState::Normal ? 0 : 1;  // remove / add
        if (state == WindowState::Fullscreen) {
            event.xclient.data.l[1] = static_cast<long>(x11.atom("_NET_WM_STATE_FULLSCREEN"));
        } else {
            event.xclient.data.l[1] = static_cast<long>(x11.atom("_NET_WM_STATE_MAXIMIZED_HORZ"));
            event.xclient.data.l[2] = static_cast<long>(x11.atom("_NET_WM_STATE_MAXIMIZED_VERT"));
        }
        event.xclient.data.l[3] = 1;  // source: application
        ::XSendEvent(x11.get(), x11.root(), False,
                     SubstructureRedirectMask | SubstructureNotifyMask, &event);
        ::XFlush(x11.get());
        return Status::success();
    }

    Status raise(std::uint64_t handle) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        ::XRaiseWindow(x11.get(), static_cast<Window>(handle));
        ::XFlush(x11.get());
        return Status::success();
    }

    Status focus(std::uint64_t handle) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        const Window window = static_cast<Window>(handle);
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = x11.atom("_NET_ACTIVE_WINDOW");
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2;  // source: pager
        event.xclient.data.l[1] = CurrentTime;
        ::XSendEvent(x11.get(), x11.root(), False,
                     SubstructureRedirectMask | SubstructureNotifyMask, &event);
        ::XSetInputFocus(x11.get(), window, RevertToParent, CurrentTime);
        ::XFlush(x11.get());
        return Status::success();
    }

    Status set_always_on_top(std::uint64_t handle, bool enabled) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = static_cast<Window>(handle);
        event.xclient.message_type = x11.atom("_NET_WM_STATE");
        event.xclient.format = 32;
        event.xclient.data.l[0] = enabled ? 1 : 0;
        event.xclient.data.l[1] = static_cast<long>(x11.atom("_NET_WM_STATE_ABOVE"));
        event.xclient.data.l[3] = 1;
        ::XSendEvent(x11.get(), x11.root(), False,
                     SubstructureRedirectMask | SubstructureNotifyMask, &event);
        ::XFlush(x11.get());
        return Status::success();
    }

    Status close(std::uint64_t handle) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = static_cast<Window>(handle);
        event.xclient.message_type = x11.atom("_NET_CLOSE_WINDOW");
        event.xclient.format = 32;
        event.xclient.data.l[1] = 1;
        ::XSendEvent(x11.get(), x11.root(), False,
                     SubstructureRedirectMask | SubstructureNotifyMask, &event);
        ::XFlush(x11.get());
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
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
        }
        return core::Error{core::ErrorCode::Timeout, "no window appeared in time", "hal.linux", 0};
    }
};

class X11Displays final : public DisplayProvider {
public:
    Result<std::vector<MonitorInfo>> enumerate() override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display is reachable", "hal.linux");
        }
        std::vector<MonitorInfo> monitors;
        int count = 0;
        XRRMonitorInfo* list = ::XRRGetMonitors(x11.get(), x11.root(), True, &count);
        if (list == nullptr || count <= 0) {
            // Fall back to the screen dimensions when RandR is unavailable.
            MonitorInfo monitor;
            monitor.id = "screen-0";
            monitor.name = "Screen 0";
            monitor.bounds = {0, 0, DisplayWidth(x11.get(), 0), DisplayHeight(x11.get(), 0)};
            monitor.work_area = monitor.bounds;
            monitor.primary = true;
            monitors.push_back(std::move(monitor));
            return monitors;
        }
        for (int i = 0; i < count; ++i) {
            MonitorInfo monitor;
            char* name = ::XGetAtomName(x11.get(), list[i].name);
            monitor.name = name == nullptr ? ("Monitor " + std::to_string(i)) : name;
            if (name != nullptr) {
                ::XFree(name);
            }
            monitor.id = monitor.name;
            monitor.bounds = {list[i].x, list[i].y, list[i].width, list[i].height};
            monitor.work_area = monitor.bounds;
            monitor.primary = list[i].primary != 0;
            if (list[i].mwidth > 0) {
                monitor.dpi = static_cast<std::int32_t>(
                    (static_cast<double>(list[i].width) * 25.4) / list[i].mwidth);
                monitor.scale_factor = static_cast<double>(monitor.dpi) / 96.0;
            }
            monitors.push_back(std::move(monitor));
        }
        ::XRRFreeMonitors(list);
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

class X11Cursor final : public CursorProvider {
public:
    Result<CursorState> query() override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        Window root_return = 0;
        Window child_return = 0;
        int root_x = 0;
        int root_y = 0;
        int win_x = 0;
        int win_y = 0;
        unsigned int mask = 0;
        if (::XQueryPointer(x11.get(), x11.root(), &root_return, &child_return, &root_x, &root_y,
                            &win_x, &win_y, &mask) == 0) {
            return core::err::internal("XQueryPointer failed", "hal.linux");
        }
        CursorState cursor;
        cursor.x = root_x;
        cursor.y = root_y;
        return cursor;
    }

    Status warp(std::int32_t x, std::int32_t y) override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        ::XWarpPointer(x11.get(), None, x11.root(), 0, 0, 0, 0, x, y);
        ::XFlush(x11.get());
        return Status::success();
    }
};

class X11Workspaces final : public WorkspaceProvider {
public:
    Result<std::vector<Workspace>> enumerate() override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        const long count = x11.cardinal(x11.root(), "_NET_NUMBER_OF_DESKTOPS", 1);
        const long active = x11.cardinal(x11.root(), "_NET_CURRENT_DESKTOP", 0);
        const std::string names = x11.text_property(x11.root(), "_NET_DESKTOP_NAMES");

        std::vector<std::string> labels;
        std::string current;
        for (const char character : names) {
            if (character == '\0') {
                labels.push_back(current);
                current.clear();
            } else {
                current.push_back(character);
            }
        }
        std::vector<Workspace> workspaces;
        for (long i = 0; i < count; ++i) {
            Workspace workspace;
            workspace.id = "ws-" + std::to_string(i);
            workspace.name = static_cast<std::size_t>(i) < labels.size()
                                 ? labels[static_cast<std::size_t>(i)]
                                 : ("Desktop " + std::to_string(i + 1));
            workspace.index = static_cast<std::int32_t>(i);
            workspace.active = i == active;
            workspaces.push_back(std::move(workspace));
        }
        return workspaces;
    }

    Result<std::string> current() override {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        return "ws-" + std::to_string(x11.cardinal(x11.root(), "_NET_CURRENT_DESKTOP", 0));
    }

    Status activate(std::string_view workspace_id) override {
        return send_desktop_message(workspace_id, 0);
    }

    Status move_window(std::uint64_t handle, std::string_view workspace_id) override {
        return send_desktop_message(workspace_id, handle);
    }

private:
    static Status send_desktop_message(std::string_view workspace_id, std::uint64_t handle) {
        X11Display& x11 = display();
        if (!x11.valid()) {
            return core::err::unsupported("no X11 display", "hal.linux");
        }
        const std::size_t dash = workspace_id.rfind('-');
        const long index = std::atol(
            std::string(dash == std::string_view::npos ? workspace_id
                                                       : workspace_id.substr(dash + 1))
                .c_str());
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.format = 32;
        event.xclient.data.l[0] = index;
        event.xclient.data.l[1] = CurrentTime;
        if (handle == 0) {
            event.xclient.window = x11.root();
            event.xclient.message_type = x11.atom("_NET_CURRENT_DESKTOP");
        } else {
            event.xclient.window = static_cast<Window>(handle);
            event.xclient.message_type = x11.atom("_NET_WM_DESKTOP");
        }
        ::XSendEvent(x11.get(), x11.root(), False,
                     SubstructureRedirectMask | SubstructureNotifyMask, &event);
        ::XFlush(x11.get());
        return Status::success();
    }
};

#endif  // CONTEXTSNAP_HAS_X11

// --- Providers that work on both X11 and Wayland ------------------------------

class LinuxProcesses final : public ProcessProvider {
public:
    Result<ProcessInfo> info(std::uint64_t pid) override {
        std::error_code ec;
        if (!fs::exists(fs::path("/proc") / std::to_string(pid), ec)) {
            return core::err::not_found("no process " + std::to_string(pid), "hal.linux");
        }
        return read_process(pid);
    }

    Result<std::uint64_t> launch(const ProcessInfo& process) override {
        if (process.executable_path.empty()) {
            return core::err::invalid("process has no executable path", "hal.linux");
        }
        std::vector<std::string> argv = process.command_line;
        if (argv.empty()) {
            argv.push_back(process.executable_path);
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (std::string& argument : argv) {
            raw.push_back(argument.data());
        }
        raw.push_back(nullptr);

        const pid_t child = ::fork();
        if (child < 0) {
            return core::err::io("fork failed", "hal.linux");
        }
        if (child == 0) {
            // Detach from the daemon so restored apps survive a daemon restart.
            ::setsid();
            if (!process.working_directory.empty()) {
                if (::chdir(process.working_directory.c_str()) != 0) {
                    // Non-fatal: start in the daemon's directory instead.
                }
            }
            ::execv(process.executable_path.c_str(), raw.data());
            ::_exit(127);
        }
        return static_cast<std::uint64_t>(child);
    }

    bool is_running(std::uint64_t pid) override {
        return ::kill(static_cast<pid_t>(pid), 0) == 0;
    }

    Result<std::vector<std::uint64_t>> find_by_app_id(std::string_view app_id) override {
        std::vector<std::uint64_t> pids;
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator("/proc", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
                continue;
            }
            const std::uint64_t pid = std::strtoull(name.c_str(), nullptr, 10);
            const fs::path executable = fs::read_symlink(entry.path() / "exe", ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (core::matching::normalize_app_id(executable.string()) == app_id) {
                pids.push_back(pid);
            }
        }
        return pids;
    }
};

/// Provider set used when no X11 display is reachable (pure Wayland session).
class UnavailableWindows final : public WindowProvider {
public:
    Result<std::vector<WindowInfo>> enumerate(const CaptureOptions&) override {
        return core::err::unsupported(
            "this Wayland compositor exposes no window list; install the portal helper or run "
            "an X11/XWayland session",
            "hal.wayland");
    }
    Result<WindowInfo> get(std::uint64_t) override { return unsupported(); }
    Status set_frame(std::uint64_t, const Rect&) override { return unsupported(); }
    Status set_state(std::uint64_t, WindowState) override { return unsupported(); }
    Status raise(std::uint64_t) override { return unsupported(); }
    Status focus(std::uint64_t) override { return unsupported(); }
    Status set_always_on_top(std::uint64_t, bool) override { return unsupported(); }
    Status close(std::uint64_t) override { return unsupported(); }
    Result<WindowInfo> wait_for_window(std::uint64_t, std::string_view,
                                       std::chrono::milliseconds) override {
        return unsupported();
    }

private:
    static core::Error unsupported() {
        return core::Error{core::ErrorCode::Unsupported,
                           "window control is not available in this session", "hal.wayland", 0};
    }
};

class UnavailableDisplays final : public DisplayProvider {
public:
    Result<std::vector<MonitorInfo>> enumerate() override {
        return core::err::unsupported("no display information available", "hal.wayland");
    }
    Result<Rect> virtual_bounds() override {
        return core::err::unsupported("no display information available", "hal.wayland");
    }
};

class UnavailableCursor final : public CursorProvider {
public:
    Result<CursorState> query() override {
        return core::err::unsupported("Wayland does not expose the global pointer position",
                                      "hal.wayland");
    }
    Status warp(std::int32_t, std::int32_t) override {
        return core::err::unsupported("Wayland does not allow pointer warping", "hal.wayland");
    }
};

class UnavailableWorkspaces final : public WorkspaceProvider {
public:
    Result<std::vector<Workspace>> enumerate() override {
        return core::err::unsupported("no workspace protocol available", "hal.wayland");
    }
    Result<std::string> current() override {
        return core::err::unsupported("no workspace protocol available", "hal.wayland");
    }
    Status activate(std::string_view) override {
        return core::err::unsupported("no workspace protocol available", "hal.wayland");
    }
    Status move_window(std::uint64_t, std::string_view) override {
        return core::err::unsupported("no workspace protocol available", "hal.wayland");
    }
};

class LinuxPlatform final : public Platform {
public:
    LinuxPlatform() {
#if CONTEXTSNAP_HAS_X11
        x11_available_ = display().valid();
#endif
        wayland_ = wayland_session();
        build_capabilities();
    }

    WindowProvider& windows() override {
#if CONTEXTSNAP_HAS_X11
        if (x11_available_) return x11_windows_;
#endif
        return unavailable_windows_;
    }

    DisplayProvider& displays() override {
#if CONTEXTSNAP_HAS_X11
        if (x11_available_) return x11_displays_;
#endif
        return unavailable_displays_;
    }

    CursorProvider& cursor() override {
#if CONTEXTSNAP_HAS_X11
        if (x11_available_) return x11_cursor_;
#endif
        return unavailable_cursor_;
    }

    ProcessProvider& processes() override { return processes_; }

    WorkspaceProvider& workspaces() override {
#if CONTEXTSNAP_HAS_X11
        if (x11_available_) return x11_workspaces_;
#endif
        return unavailable_workspaces_;
    }

    [[nodiscard]] const Capabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] core::PlatformKind kind() const noexcept override {
        return core::PlatformKind::Linux;
    }

    [[nodiscard]] core::SessionType session_type() const noexcept override {
        if (x11_available_ && wayland_) return core::SessionType::X11;
        if (wayland_) return core::SessionType::Wayland;
        return x11_available_ ? core::SessionType::X11 : core::SessionType::Unknown;
    }

    [[nodiscard]] std::string name() const override {
        std::string label = "Linux (";
        label += std::string(core::to_string(session_type()));
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        if (desktop != nullptr && *desktop != '\0') {
            label += std::string(", ") + desktop;
        }
        return label + ")";
    }

    [[nodiscard]] std::string os_version() const override {
        const std::string release = read_file("/etc/os-release");
        const std::size_t start = release.find("PRETTY_NAME=\"");
        if (start == std::string::npos) {
            return "Linux";
        }
        const std::size_t begin = start + 13;
        const std::size_t end = release.find('"', begin);
        return release.substr(begin, end - begin);
    }

    [[nodiscard]] std::vector<std::string> missing_permissions() override {
        std::vector<std::string> missing;
        if (!x11_available_ && wayland_) {
            missing.emplace_back(
                "Wayland: window enumeration requires a compositor that implements "
                "ext-foreign-toplevel-list-v1 or wlr-foreign-toplevel-management");
        }
        if (!x11_available_ && !wayland_) {
            missing.emplace_back("no graphical session detected (is DISPLAY set?)");
        }
        return missing;
    }

    [[nodiscard]] Status request_permissions() override {
        // Neither X11 nor Wayland has a runtime permission prompt: the
        // compositor either exposes the protocols or it does not.
        return Status::success();
    }

private:
    void build_capabilities() {
        Capabilities& c = capabilities_;
        c.enumerate_windows = x11_available_;
        c.window_geometry = x11_available_;
        c.set_window_geometry = x11_available_;
        c.set_window_state = x11_available_;
        c.z_order = x11_available_;
        c.focus_control = x11_available_;
        c.virtual_desktops = x11_available_;
        c.move_between_desktops = x11_available_;
        c.cursor_query = x11_available_;
        c.cursor_warp = x11_available_;
        c.process_command_line = true;       // /proc/<pid>/cmdline
        c.process_working_directory = true;  // /proc/<pid>/cwd
        c.launch_processes = true;
        c.window_thumbnails = false;
        if (!x11_available_ && wayland_) {
            c.limitations.emplace_back(
                "Wayland session: window geometry, stacking order and pointer warping are not "
                "available to unprivileged clients");
            c.limitations.emplace_back(
                "Tabs and running applications are still captured; window placement is skipped");
        }
#if !CONTEXTSNAP_HAS_X11
        c.limitations.emplace_back("built without X11 support (CONTEXTSNAP_LINUX_X11=OFF)");
#endif
    }

    Capabilities capabilities_;
    bool x11_available_{false};
    bool wayland_{false};

#if CONTEXTSNAP_HAS_X11
    X11Windows x11_windows_;
    X11Displays x11_displays_;
    X11Cursor x11_cursor_;
    X11Workspaces x11_workspaces_;
#endif
    UnavailableWindows unavailable_windows_;
    UnavailableDisplays unavailable_displays_;
    UnavailableCursor unavailable_cursor_;
    UnavailableWorkspaces unavailable_workspaces_;
    LinuxProcesses processes_;
};

}  // namespace

Result<std::shared_ptr<Platform>> create_linux_platform() {
    auto platform = std::make_shared<LinuxPlatform>();
    return std::static_pointer_cast<Platform>(platform);
}

}  // namespace contextsnap::hal

#endif  // __linux__

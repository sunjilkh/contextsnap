#include <contextsnap/hal/null_platform.hpp>

#include <contextsnap/core/geometry.hpp>
#include <contextsnap/core/ulid.hpp>

#include <algorithm>
#include <utility>

namespace contextsnap::hal {
namespace {

Status mutation_blocked() {
    return core::err::unsupported("the null platform is read-only in this configuration",
                                  "hal.null");
}

class NullWindows final : public WindowProvider {
public:
    explicit NullWindows(NullPlatformState& state) : state_(state) {}

    Result<std::vector<WindowInfo>> enumerate(const CaptureOptions& options) override {
        std::vector<WindowInfo> windows;
        for (const WindowInfo& window : state_.windows) {
            if (!options.include_minimized && window.state == core::WindowState::Minimized) {
                continue;
            }
            windows.push_back(window);
        }
        return windows;
    }

    Result<WindowInfo> get(std::uint64_t native_handle) override {
        if (WindowInfo* window = find(native_handle); window != nullptr) {
            return *window;
        }
        return core::err::not_found("no such window handle", "hal.null");
    }

    Status set_frame(std::uint64_t native_handle, const Rect& frame) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        WindowInfo* window = find(native_handle);
        if (window == nullptr) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        window->frame = frame;
        window->restored_frame = frame;
        return Status::success();
    }

    Status set_state(std::uint64_t native_handle, WindowState state) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        WindowInfo* window = find(native_handle);
        if (window == nullptr) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        window->state = state;
        return Status::success();
    }

    Status raise(std::uint64_t native_handle) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        std::int32_t top = 0;
        for (const WindowInfo& window : state_.windows) {
            top = std::max(top, window.z_order);
        }
        WindowInfo* window = find(native_handle);
        if (window == nullptr) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        window->z_order = top + 1;
        return Status::success();
    }

    Status focus(std::uint64_t native_handle) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        WindowInfo* target = find(native_handle);
        if (target == nullptr) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        for (WindowInfo& window : state_.windows) {
            window.focused = false;
        }
        target->focused = true;
        state_.cursor.focused_window_id = target->id;
        return Status::success();
    }

    Status set_always_on_top(std::uint64_t native_handle, bool on_top) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        WindowInfo* window = find(native_handle);
        if (window == nullptr) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        window->always_on_top = on_top;
        return Status::success();
    }

    Status close(std::uint64_t native_handle) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        const auto removed = std::remove_if(
            state_.windows.begin(), state_.windows.end(),
            [&](const WindowInfo& window) { return window.native_handle == native_handle; });
        if (removed == state_.windows.end()) {
            return core::err::not_found("no such window handle", "hal.null");
        }
        state_.windows.erase(removed, state_.windows.end());
        return Status::success();
    }

    Result<WindowInfo> wait_for_window(std::uint64_t pid, std::string_view title_hint,
                                       std::chrono::milliseconds) override {
        for (const WindowInfo& window : state_.windows) {
            const bool pid_matches = pid == 0 || window.process.pid == pid;
            const bool title_matches =
                title_hint.empty() || window.title.find(title_hint) != std::string::npos;
            if (pid_matches && title_matches) {
                return window;
            }
        }
        // The null platform never spawns real windows, so this is a timeout by
        // definition rather than an error in the caller.
        return core::Error{core::ErrorCode::Timeout, "no window appeared for the launched process",
                          "hal.null", 0};
    }

private:
    WindowInfo* find(std::uint64_t native_handle) {
        for (WindowInfo& window : state_.windows) {
            if (window.native_handle == native_handle) {
                return &window;
            }
        }
        return nullptr;
    }

    NullPlatformState& state_;
};

class NullDisplays final : public DisplayProvider {
public:
    explicit NullDisplays(NullPlatformState& state) : state_(state) {}

    Result<std::vector<MonitorInfo>> enumerate() override { return state_.monitors; }

    Result<Rect> virtual_bounds() override { return core::geometry::virtual_bounds(state_.monitors); }

private:
    NullPlatformState& state_;
};

class NullCursor final : public CursorProvider {
public:
    explicit NullCursor(NullPlatformState& state) : state_(state) {}

    Result<CursorState> query() override { return state_.cursor; }

    Status warp(std::int32_t x, std::int32_t y) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        state_.cursor.x = x;
        state_.cursor.y = y;
        return Status::success();
    }

private:
    NullPlatformState& state_;
};

class NullProcesses final : public ProcessProvider {
public:
    explicit NullProcesses(NullPlatformState& state) : state_(state) {}

    Result<ProcessInfo> info(std::uint64_t pid) override {
        for (const WindowInfo& window : state_.windows) {
            if (window.process.pid == pid) {
                return window.process;
            }
        }
        return core::err::not_found("no such pid", "hal.null");
    }

    Result<std::uint64_t> launch(const ProcessInfo& process) override {
        if (!state_.allow_mutation) {
            return core::err::unsupported("the null platform cannot launch processes", "hal.null");
        }
        // Record the launch as a synthetic pid so restore reports stay useful in
        // tests without touching the real process table.
        static std::uint64_t next_pid = 90000;
        ProcessInfo launched = process;
        launched.pid = ++next_pid;
        return launched.pid;
    }

    bool is_running(std::uint64_t pid) override {
        return std::any_of(state_.windows.begin(), state_.windows.end(),
                           [&](const WindowInfo& window) { return window.process.pid == pid; });
    }

    Result<std::vector<std::uint64_t>> find_by_app_id(std::string_view app_id) override {
        std::vector<std::uint64_t> pids;
        for (const WindowInfo& window : state_.windows) {
            if (window.process.app_id == app_id) {
                pids.push_back(window.process.pid);
            }
        }
        return pids;
    }

private:
    NullPlatformState& state_;
};

class NullWorkspaces final : public WorkspaceProvider {
public:
    explicit NullWorkspaces(NullPlatformState& state) : state_(state) {}

    Result<std::vector<Workspace>> enumerate() override { return state_.workspaces; }

    Result<std::string> current() override {
        for (const Workspace& workspace : state_.workspaces) {
            if (workspace.active) {
                return workspace.id;
            }
        }
        return core::err::not_found("no active workspace", "hal.null");
    }

    Status activate(std::string_view workspace_id) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        bool found = false;
        for (Workspace& workspace : state_.workspaces) {
            workspace.active = workspace.id == workspace_id;
            found = found || workspace.active;
        }
        return found ? Status::success()
                     : core::err::not_found("no such workspace", "hal.null");
    }

    Status move_window(std::uint64_t native_handle, std::string_view workspace_id) override {
        if (!state_.allow_mutation) {
            return mutation_blocked();
        }
        for (WindowInfo& window : state_.windows) {
            if (window.native_handle == native_handle) {
                window.workspace_id = std::string(workspace_id);
                return Status::success();
            }
        }
        return core::err::not_found("no such window handle", "hal.null");
    }

private:
    NullPlatformState& state_;
};

class NullPlatform final : public Platform {
public:
    explicit NullPlatform(NullPlatformState state)
        : state_(std::move(state)),
          windows_(state_),
          displays_(state_),
          cursor_(state_),
          processes_(state_),
          workspaces_(state_) {
        capabilities_.enumerate_windows = true;
        capabilities_.window_geometry = true;
        capabilities_.set_window_geometry = state_.allow_mutation;
        capabilities_.set_window_state = state_.allow_mutation;
        capabilities_.z_order = state_.allow_mutation;
        capabilities_.focus_control = state_.allow_mutation;
        capabilities_.virtual_desktops = !state_.workspaces.empty();
        capabilities_.move_between_desktops = state_.allow_mutation;
        capabilities_.cursor_query = true;
        capabilities_.cursor_warp = state_.allow_mutation;
        capabilities_.process_command_line = true;
        capabilities_.process_working_directory = true;
        capabilities_.launch_processes = state_.allow_mutation;
        capabilities_.window_thumbnails = false;
        capabilities_.limitations.emplace_back(
            "headless backend: state is in-memory and never touches a real desktop");
    }

    PlatformKind kind() const noexcept override { return PlatformKind::Unknown; }
    SessionType session_type() const noexcept override { return SessionType::Unknown; }
    std::string name() const override { return "null (headless)"; }
    std::string os_version() const override { return "n/a"; }
    const Capabilities& capabilities() const noexcept override { return capabilities_; }
    WindowProvider& windows() override { return windows_; }
    DisplayProvider& displays() override { return displays_; }
    CursorProvider& cursor() override { return cursor_; }
    ProcessProvider& processes() override { return processes_; }
    WorkspaceProvider& workspaces() override { return workspaces_; }
    std::vector<std::string> missing_permissions() override { return {}; }
    Status request_permissions() override { return Status::success(); }

private:
    NullPlatformState state_;
    Capabilities capabilities_;
    NullWindows windows_;
    NullDisplays displays_;
    NullCursor cursor_;
    NullProcesses processes_;
    NullWorkspaces workspaces_;
};

}  // namespace

NullPlatformState default_null_state() {
    NullPlatformState state;
    MonitorInfo monitor;
    monitor.id = "null-0";
    monitor.name = "Headless Display";
    monitor.bounds = Rect{0, 0, 1920, 1080};
    monitor.work_area = Rect{0, 0, 1920, 1040};
    monitor.primary = true;
    state.monitors.push_back(monitor);
    state.workspaces.push_back(WorkspaceProvider::Workspace{"ws-1", "Workspace 1", 0, true});
    state.cursor = CursorState{960, 540, "null-0", std::nullopt};
    return state;
}

NullPlatformState sample_null_state() {
    NullPlatformState state = default_null_state();

    MonitorInfo secondary;
    secondary.id = "null-1";
    secondary.name = "Headless HiDPI";
    secondary.bounds = Rect{1920, 0, 2560, 1440};
    secondary.work_area = secondary.bounds;
    secondary.dpi = 192;
    secondary.scale_factor = 2.0;
    state.monitors.push_back(secondary);

    WindowInfo editor;
    editor.id = core::generate_ulid();
    editor.native_handle = 1001;
    editor.title = "contextsnap - main.cpp";
    editor.window_class = "Code";
    editor.frame = Rect{40, 60, 1200, 900};
    editor.client_area = editor.frame;
    editor.restored_frame = editor.frame;
    editor.monitor_id = "null-0";
    editor.workspace_id = "ws-1";
    editor.z_order = 1;
    editor.focused = true;
    editor.process.pid = 4242;
    editor.process.executable_path = "/usr/bin/code";
    editor.process.command_line = {"/usr/bin/code", "/home/dev/contextsnap"};
    editor.process.working_directory = "/home/dev/contextsnap";
    editor.process.app_id = "code";
    editor.process.single_instance = true;
    state.windows.push_back(editor);

    WindowInfo browser;
    browser.id = core::generate_ulid();
    browser.native_handle = 1002;
    browser.title = "ContextSnap design - Figma";
    browser.window_class = "Chromium";
    browser.frame = Rect{1920, 0, 2560, 1440};
    browser.client_area = browser.frame;
    browser.restored_frame = browser.frame;
    browser.monitor_id = "null-1";
    browser.workspace_id = "ws-1";
    browser.z_order = 2;
    browser.browser = core::BrowserKind::Chromium;
    browser.browser_profile = "Default";
    browser.process.pid = 4243;
    browser.process.executable_path = "/usr/bin/chromium";
    browser.process.command_line = {"/usr/bin/chromium"};
    browser.process.app_id = "chromium";

    core::TabInfo tab;
    tab.id = "tab-1";
    tab.index = 0;
    tab.url = "https://www.figma.com/file/abc/ContextSnap";
    tab.title = "ContextSnap design - Figma";
    tab.active = true;
    browser.tabs.push_back(tab);

    core::TabInfo docs;
    docs.id = "tab-2";
    docs.index = 1;
    docs.url = "https://wayland.freedesktop.org/docs/html/";
    docs.title = "Wayland Protocol Documentation";
    browser.tabs.push_back(docs);

    state.windows.push_back(browser);
    state.cursor.focused_window_id = editor.id;
    return state;
}

std::shared_ptr<Platform> create_null_platform(NullPlatformState state) {
    return std::make_shared<NullPlatform>(std::move(state));
}

}  // namespace contextsnap::hal

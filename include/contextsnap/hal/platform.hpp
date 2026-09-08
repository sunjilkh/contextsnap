// The Hardware/OS Abstraction Layer.
//
// Every OS-specific line of code in ContextSnap lives behind these five
// interfaces. Backends live in src/hal/{windows,macos,linux}. A backend must
// never throw across this boundary and must report unimplemented capabilities
// through Capabilities rather than failing at call time — the restore engine
// uses the capability set to build a plan the platform can actually execute.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace contextsnap::hal {

using core::CaptureOptions;
using core::CursorState;
using core::MonitorInfo;
using core::PlatformKind;
using core::ProcessInfo;
using core::Rect;
using core::Result;
using core::SessionType;
using core::Status;
using core::WindowInfo;
using core::WindowState;

/// What this backend can actually do on the running system. Populated once at
/// startup (may consult the compositor, TCC database, or registry).
struct Capabilities {
    bool enumerate_windows{false};
    bool window_geometry{false};       ///< Reading exact frame rects.
    bool set_window_geometry{false};   ///< Moving/resizing other apps' windows.
    bool set_window_state{false};      ///< Minimize/maximize/fullscreen.
    bool z_order{false};
    bool focus_control{false};
    bool virtual_desktops{false};
    bool move_between_desktops{false};
    bool cursor_query{false};
    bool cursor_warp{false};
    bool process_command_line{false};  ///< argv of other processes.
    bool process_working_directory{false};
    bool launch_processes{false};
    bool window_thumbnails{false};

    /// Human-readable reasons for the disabled bits, surfaced by `doctor`.
    std::vector<std::string> limitations;
};

/// Enumerates and manipulates top-level windows.
class WindowProvider {
public:
    virtual ~WindowProvider() = default;

    /// Windows in z-order (index 0 == topmost), excluding tool/utility windows.
    [[nodiscard]] virtual Result<std::vector<WindowInfo>> enumerate(
        const CaptureOptions& options) = 0;

    /// Re-reads one window by native handle; NotFound once it has closed.
    [[nodiscard]] virtual Result<WindowInfo> get(std::uint64_t native_handle) = 0;

    [[nodiscard]] virtual Status set_frame(std::uint64_t native_handle, const Rect& frame) = 0;
    [[nodiscard]] virtual Status set_state(std::uint64_t native_handle, WindowState state) = 0;
    [[nodiscard]] virtual Status raise(std::uint64_t native_handle) = 0;
    [[nodiscard]] virtual Status focus(std::uint64_t native_handle) = 0;
    [[nodiscard]] virtual Status set_always_on_top(std::uint64_t native_handle, bool on_top) = 0;
    [[nodiscard]] virtual Status close(std::uint64_t native_handle) = 0;

    /// Blocks until a window owned by `pid` (optionally matching `title_hint`)
    /// appears, or until the timeout elapses.
    [[nodiscard]] virtual Result<WindowInfo> wait_for_window(
        std::uint64_t pid,
        std::string_view title_hint,
        std::chrono::milliseconds timeout) = 0;
};

/// Monitors, DPI and the virtual desktop rectangle.
class DisplayProvider {
public:
    virtual ~DisplayProvider() = default;

    [[nodiscard]] virtual Result<std::vector<MonitorInfo>> enumerate() = 0;
    [[nodiscard]] virtual Result<Rect> virtual_bounds() = 0;
};

class CursorProvider {
public:
    virtual ~CursorProvider() = default;

    [[nodiscard]] virtual Result<CursorState> query() = 0;
    [[nodiscard]] virtual Status warp(std::int32_t x, std::int32_t y) = 0;
};

/// Process inspection and launching.
class ProcessProvider {
public:
    virtual ~ProcessProvider() = default;

    [[nodiscard]] virtual Result<ProcessInfo> info(std::uint64_t pid) = 0;

    /// Launches `process` detached from the daemon; returns the new pid.
    /// Implementations must not inherit the daemon's environment wholesale —
    /// see docs/privacy-security.md#process-launching.
    [[nodiscard]] virtual Result<std::uint64_t> launch(const ProcessInfo& process) = 0;

    [[nodiscard]] virtual bool is_running(std::uint64_t pid) = 0;

    /// Pids of every live process whose app id normalizes to `app_id`.
    [[nodiscard]] virtual Result<std::vector<std::uint64_t>> find_by_app_id(
        std::string_view app_id) = 0;
};

/// Virtual desktops / Spaces / workspaces.
class WorkspaceProvider {
public:
    struct Workspace {
        std::string id;
        std::string name;
        std::int32_t index{0};
        bool active{false};
    };

    virtual ~WorkspaceProvider() = default;

    [[nodiscard]] virtual Result<std::vector<Workspace>> enumerate() = 0;
    [[nodiscard]] virtual Result<std::string> current() = 0;
    [[nodiscard]] virtual Status activate(std::string_view workspace_id) = 0;
    [[nodiscard]] virtual Status move_window(std::uint64_t native_handle,
                                             std::string_view workspace_id) = 0;
};

/// Aggregate handed to the rest of the system.
class Platform {
public:
    virtual ~Platform() = default;

    [[nodiscard]] virtual PlatformKind kind() const noexcept = 0;
    [[nodiscard]] virtual SessionType session_type() const noexcept = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string os_version() const = 0;
    [[nodiscard]] virtual const Capabilities& capabilities() const noexcept = 0;

    [[nodiscard]] virtual WindowProvider& windows() = 0;
    [[nodiscard]] virtual DisplayProvider& displays() = 0;
    [[nodiscard]] virtual CursorProvider& cursor() = 0;
    [[nodiscard]] virtual ProcessProvider& processes() = 0;
    [[nodiscard]] virtual WorkspaceProvider& workspaces() = 0;

    /// Permissions the backend needs but does not currently hold, e.g.
    /// {"macos.accessibility"} or {"wayland.portal.remote-desktop"}.
    [[nodiscard]] virtual std::vector<std::string> missing_permissions() = 0;

    /// Triggers the platform's permission prompt where one exists.
    [[nodiscard]] virtual Status request_permissions() = 0;

    /// Builds the backend for the running system. Fails only when no windowing
    /// session is available (headless, SSH without DISPLAY/WAYLAND_DISPLAY).
    [[nodiscard]] static Result<std::shared_ptr<Platform>> create();
};

}  // namespace contextsnap::hal

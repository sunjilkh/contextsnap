// The snapshot domain model. This header is the single source of truth for what
// a "context state" is; SQL migrations (sql/), the FlatBuffers schema
// (schemas/snapshot.fbs), the JSON export and the IPC contract are all derived
// from these structures and must be updated together. See docs/data-model.md.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::core {

using Clock = std::chrono::system_clock;
using Timestamp = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;

/// Rectangle in *virtual desktop* coordinates: the union of all monitors, with
/// the primary monitor's top-left at (0, 0). Negative coordinates are normal on
/// multi-monitor setups where a display sits left of or above the primary one.
struct Rect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};

    [[nodiscard]] constexpr std::int32_t left() const noexcept { return x; }

    [[nodiscard]] constexpr std::int32_t top() const noexcept { return y; }

    [[nodiscard]] constexpr std::int32_t right() const noexcept { return x + width; }

    [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + height; }

    [[nodiscard]] constexpr std::int64_t area() const noexcept {
        return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool contains(std::int32_t px, std::int32_t py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    friend constexpr bool operator==(const Rect& a, const Rect& b) noexcept {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

enum class WindowState : std::uint8_t {
    Normal = 0,
    Minimized,
    Maximized,
    Fullscreen,
    Hidden,
};

enum class PlatformKind : std::uint8_t { Unknown = 0, Windows, MacOS, Linux };

/// Windowing session flavour — decisive for which restore operations are legal.
enum class SessionType : std::uint8_t { Unknown = 0, Win32, Quartz, X11, Wayland };

enum class BrowserKind : std::uint8_t {
    None = 0,
    Chrome,
    Chromium,
    Edge,
    Brave,
    Vivaldi,
    Opera,
    Firefox,
    LibreWolf,
    Safari,
    Other,
};

[[nodiscard]] std::string_view to_string(WindowState state) noexcept;
[[nodiscard]] std::string_view to_string(PlatformKind kind) noexcept;
[[nodiscard]] std::string_view to_string(SessionType type) noexcept;
[[nodiscard]] std::string_view to_string(BrowserKind kind) noexcept;
[[nodiscard]] WindowState window_state_from_string(std::string_view text) noexcept;
[[nodiscard]] BrowserKind browser_kind_from_string(std::string_view text) noexcept;

/// A physical (or virtual) display.
struct MonitorInfo {
    std::string id;             ///< Stable across reboots: hash(edid|name|index).
    std::string name;           ///< "DELL U2720Q", "Built-in Retina Display".
    Rect bounds;                ///< Full monitor rect in virtual coordinates.
    Rect work_area;             ///< bounds minus taskbars/docks/panels.
    std::uint32_t dpi{96};      ///< Effective DPI (96 = 100% scaling).
    double scale_factor{1.0};   ///< Backing scale (macOS 2.0 on Retina).
    std::uint32_t refresh_hz{0};
    std::string orientation{"landscape"};  ///< landscape | portrait | landscape_flipped | ...
    bool primary{false};
    std::optional<std::string> edid_hash;
};

/// Enough information to relaunch an application deterministically.
struct ProcessInfo {
    std::uint64_t pid{0};
    std::string executable_path;
    std::vector<std::string> command_line;  ///< argv[1..], argv[0] is executable_path.
    std::string working_directory;
    std::string app_id;  ///< Bundle id / AppUserModelID / desktop file id / exe stem.
    std::string user;
    bool single_instance{false};  ///< True when relaunching focuses the existing process.
    bool elevated{false};
};

/// One browser tab inside a browser window.
struct TabInfo {
    std::string id;             ///< Snapshot-local identifier.
    std::int32_t index{0};      ///< Position within the window.
    std::string url;            ///< Already sanitized before persistence.
    std::string title;
    std::optional<std::string> favicon_path;  ///< Cache-relative path, never a data URI.
    std::optional<std::string> group_id;      ///< Chromium tab group / Firefox container.
    std::optional<std::string> group_title;
    std::optional<std::string> cookie_store_id;  ///< Firefox contextual identity.
    std::optional<std::int32_t> opener_index;
    std::int32_t scroll_y{0};
    bool pinned{false};
    bool active{false};
    bool audible{false};
    bool muted{false};
    bool discarded{false};  ///< Restored lazily; see docs/browser-integration.md.
    bool incognito{false};
    bool sanitized{false};  ///< A privacy rule rewrote this URL.
};

/// A top-level window. Browser windows additionally carry `tabs`.
struct WindowInfo {
    std::string id;                   ///< Snapshot-local identifier (stable within a snapshot).
    std::uint64_t native_handle{0};   ///< HWND / CGWindowID / xcb_window_t. Capture-time only.
    std::string title;
    std::string window_class;         ///< WM_CLASS / bundle id / window class name.
    Rect frame;                       ///< Outer frame, DWM extended bounds on Windows.
    Rect client_area;
    Rect restored_frame;              ///< Geometry to use when un-maximizing.
    WindowState state{WindowState::Normal};
    std::int32_t z_order{0};          ///< 0 = topmost.
    std::string monitor_id;
    std::optional<std::string> workspace_id;  ///< Virtual desktop / Space / _NET_WM_DESKTOP.
    std::optional<std::string> workspace_name;
    double opacity{1.0};
    bool focused{false};
    bool always_on_top{false};
    bool minimizable{true};
    bool resizable{true};
    bool captured_geometry_reliable{true};  ///< False on Wayland: geometry is compositor-private.
    ProcessInfo process;
    BrowserKind browser{BrowserKind::None};
    std::optional<std::string> browser_profile;
    std::vector<TabInfo> tabs;
};

struct CursorState {
    std::int32_t x{0};
    std::int32_t y{0};
    std::string monitor_id;
    std::optional<std::string> focused_window_id;
};

struct SnapshotMetadata {
    std::string id;  ///< ULID: lexicographically sortable, 26 chars.
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    Timestamp created_at{};
    Timestamp updated_at{};
    PlatformKind platform{PlatformKind::Unknown};
    SessionType session_type{SessionType::Unknown};
    std::string host_name;
    std::string os_version;
    std::string app_version;
    std::uint32_t schema_version{1};
    std::uint32_t capture_duration_ms{0};
    bool favorite{false};
    bool automatic{false};  ///< Produced by the scheduler rather than by a user action.
};

struct Snapshot {
    SnapshotMetadata metadata;
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo> windows;
    CursorState cursor;
    std::optional<std::string> active_workspace_id;

    [[nodiscard]] std::size_t tab_count() const noexcept {
        std::size_t count = 0;
        for (const auto& window : windows) {
            count += window.tabs.size();
        }
        return count;
    }

    [[nodiscard]] const WindowInfo* find_window(std::string_view window_id) const noexcept {
        for (const auto& window : windows) {
            if (window.id == window_id) {
                return &window;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const MonitorInfo* find_monitor(std::string_view monitor_id) const noexcept {
        for (const auto& monitor : monitors) {
            if (monitor.id == monitor_id) {
                return &monitor;
            }
        }
        return nullptr;
    }
};

/// Lightweight row used by list views (no windows/tabs payload).
struct SnapshotSummary {
    std::string id;
    std::string name;
    std::vector<std::string> tags;
    Timestamp created_at{};
    std::uint32_t window_count{0};
    std::uint32_t tab_count{0};
    std::uint32_t monitor_count{0};
    bool favorite{false};
    bool automatic{false};
};

struct CaptureOptions {
    bool include_tabs{true};
    bool include_minimized{true};
    bool include_cursor{true};
    bool include_incognito{false};
    bool include_all_workspaces{true};
    bool sanitize_urls{true};
    Milliseconds browser_timeout{700};  ///< Capture proceeds without tabs after this.
    std::vector<std::string> excluded_app_ids;
    std::vector<std::string> excluded_url_patterns;
    std::string name;
    std::vector<std::string> tags;
    bool automatic{false};
};

/// Selector used by partial restore: "app:code", "window:01H...", "tab:*.figma.com".
struct RestoreSelector {
    enum class Kind : std::uint8_t { All, App, Window, Tab, Monitor, Workspace };

    Kind kind{Kind::All};
    std::string pattern;
};

struct RestoreOptions {
    std::vector<RestoreSelector> selectors;  ///< Empty == restore everything.
    bool launch_missing_apps{true};
    bool restore_tabs{true};
    bool lazy_load_tabs{true};  ///< Create tabs discarded to avoid a memory spike.
    bool restore_cursor{true};
    bool restore_focus{true};
    bool restore_z_order{true};
    bool restore_workspaces{true};
    bool close_conflicting_windows{false};
    bool dry_run{false};
    Milliseconds app_launch_timeout{8000};
    Milliseconds window_settle_timeout{2500};
    std::uint32_t max_parallel_launches{4};
};

enum class RestoreStepKind : std::uint8_t {
    LaunchProcess,
    WaitForWindow,
    MoveWindow,
    SetWindowState,
    SetWorkspace,
    RaiseWindow,
    RestoreTabs,
    WarpCursor,
    FocusWindow,
};

struct RestoreStep {
    RestoreStepKind kind{RestoreStepKind::MoveWindow};
    std::string target_id;   ///< Window id, app id or tab id depending on `kind`.
    std::string description; ///< Human readable, shown by `--dry-run`.
    bool skipped{false};
    std::string skip_reason;
};

struct RestorePlan {
    std::string snapshot_id;
    std::vector<RestoreStep> steps;
    std::vector<std::string> warnings;
    std::uint32_t estimated_duration_ms{0};
};

struct RestoreReport {
    std::string snapshot_id;
    std::uint32_t windows_restored{0};
    std::uint32_t windows_failed{0};
    std::uint32_t apps_launched{0};
    std::uint32_t tabs_restored{0};
    std::uint32_t duration_ms{0};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool fully_successful() const noexcept {
        return windows_failed == 0 && errors.empty();
    }
};

}  // namespace contextsnap::core

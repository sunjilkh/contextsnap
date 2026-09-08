#include <contextsnap/storage/snapshot_repository.hpp>

#include <contextsnap/core/json.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/ulid.hpp>
#include <contextsnap/storage/serialization.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace contextsnap::storage {
namespace {

std::string tags_to_json(const std::vector<std::string>& tags) {
    core::json::Array array;
    for (const std::string& tag : tags) {
        array.emplace_back(core::json::Value(tag));
    }
    return core::json::Value(std::move(array)).dump();
}

std::vector<std::string> tags_from_json(std::string_view text) {
    std::vector<std::string> tags;
    auto parsed = core::json::parse(text);
    if (!parsed || !parsed.value().is_array()) {
        return tags;
    }
    for (const core::json::Value& item : parsed.value().as_array()) {
        tags.push_back(item.as_string());
    }
    return tags;
}

std::string join(const std::vector<std::string>& values, char separator) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out.push_back(separator);
        }
        out.append(value);
    }
    return out;
}

}  // namespace

SnapshotRepository::SnapshotRepository(Database database) : database_(std::move(database)) {}

Result<std::shared_ptr<SnapshotRepository>> SnapshotRepository::open(
    const DatabaseOptions& options) {
    auto database = Database::open(options);
    if (!database) {
        return database.error();
    }
    if (const Status migrated = database.value().migrate(); !migrated) {
        return migrated.error();
    }
    return std::make_shared<SnapshotRepository>(std::move(database.value()));
}

Result<std::string> SnapshotRepository::insert(const Snapshot& original) {
    // The repository owns id assignment so callers can insert a freshly
    // captured snapshot without pre-generating an id.
    Snapshot snapshot = original;
    if (snapshot.metadata.id.empty()) {
        snapshot.metadata.id = core::generate_ulid();
    }
    if (!core::is_valid_ulid(snapshot.metadata.id)) {
        return core::err::invalid("snapshot id must be a ULID", "storage.repository");
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(database_);

    auto statement = database_.prepare(
        "INSERT INTO snapshots (id, name, description, tags, created_at, updated_at, platform, "
        "session_type, host_name, os_version, app_version, schema_version, capture_duration_ms, "
        "favorite, automatic, active_workspace_id) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return statement.error();
    }
    const core::SnapshotMetadata& meta = snapshot.metadata;
    statement.value()
        .bind(1, meta.id)
        .bind(2, meta.name)
        .bind(3, meta.description)
        .bind(4, tags_to_json(meta.tags))
        .bind(5, to_unix_millis(meta.created_at))
        .bind(6, to_unix_millis(meta.updated_at))
        .bind(7, core::to_string(meta.platform))
        .bind(8, core::to_string(meta.session_type))
        .bind(9, meta.host_name)
        .bind(10, meta.os_version)
        .bind(11, meta.app_version)
        .bind(12, static_cast<std::int64_t>(meta.schema_version))
        .bind(13, static_cast<std::int64_t>(meta.capture_duration_ms))
        .bind(14, meta.favorite)
        .bind(15, meta.automatic)
        .bind(16, snapshot.active_workspace_id.value_or(""));
    if (const Status inserted = statement.value().execute(); !inserted) {
        return inserted.error();
    }
    if (const Status monitors = insert_monitors(snapshot); !monitors) {
        return monitors.error();
    }
    if (const Status windows = insert_windows(snapshot); !windows) {
        return windows.error();
    }
    if (const Status cursor = insert_cursor(snapshot); !cursor) {
        return cursor.error();
    }
    if (const Status indexed = index_for_search(snapshot); !indexed) {
        return indexed.error();
    }
    if (const Status committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return snapshot.metadata.id;
}

Status SnapshotRepository::insert_monitors(const Snapshot& snapshot) {
    auto statement = database_.prepare(
        "INSERT INTO monitors (snapshot_id, monitor_id, name, x, y, width, height, work_x, "
        "work_y, work_width, work_height, dpi, scale_factor, refresh_hz, orientation, is_primary, "
        "edid_hash) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return statement.error();
    }
    for (const core::MonitorInfo& monitor : snapshot.monitors) {
        statement.value()
            .reset()
            .bind(1, snapshot.metadata.id)
            .bind(2, monitor.id)
            .bind(3, monitor.name)
            .bind(4, static_cast<std::int32_t>(monitor.bounds.x))
            .bind(5, static_cast<std::int32_t>(monitor.bounds.y))
            .bind(6, static_cast<std::int32_t>(monitor.bounds.width))
            .bind(7, static_cast<std::int32_t>(monitor.bounds.height))
            .bind(8, static_cast<std::int32_t>(monitor.work_area.x))
            .bind(9, static_cast<std::int32_t>(monitor.work_area.y))
            .bind(10, static_cast<std::int32_t>(monitor.work_area.width))
            .bind(11, static_cast<std::int32_t>(monitor.work_area.height))
            .bind(12, static_cast<std::int64_t>(monitor.dpi))
            .bind(13, monitor.scale_factor)
            .bind(14, static_cast<std::int64_t>(monitor.refresh_hz))
            .bind(15, monitor.orientation)
            .bind(16, monitor.primary)
            .bind_optional(17, monitor.edid_hash);
        if (const Status status = statement.value().execute(); !status) {
            return status;
        }
    }
    return Status::success();
}

Status SnapshotRepository::insert_windows(const Snapshot& snapshot) {
    auto statement = database_.prepare(
        "INSERT INTO windows (snapshot_id, window_id, native_handle, title, window_class, "
        "frame_x, frame_y, frame_width, frame_height, client_x, client_y, client_width, "
        "client_height, restored_x, restored_y, restored_width, restored_height, state, z_order, "
        "monitor_id, workspace_id, workspace_name, opacity, focused, always_on_top, minimizable, "
        "resizable, captured_geometry_reliable, pid, executable_path, command_line, "
        "working_directory, app_id, user_name, single_instance, elevated, browser, "
        "browser_profile) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
        "?,?,?,?,?,?,?)");
    if (!statement) {
        return statement.error();
    }
    for (const core::WindowInfo& window : snapshot.windows) {
        core::json::Array argv;
        for (const std::string& argument : window.process.command_line) {
            argv.emplace_back(core::json::Value(argument));
        }
        statement.value()
            .reset()
            .bind(1, snapshot.metadata.id)
            .bind(2, window.id)
            .bind(3, static_cast<std::int64_t>(window.native_handle))
            .bind(4, window.title)
            .bind(5, window.window_class)
            .bind(6, static_cast<std::int32_t>(window.frame.x))
            .bind(7, static_cast<std::int32_t>(window.frame.y))
            .bind(8, static_cast<std::int32_t>(window.frame.width))
            .bind(9, static_cast<std::int32_t>(window.frame.height))
            .bind(10, static_cast<std::int32_t>(window.client_area.x))
            .bind(11, static_cast<std::int32_t>(window.client_area.y))
            .bind(12, static_cast<std::int32_t>(window.client_area.width))
            .bind(13, static_cast<std::int32_t>(window.client_area.height))
            .bind(14, static_cast<std::int32_t>(window.restored_frame.x))
            .bind(15, static_cast<std::int32_t>(window.restored_frame.y))
            .bind(16, static_cast<std::int32_t>(window.restored_frame.width))
            .bind(17, static_cast<std::int32_t>(window.restored_frame.height))
            .bind(18, core::to_string(window.state))
            .bind(19, static_cast<std::int32_t>(window.z_order))
            .bind(20, window.monitor_id)
            .bind(21, window.workspace_id.value_or(""))
            .bind(22, window.workspace_name.value_or(""))
            .bind(23, window.opacity)
            .bind(24, window.focused)
            .bind(25, window.always_on_top)
            .bind(26, window.minimizable)
            .bind(27, window.resizable)
            .bind(28, window.captured_geometry_reliable)
            .bind(29, static_cast<std::int64_t>(window.process.pid))
            .bind(30, window.process.executable_path)
            .bind(31, core::json::Value(std::move(argv)).dump())
            .bind(32, window.process.working_directory)
            .bind(33, window.process.app_id)
            .bind(34, window.process.user)
            .bind(35, window.process.single_instance)
            .bind(36, window.process.elevated)
            .bind(37, core::to_string(window.browser))
            .bind(38, window.browser_profile.value_or(""));
        if (const Status status = statement.value().execute(); !status) {
            return status;
        }
        if (const Status tabs = insert_tabs(snapshot.metadata.id, window); !tabs) {
            return tabs;
        }
    }
    return Status::success();
}

Status SnapshotRepository::insert_tabs(const std::string& snapshot_id,
                                       const core::WindowInfo& window) {
    if (window.tabs.empty()) {
        return Status::success();
    }
    auto statement = database_.prepare(
        "INSERT INTO tabs (snapshot_id, window_id, tab_id, tab_index, url, title, favicon_path, "
        "group_id, group_title, cookie_store_id, opener_index, scroll_y, pinned, active, audible, "
        "muted, discarded, incognito, sanitized, last_accessed_at, restore_priority) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return statement.error();
    }
    for (const core::TabInfo& tab : window.tabs) {
        // Active and pinned tabs load eagerly; everything else can stay discarded.
        const std::int32_t priority = tab.active ? 100 : (tab.pinned ? 50 : 0);
        statement.value()
            .reset()
            .bind(1, snapshot_id)
            .bind(2, window.id)
            .bind(3, tab.id)
            .bind(4, static_cast<std::int32_t>(tab.index))
            .bind(5, tab.url)
            .bind(6, tab.title)
            .bind(7, tab.favicon_path.value_or(""))
            .bind(8, tab.group_id.value_or(""))
            .bind(9, tab.group_title.value_or(""))
            .bind(10, tab.cookie_store_id.value_or(""))
            .bind(11, static_cast<std::int32_t>(tab.opener_index.value_or(-1)))
            .bind(12, static_cast<std::int32_t>(tab.scroll_y))
            .bind(13, tab.pinned)
            .bind(14, tab.active)
            .bind(15, tab.audible)
            .bind(16, tab.muted)
            .bind(17, tab.discarded)
            .bind(18, tab.incognito)
            .bind(19, tab.sanitized)
            .bind(20, to_unix_millis(core::Clock::now()))
            .bind(21, priority);
        if (const Status status = statement.value().execute(); !status) {
            return status;
        }
    }
    return Status::success();
}

Status SnapshotRepository::insert_cursor(const Snapshot& snapshot) {
    auto statement = database_.prepare(
        "INSERT INTO cursor_states (snapshot_id, x, y, monitor_id, focused_window_id) "
        "VALUES (?,?,?,?,?)");
    if (!statement) {
        return statement.error();
    }
    statement.value()
        .bind(1, snapshot.metadata.id)
        .bind(2, static_cast<std::int32_t>(snapshot.cursor.x))
        .bind(3, static_cast<std::int32_t>(snapshot.cursor.y))
        .bind(4, snapshot.cursor.monitor_id)
        .bind(5, snapshot.cursor.focused_window_id.value_or(""));
    return statement.value().execute();
}

Status SnapshotRepository::index_for_search(const Snapshot& snapshot) {
    std::set<std::string> app_ids;
    std::vector<std::string> window_titles;
    std::vector<std::string> tab_titles;
    std::set<std::string> tab_hosts;
    for (const core::WindowInfo& window : snapshot.windows) {
        if (!window.process.app_id.empty()) {
            app_ids.insert(window.process.app_id);
        }
        if (!window.title.empty()) {
            window_titles.push_back(window.title);
        }
        for (const core::TabInfo& tab : window.tabs) {
            if (!tab.title.empty()) {
                tab_titles.push_back(tab.title);
            }
            const std::size_t scheme = tab.url.find("://");
            if (scheme != std::string::npos) {
                const std::size_t start = scheme + 3;
                const std::size_t end = tab.url.find('/', start);
                tab_hosts.insert(tab.url.substr(start, end == std::string::npos ? end : end - start));
            }
        }
    }
    auto statement = database_.prepare(
        "INSERT INTO snapshot_search (snapshot_id, name, tags, app_ids, window_titles, "
        "tab_titles, tab_hosts) VALUES (?,?,?,?,?,?,?)");
    if (!statement) {
        // An FTS5-less SQLite build should not make capture fail.
        core::log::warn("full-text index unavailable; search will fall back to LIKE",
                        {core::log::field("error", statement.error().to_string())});
        return Status::success();
    }
    statement.value()
        .bind(1, snapshot.metadata.id)
        .bind(2, snapshot.metadata.name)
        .bind(3, join(snapshot.metadata.tags, ' '))
        .bind(4, join(std::vector<std::string>(app_ids.begin(), app_ids.end()), ' '))
        .bind(5, join(window_titles, ' '))
        .bind(6, join(tab_titles, ' '))
        .bind(7, join(std::vector<std::string>(tab_hosts.begin(), tab_hosts.end()), ' '));
    if (const Status status = statement.value().execute(); !status) {
        core::log::warn("cannot index snapshot for search",
                        {core::log::field("error", status.error().to_string())});
    }
    return Status::success();
}

Status SnapshotRepository::update_metadata(const core::SnapshotMetadata& metadata) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare(
        "UPDATE snapshots SET name = ?, description = ?, tags = ?, favorite = ?, updated_at = ? "
        "WHERE id = ?");
    if (!statement) {
        return statement.error();
    }
    statement.value()
        .bind(1, metadata.name)
        .bind(2, metadata.description)
        .bind(3, tags_to_json(metadata.tags))
        .bind(4, metadata.favorite)
        .bind(5, to_unix_millis(core::Clock::now()))
        .bind(6, metadata.id);
    if (const Status status = statement.value().execute(); !status) {
        return status;
    }
    if (database_.changes() == 0) {
        return core::err::not_found("no snapshot with id " + metadata.id, "storage.repository");
    }
    // Keep the search index in sync with the new name/tags.
    auto refreshed = load(metadata.id);
    if (refreshed) {
        if (const Status indexed = index_for_search(refreshed.value()); !indexed) {
            core::log::warn("cannot refresh search index",
                            {core::log::field("error", indexed.error().to_string())});
        }
    }
    return Status::success();
}

Result<std::string> SnapshotRepository::resolve_id_prefix(const std::string& prefix) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare(
        "SELECT id FROM snapshots WHERE id LIKE ? ORDER BY created_at DESC LIMIT 2");
    if (!statement) {
        return statement.error();
    }
    std::string upper(prefix);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    statement.value().bind(1, upper + "%");

    std::vector<std::string> matches;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        matches.push_back(statement.value().column_text(0));
    }
    if (matches.empty()) {
        return core::err::not_found("no snapshot matches " + std::string(prefix),
                                    "storage.repository");
    }
    if (matches.size() > 1) {
        return core::err::invalid("id prefix is ambiguous: " + std::string(prefix),
                                  "storage.repository");
    }
    return matches.front();
}

}  // namespace contextsnap::storage

namespace contextsnap::storage {

SnapshotRepository::~SnapshotRepository() = default;

Result<Snapshot> SnapshotRepository::load(const std::string& snapshot_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare(
        "SELECT id, name, description, tags, created_at, updated_at, platform, session_type, "
        "host_name, os_version, app_version, schema_version, capture_duration_ms, favorite, "
        "automatic, active_workspace_id FROM snapshots WHERE id = ?");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, snapshot_id);
    auto stepped = statement.value().step();
    if (!stepped) {
        return stepped.error();
    }
    if (!stepped.value()) {
        return core::err::not_found("no snapshot with id " + snapshot_id, "storage.repository");
    }

    Snapshot snapshot;
    Statement& row = statement.value();
    snapshot.metadata.id = row.column_text(0);
    snapshot.metadata.name = row.column_text(1);
    snapshot.metadata.description = row.column_text(2);
    snapshot.metadata.tags = tags_from_json(row.column_text(3));
    snapshot.metadata.created_at = from_unix_millis(row.column_int(4));
    snapshot.metadata.updated_at = from_unix_millis(row.column_int(5));
    snapshot.metadata.host_name = row.column_text(8);
    snapshot.metadata.os_version = row.column_text(9);
    snapshot.metadata.app_version = row.column_text(10);
    snapshot.metadata.schema_version = static_cast<std::uint32_t>(row.column_int(11));
    snapshot.metadata.capture_duration_ms = static_cast<std::uint32_t>(row.column_int(12));
    snapshot.metadata.favorite = row.column_bool(13);
    snapshot.metadata.automatic = row.column_bool(14);
    if (const std::string workspace = row.column_text(15); !workspace.empty()) {
        snapshot.active_workspace_id = workspace;
    }
    // Platform/session strings round-trip through the same helpers as JSON.
    const std::string platform_text = row.column_text(6);
    for (const auto kind : {core::PlatformKind::Windows, core::PlatformKind::MacOS,
                            core::PlatformKind::Linux}) {
        if (core::to_string(kind) == platform_text) {
            snapshot.metadata.platform = kind;
        }
    }
    const std::string session_text = row.column_text(7);
    for (const auto type : {core::SessionType::Win32, core::SessionType::Quartz,
                            core::SessionType::X11, core::SessionType::Wayland}) {
        if (core::to_string(type) == session_text) {
            snapshot.metadata.session_type = type;
        }
    }

    auto monitors = load_monitors(snapshot_id);
    if (!monitors) {
        return monitors.error();
    }
    snapshot.monitors = std::move(monitors.value());

    auto windows = load_windows(snapshot_id);
    if (!windows) {
        return windows.error();
    }
    snapshot.windows = std::move(windows.value());
    if (const Status tabs = load_tabs(snapshot_id, snapshot.windows); !tabs) {
        return tabs.error();
    }
    auto cursor = load_cursor(snapshot_id);
    if (!cursor) {
        return cursor.error();
    }
    snapshot.cursor = cursor.value();
    return snapshot;
}

Result<std::vector<core::MonitorInfo>> SnapshotRepository::load_monitors(const std::string& id) {
    auto statement = database_.prepare(
        "SELECT monitor_id, name, x, y, width, height, work_x, work_y, work_width, work_height, "
        "dpi, scale_factor, refresh_hz, orientation, is_primary, edid_hash FROM monitors "
        "WHERE snapshot_id = ? ORDER BY is_primary DESC, monitor_id");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, id);
    std::vector<core::MonitorInfo> monitors;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        Statement& row = statement.value();
        core::MonitorInfo monitor;
        monitor.id = row.column_text(0);
        monitor.name = row.column_text(1);
        monitor.bounds = core::Rect{static_cast<std::int32_t>(row.column_int(2)),
                                    static_cast<std::int32_t>(row.column_int(3)),
                                    static_cast<std::int32_t>(row.column_int(4)),
                                    static_cast<std::int32_t>(row.column_int(5))};
        monitor.work_area = core::Rect{static_cast<std::int32_t>(row.column_int(6)),
                                       static_cast<std::int32_t>(row.column_int(7)),
                                       static_cast<std::int32_t>(row.column_int(8)),
                                       static_cast<std::int32_t>(row.column_int(9))};
        monitor.dpi = static_cast<std::uint32_t>(row.column_int(10));
        monitor.scale_factor = row.column_double(11);
        monitor.refresh_hz = static_cast<std::uint32_t>(row.column_int(12));
        monitor.orientation = row.column_text(13);
        monitor.primary = row.column_bool(14);
        monitor.edid_hash = row.column_text_optional(15);
        monitors.push_back(std::move(monitor));
    }
    return monitors;
}

Result<std::vector<core::WindowInfo>> SnapshotRepository::load_windows(const std::string& id) {
    auto statement = database_.prepare(
        "SELECT window_id, native_handle, title, window_class, frame_x, frame_y, frame_width, "
        "frame_height, client_x, client_y, client_width, client_height, restored_x, restored_y, "
        "restored_width, restored_height, state, z_order, monitor_id, workspace_id, "
        "workspace_name, opacity, focused, always_on_top, minimizable, resizable, "
        "captured_geometry_reliable, pid, executable_path, command_line, working_directory, "
        "app_id, user_name, single_instance, elevated, browser, browser_profile FROM windows "
        "WHERE snapshot_id = ? ORDER BY z_order");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, id);
    std::vector<core::WindowInfo> windows;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        Statement& row = statement.value();
        core::WindowInfo window;
        window.id = row.column_text(0);
        window.native_handle = static_cast<std::uint64_t>(row.column_int(1));
        window.title = row.column_text(2);
        window.window_class = row.column_text(3);
        window.frame = core::Rect{static_cast<std::int32_t>(row.column_int(4)),
                                  static_cast<std::int32_t>(row.column_int(5)),
                                  static_cast<std::int32_t>(row.column_int(6)),
                                  static_cast<std::int32_t>(row.column_int(7))};
        window.client_area = core::Rect{static_cast<std::int32_t>(row.column_int(8)),
                                        static_cast<std::int32_t>(row.column_int(9)),
                                        static_cast<std::int32_t>(row.column_int(10)),
                                        static_cast<std::int32_t>(row.column_int(11))};
        window.restored_frame = core::Rect{static_cast<std::int32_t>(row.column_int(12)),
                                           static_cast<std::int32_t>(row.column_int(13)),
                                           static_cast<std::int32_t>(row.column_int(14)),
                                           static_cast<std::int32_t>(row.column_int(15))};
        window.state = core::window_state_from_string(row.column_text(16));
        window.z_order = static_cast<std::int32_t>(row.column_int(17));
        window.monitor_id = row.column_text(18);
        window.workspace_id = row.column_text_optional(19);
        window.workspace_name = row.column_text_optional(20);
        window.opacity = row.column_double(21);
        window.focused = row.column_bool(22);
        window.always_on_top = row.column_bool(23);
        window.minimizable = row.column_bool(24);
        window.resizable = row.column_bool(25);
        window.captured_geometry_reliable = row.column_bool(26);
        window.process.pid = static_cast<std::uint64_t>(row.column_int(27));
        window.process.executable_path = row.column_text(28);
        if (auto argv = core::json::parse(row.column_text(29)); argv && argv.value().is_array()) {
            for (const core::json::Value& item : argv.value().as_array()) {
                window.process.command_line.push_back(item.as_string());
            }
        }
        window.process.working_directory = row.column_text(30);
        window.process.app_id = row.column_text(31);
        window.process.user = row.column_text(32);
        window.process.single_instance = row.column_bool(33);
        window.process.elevated = row.column_bool(34);
        window.browser = core::browser_kind_from_string(row.column_text(35));
        window.browser_profile = row.column_text_optional(36);
        windows.push_back(std::move(window));
    }
    return windows;
}

Status SnapshotRepository::load_tabs(const std::string& snapshot_id,
                                     std::vector<core::WindowInfo>& windows) {
    auto statement = database_.prepare(
        "SELECT window_id, tab_id, tab_index, url, title, favicon_path, group_id, group_title, "
        "cookie_store_id, opener_index, scroll_y, pinned, active, audible, muted, discarded, "
        "incognito, sanitized FROM tabs WHERE snapshot_id = ? ORDER BY window_id, tab_index");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, snapshot_id);
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        Statement& row = statement.value();
        const std::string window_id = row.column_text(0);
        const auto target = std::find_if(
            windows.begin(), windows.end(),
            [&](const core::WindowInfo& window) { return window.id == window_id; });
        if (target == windows.end()) {
            continue;  // Orphaned row; the composite FK normally prevents this.
        }
        core::TabInfo tab;
        tab.id = row.column_text(1);
        tab.index = static_cast<std::int32_t>(row.column_int(2));
        tab.url = row.column_text(3);
        tab.title = row.column_text(4);
        tab.favicon_path = row.column_text_optional(5);
        tab.group_id = row.column_text_optional(6);
        tab.group_title = row.column_text_optional(7);
        tab.cookie_store_id = row.column_text_optional(8);
        if (const auto opener = static_cast<std::int32_t>(row.column_int(9)); opener >= 0) {
            tab.opener_index = opener;
        }
        tab.scroll_y = static_cast<std::int32_t>(row.column_int(10));
        tab.pinned = row.column_bool(11);
        tab.active = row.column_bool(12);
        tab.audible = row.column_bool(13);
        tab.muted = row.column_bool(14);
        tab.discarded = row.column_bool(15);
        tab.incognito = row.column_bool(16);
        tab.sanitized = row.column_bool(17);
        target->tabs.push_back(std::move(tab));
    }
    return Status::success();
}

Result<core::CursorState> SnapshotRepository::load_cursor(const std::string& id) {
    auto statement = database_.prepare(
        "SELECT x, y, monitor_id, focused_window_id FROM cursor_states WHERE snapshot_id = ?");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, id);
    auto stepped = statement.value().step();
    if (!stepped) {
        return stepped.error();
    }
    core::CursorState cursor;
    if (!stepped.value()) {
        return cursor;  // Cursor capture is optional.
    }
    Statement& row = statement.value();
    cursor.x = static_cast<std::int32_t>(row.column_int(0));
    cursor.y = static_cast<std::int32_t>(row.column_int(1));
    cursor.monitor_id = row.column_text(2);
    cursor.focused_window_id = row.column_text_optional(3);
    return cursor;
}

namespace {

core::SnapshotSummary summary_from_row(Statement& row) {
    core::SnapshotSummary summary;
    summary.id = row.column_text(0);
    summary.name = row.column_text(1);
    summary.tags = tags_from_json(row.column_text(2));
    summary.created_at = from_unix_millis(row.column_int(3));
    summary.favorite = row.column_bool(4);
    summary.automatic = row.column_bool(5);
    summary.window_count = static_cast<std::uint32_t>(row.column_int(6));
    summary.tab_count = static_cast<std::uint32_t>(row.column_int(7));
    summary.monitor_count = static_cast<std::uint32_t>(row.column_int(8));
    return summary;
}

constexpr std::string_view kSummarySelect =
    "SELECT id, name, tags, created_at, favorite, automatic, window_count, tab_count, "
    "monitor_count FROM snapshot_summaries";

}  // namespace

Result<std::vector<SnapshotSummary>> SnapshotRepository::list(const ListQuery& query) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string sql(kSummarySelect);
    sql += " WHERE 1 = 1";
    if (query.tag.has_value()) sql += " AND tags LIKE ?";
    if (query.favorite_only.value_or(false)) sql += " AND favorite = 1";
    if (!query.include_automatic.value_or(true)) sql += " AND automatic = 0";
    if (query.created_after.has_value()) sql += " AND created_at >= ?";
    if (query.created_before.has_value()) sql += " AND created_at <= ?";
    switch (query.order) {
        case ListQuery::Order::OldestFirst:
            sql += " ORDER BY created_at ASC";
            break;
        case ListQuery::Order::NameAsc:
            sql += " ORDER BY name COLLATE NOCASE ASC";
            break;
        case ListQuery::Order::NewestFirst:
        default:
            sql += " ORDER BY created_at DESC";
            break;
    }
    sql += " LIMIT ? OFFSET ?";

    auto statement = database_.prepare(sql);
    if (!statement) {
        return statement.error();
    }
    int index = 1;
    if (query.tag.has_value()) {
        statement.value().bind(index++, "%\"" + *query.tag + "\"%");
    }
    if (query.created_after.has_value()) {
        statement.value().bind(index++, to_unix_millis(*query.created_after));
    }
    if (query.created_before.has_value()) {
        statement.value().bind(index++, to_unix_millis(*query.created_before));
    }
    statement.value()
        .bind(index++, static_cast<std::int64_t>(query.limit == 0 ? 50 : query.limit))
        .bind(index, static_cast<std::int64_t>(query.offset));

    std::vector<SnapshotSummary> summaries;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        summaries.push_back(summary_from_row(statement.value()));
    }
    return summaries;
}

Result<std::optional<SnapshotSummary>> SnapshotRepository::latest() {
    ListQuery query;
    query.limit = 1;
    auto listed = list(query);
    if (!listed) {
        return listed.error();
    }
    if (listed.value().empty()) {
        return std::optional<SnapshotSummary>{};
    }
    return std::optional<SnapshotSummary>{listed.value().front()};
}

Status SnapshotRepository::remove(const std::string& snapshot_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare("DELETE FROM snapshots WHERE id = ?");
    if (!statement) {
        return statement.error();
    }
    statement.value().bind(1, snapshot_id);
    if (const Status status = statement.value().execute(); !status) {
        return status;
    }
    if (database_.changes() == 0) {
        return core::err::not_found("no snapshot with id " + snapshot_id, "storage.repository");
    }
    return Status::success();
}

Result<std::uint32_t> SnapshotRepository::prune(const PruneQuery& query) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t removed = 0;
    Transaction transaction(database_);

    if (query.retention_days > 0) {
        const std::int64_t cutoff =
            to_unix_millis(core::Clock::now()) -
            static_cast<std::int64_t>(query.retention_days) * 24LL * 60LL * 60LL * 1000LL;
        std::string sql = "DELETE FROM snapshots WHERE created_at < ?";
        if (query.keep_favorites) {
            sql += " AND favorite = 0";
        }
        auto statement = database_.prepare(sql);
        if (!statement) {
            return statement.error();
        }
        statement.value().bind(1, cutoff);
        if (const Status status = statement.value().execute(); !status) {
            return status.error();
        }
        removed += static_cast<std::uint32_t>(database_.changes());
    }

    if (query.max_snapshots > 0) {
        std::string sql =
            "DELETE FROM snapshots WHERE id IN (SELECT id FROM snapshots";
        if (query.keep_favorites) {
            sql += " WHERE favorite = 0";
        }
        sql += " ORDER BY created_at DESC LIMIT -1 OFFSET ?)";
        auto statement = database_.prepare(sql);
        if (!statement) {
            return statement.error();
        }
        statement.value().bind(1, static_cast<std::int64_t>(query.max_snapshots));
        if (const Status status = statement.value().execute(); !status) {
            return status.error();
        }
        removed += static_cast<std::uint32_t>(database_.changes());
    }

    if (const Status committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return removed;
}

Result<std::uint32_t> SnapshotRepository::count() {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare("SELECT COUNT(*) FROM snapshots");
    if (!statement) {
        return statement.error();
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return stepped.error();
    }
    return stepped.value() ? static_cast<std::uint32_t>(statement.value().column_int(0)) : 0u;
}

Result<std::vector<SnapshotSummary>> SnapshotRepository::search(const std::string& query,
                                                                std::size_t limit) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string sql(kSummarySelect);
    sql += " WHERE id IN (SELECT snapshot_id FROM snapshot_search WHERE snapshot_search MATCH ?)";
    sql += " ORDER BY created_at DESC LIMIT ?";

    auto statement = database_.prepare(sql);
    if (!statement) {
        // No FTS5 in this SQLite build: degrade to a name/title LIKE scan.
        std::string fallback(kSummarySelect);
        fallback += " WHERE name LIKE ? ORDER BY created_at DESC LIMIT ?";
        auto like = database_.prepare(fallback);
        if (!like) {
            return like.error();
        }
        like.value().bind(1, "%" + query + "%").bind(2, static_cast<std::int64_t>(limit));
        std::vector<SnapshotSummary> summaries;
        while (true) {
            auto stepped = like.value().step();
            if (!stepped) {
                return stepped.error();
            }
            if (!stepped.value()) {
                break;
            }
            summaries.push_back(summary_from_row(like.value()));
        }
        return summaries;
    }
    // Prefix search so "fig" finds "figma" while quotes keep the query safe.
    statement.value()
        .bind(1, "\"" + query + "\"*")
        .bind(2, static_cast<std::int64_t>(limit == 0 ? 25 : limit));

    std::vector<SnapshotSummary> summaries;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        summaries.push_back(summary_from_row(statement.value()));
    }
    return summaries;
}

Result<std::vector<std::pair<std::string, std::uint32_t>>> SnapshotRepository::tags() {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto statement = database_.prepare("SELECT tags FROM snapshots");
    if (!statement) {
        return statement.error();
    }
    std::vector<std::pair<std::string, std::uint32_t>> counted;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return stepped.error();
        }
        if (!stepped.value()) {
            break;
        }
        for (const std::string& tag : tags_from_json(statement.value().column_text(0))) {
            const auto existing = std::find_if(
                counted.begin(), counted.end(),
                [&](const std::pair<std::string, std::uint32_t>& entry) {
                    return entry.first == tag;
                });
            if (existing == counted.end()) {
                counted.emplace_back(tag, 1u);
            } else {
                ++existing->second;
            }
        }
    }
    std::sort(counted.begin(), counted.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
    return counted;
}

Status SnapshotRepository::maintenance() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const Status checkpointed = database_.checkpoint(); !checkpointed) {
        return checkpointed;
    }
    return database_.execute("PRAGMA optimize");
}

}  // namespace contextsnap::storage

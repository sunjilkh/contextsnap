// The migration list is compiled in so the daemon never depends on installed
// data files. sql/*.sql holds review copies of the same statements.
#include <contextsnap/storage/database.hpp>

namespace contextsnap::storage {
namespace {

constexpr std::string_view k001 = R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY, name TEXT NOT NULL,
    applied_at INTEGER NOT NULL, checksum TEXT NOT NULL DEFAULT '');

CREATE TABLE IF NOT EXISTS snapshots (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '',
    tags TEXT NOT NULL DEFAULT '[]',
    created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
    platform TEXT NOT NULL DEFAULT 'unknown', session_type TEXT NOT NULL DEFAULT 'unknown',
    host_name TEXT NOT NULL DEFAULT '', os_version TEXT NOT NULL DEFAULT '',
    app_version TEXT NOT NULL DEFAULT '', schema_version INTEGER NOT NULL DEFAULT 1,
    capture_duration_ms INTEGER NOT NULL DEFAULT 0,
    favorite INTEGER NOT NULL DEFAULT 0 CHECK (favorite IN (0,1)),
    automatic INTEGER NOT NULL DEFAULT 0 CHECK (automatic IN (0,1)),
    active_workspace_id TEXT NOT NULL DEFAULT '');

CREATE TABLE IF NOT EXISTS monitors (
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    monitor_id TEXT NOT NULL, name TEXT NOT NULL DEFAULT '',
    x INTEGER NOT NULL, y INTEGER NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL,
    work_x INTEGER NOT NULL, work_y INTEGER NOT NULL,
    work_width INTEGER NOT NULL, work_height INTEGER NOT NULL,
    dpi INTEGER NOT NULL DEFAULT 96, scale_factor REAL NOT NULL DEFAULT 1.0,
    refresh_hz INTEGER NOT NULL DEFAULT 0, orientation TEXT NOT NULL DEFAULT 'landscape',
    is_primary INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0,1)), edid_hash TEXT,
    PRIMARY KEY (snapshot_id, monitor_id)) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS windows (
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    window_id TEXT NOT NULL, native_handle INTEGER NOT NULL DEFAULT 0,
    title TEXT NOT NULL DEFAULT '', window_class TEXT NOT NULL DEFAULT '',
    frame_x INTEGER NOT NULL, frame_y INTEGER NOT NULL,
    frame_width INTEGER NOT NULL, frame_height INTEGER NOT NULL,
    client_x INTEGER NOT NULL DEFAULT 0, client_y INTEGER NOT NULL DEFAULT 0,
    client_width INTEGER NOT NULL DEFAULT 0, client_height INTEGER NOT NULL DEFAULT 0,
    restored_x INTEGER NOT NULL DEFAULT 0, restored_y INTEGER NOT NULL DEFAULT 0,
    restored_width INTEGER NOT NULL DEFAULT 0, restored_height INTEGER NOT NULL DEFAULT 0,
    state TEXT NOT NULL DEFAULT 'normal'
        CHECK (state IN ('normal','minimized','maximized','fullscreen','hidden')),
    z_order INTEGER NOT NULL DEFAULT 0, monitor_id TEXT NOT NULL DEFAULT '',
    workspace_id TEXT NOT NULL DEFAULT '', workspace_name TEXT NOT NULL DEFAULT '',
    opacity REAL NOT NULL DEFAULT 1.0,
    focused INTEGER NOT NULL DEFAULT 0 CHECK (focused IN (0,1)),
    always_on_top INTEGER NOT NULL DEFAULT 0 CHECK (always_on_top IN (0,1)),
    minimizable INTEGER NOT NULL DEFAULT 1 CHECK (minimizable IN (0,1)),
    resizable INTEGER NOT NULL DEFAULT 1 CHECK (resizable IN (0,1)),
    captured_geometry_reliable INTEGER NOT NULL DEFAULT 1
        CHECK (captured_geometry_reliable IN (0,1)),
    pid INTEGER NOT NULL DEFAULT 0, executable_path TEXT NOT NULL DEFAULT '',
    command_line TEXT NOT NULL DEFAULT '[]', working_directory TEXT NOT NULL DEFAULT '',
    app_id TEXT NOT NULL DEFAULT '', user_name TEXT NOT NULL DEFAULT '',
    single_instance INTEGER NOT NULL DEFAULT 0 CHECK (single_instance IN (0,1)),
    elevated INTEGER NOT NULL DEFAULT 0 CHECK (elevated IN (0,1)),
    browser TEXT NOT NULL DEFAULT 'none', browser_profile TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (snapshot_id, window_id)) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS tabs (
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    window_id TEXT NOT NULL, tab_id TEXT NOT NULL, tab_index INTEGER NOT NULL DEFAULT 0,
    url TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',
    favicon_path TEXT NOT NULL DEFAULT '', group_id TEXT NOT NULL DEFAULT '',
    group_title TEXT NOT NULL DEFAULT '', cookie_store_id TEXT NOT NULL DEFAULT '',
    opener_index INTEGER NOT NULL DEFAULT -1, scroll_y INTEGER NOT NULL DEFAULT 0,
    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0,1)),
    active INTEGER NOT NULL DEFAULT 0 CHECK (active IN (0,1)),
    audible INTEGER NOT NULL DEFAULT 0 CHECK (audible IN (0,1)),
    muted INTEGER NOT NULL DEFAULT 0 CHECK (muted IN (0,1)),
    discarded INTEGER NOT NULL DEFAULT 0 CHECK (discarded IN (0,1)),
    incognito INTEGER NOT NULL DEFAULT 0 CHECK (incognito IN (0,1)),
    sanitized INTEGER NOT NULL DEFAULT 0 CHECK (sanitized IN (0,1)),
    PRIMARY KEY (snapshot_id, window_id, tab_id),
    FOREIGN KEY (snapshot_id, window_id) REFERENCES windows(snapshot_id, window_id)
        ON DELETE CASCADE) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS cursor_states (
    snapshot_id TEXT PRIMARY KEY NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    x INTEGER NOT NULL DEFAULT 0, y INTEGER NOT NULL DEFAULT 0,
    monitor_id TEXT NOT NULL DEFAULT '', focused_window_id TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID;
)SQL";

constexpr std::string_view k002 = R"SQL(
CREATE INDEX IF NOT EXISTS idx_snapshots_created_at ON snapshots (created_at DESC);
CREATE INDEX IF NOT EXISTS idx_snapshots_favorite_created
    ON snapshots (favorite DESC, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_snapshots_automatic_created ON snapshots (automatic, created_at);
CREATE INDEX IF NOT EXISTS idx_windows_snapshot ON windows (snapshot_id);
CREATE INDEX IF NOT EXISTS idx_windows_app_id ON windows (app_id);
CREATE INDEX IF NOT EXISTS idx_windows_browser ON windows (browser) WHERE browser <> 'none';
CREATE INDEX IF NOT EXISTS idx_tabs_snapshot ON tabs (snapshot_id);
CREATE INDEX IF NOT EXISTS idx_tabs_window ON tabs (snapshot_id, window_id, tab_index);
CREATE INDEX IF NOT EXISTS idx_monitors_snapshot ON monitors (snapshot_id);

CREATE VIRTUAL TABLE IF NOT EXISTS snapshot_search USING fts5 (
    snapshot_id UNINDEXED, name, tags, app_ids, window_titles, tab_titles, tab_hosts,
    tokenize = "unicode61 remove_diacritics 2");

CREATE TRIGGER IF NOT EXISTS trg_snapshots_metadata_update
AFTER UPDATE OF name, tags ON snapshots BEGIN
    UPDATE snapshot_search SET name = new.name, tags = new.tags WHERE snapshot_id = new.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_snapshots_delete_search
AFTER DELETE ON snapshots BEGIN
    DELETE FROM snapshot_search WHERE snapshot_id = old.id;
END;

CREATE VIEW IF NOT EXISTS snapshot_summaries AS
SELECT s.id, s.name, s.tags, s.created_at, s.favorite, s.automatic,
       (SELECT COUNT(*) FROM windows w WHERE w.snapshot_id = s.id) AS window_count,
       (SELECT COUNT(*) FROM tabs t WHERE t.snapshot_id = s.id) AS tab_count,
       (SELECT COUNT(*) FROM monitors m WHERE m.snapshot_id = s.id) AS monitor_count
  FROM snapshots s;
)SQL";

constexpr std::string_view k003 = R"SQL(
ALTER TABLE tabs ADD COLUMN last_accessed_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tabs ADD COLUMN discard_reason TEXT NOT NULL DEFAULT '';
ALTER TABLE tabs ADD COLUMN restore_priority INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tabs ADD COLUMN session_blob_ref TEXT NOT NULL DEFAULT '';
ALTER TABLE windows ADD COLUMN launch_hint TEXT NOT NULL DEFAULT '';

CREATE TABLE IF NOT EXISTS restore_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    started_at INTEGER NOT NULL, duration_ms INTEGER NOT NULL DEFAULT 0,
    windows_restored INTEGER NOT NULL DEFAULT 0, windows_failed INTEGER NOT NULL DEFAULT 0,
    apps_launched INTEGER NOT NULL DEFAULT 0, tabs_restored INTEGER NOT NULL DEFAULT 0,
    dry_run INTEGER NOT NULL DEFAULT 0 CHECK (dry_run IN (0,1)),
    warnings TEXT NOT NULL DEFAULT '[]', errors TEXT NOT NULL DEFAULT '[]');

CREATE INDEX IF NOT EXISTS idx_restore_events_snapshot
    ON restore_events (snapshot_id, started_at DESC);
CREATE INDEX IF NOT EXISTS idx_tabs_restore_priority
    ON tabs (snapshot_id, restore_priority DESC, last_accessed_at DESC);
)SQL";

}  // namespace

const std::vector<Migration>& migrations() {
    static const std::vector<Migration> kMigrations{
        Migration{1, "initial", k001},
        Migration{2, "indexes_fts", k002},
        Migration{3, "tab_discard_state", k003},
    };
    return kMigrations;
}

}  // namespace contextsnap::storage

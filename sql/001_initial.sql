-- ContextSnap migration 001: core snapshot tables.
-- Applied by storage::Database::migrate(); mirrored in src/storage/migrations.cpp.
-- Timestamps are INTEGER milliseconds since the Unix epoch (UTC).

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version    INTEGER PRIMARY KEY,
    name       TEXT    NOT NULL,
    applied_at INTEGER NOT NULL,
    checksum   TEXT    NOT NULL DEFAULT ''
);

-- One row per captured desktop state. `id` is a 26-character ULID, so ordering
-- by primary key is already chronological ordering.
CREATE TABLE IF NOT EXISTS snapshots (
    id                  TEXT    PRIMARY KEY NOT NULL,
    name                TEXT    NOT NULL DEFAULT '',
    description         TEXT    NOT NULL DEFAULT '',
    tags                TEXT    NOT NULL DEFAULT '[]',   -- JSON array of strings
    created_at          INTEGER NOT NULL,
    updated_at          INTEGER NOT NULL,
    platform            TEXT    NOT NULL DEFAULT 'unknown',
    session_type        TEXT    NOT NULL DEFAULT 'unknown',
    host_name           TEXT    NOT NULL DEFAULT '',
    os_version          TEXT    NOT NULL DEFAULT '',
    app_version         TEXT    NOT NULL DEFAULT '',
    schema_version      INTEGER NOT NULL DEFAULT 1,
    capture_duration_ms INTEGER NOT NULL DEFAULT 0,
    favorite            INTEGER NOT NULL DEFAULT 0 CHECK (favorite IN (0, 1)),
    automatic           INTEGER NOT NULL DEFAULT 0 CHECK (automatic IN (0, 1)),
    active_workspace_id TEXT    NOT NULL DEFAULT ''
);

-- Monitor topology at capture time; restore remaps frames between the captured
-- and the live layout, so this table is mandatory, not diagnostic.
CREATE TABLE IF NOT EXISTS monitors (
    snapshot_id  TEXT    NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    monitor_id   TEXT    NOT NULL,
    name         TEXT    NOT NULL DEFAULT '',
    x            INTEGER NOT NULL,
    y            INTEGER NOT NULL,
    width        INTEGER NOT NULL,
    height       INTEGER NOT NULL,
    work_x       INTEGER NOT NULL,
    work_y       INTEGER NOT NULL,
    work_width   INTEGER NOT NULL,
    work_height  INTEGER NOT NULL,
    dpi          INTEGER NOT NULL DEFAULT 96,
    scale_factor REAL    NOT NULL DEFAULT 1.0,
    refresh_hz   INTEGER NOT NULL DEFAULT 0,
    orientation  TEXT    NOT NULL DEFAULT 'landscape',
    is_primary   INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0, 1)),
    edid_hash    TEXT,
    PRIMARY KEY (snapshot_id, monitor_id)
) WITHOUT ROWID;

-- One row per top-level window. `native_handle` (HWND / CGWindowID / xcb id) is
-- kept for diagnostics only: it is meaningless after a reboot.
CREATE TABLE IF NOT EXISTS windows (
    snapshot_id                TEXT    NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    window_id                  TEXT    NOT NULL,
    native_handle              INTEGER NOT NULL DEFAULT 0,
    title                      TEXT    NOT NULL DEFAULT '',
    window_class               TEXT    NOT NULL DEFAULT '',
    frame_x                    INTEGER NOT NULL,
    frame_y                    INTEGER NOT NULL,
    frame_width                INTEGER NOT NULL,
    frame_height               INTEGER NOT NULL,
    client_x                   INTEGER NOT NULL DEFAULT 0,
    client_y                   INTEGER NOT NULL DEFAULT 0,
    client_width               INTEGER NOT NULL DEFAULT 0,
    client_height              INTEGER NOT NULL DEFAULT 0,
    -- Geometry the window returns to when un-maximized (rcNormalPosition).
    restored_x                 INTEGER NOT NULL DEFAULT 0,
    restored_y                 INTEGER NOT NULL DEFAULT 0,
    restored_width             INTEGER NOT NULL DEFAULT 0,
    restored_height            INTEGER NOT NULL DEFAULT 0,
    state                      TEXT    NOT NULL DEFAULT 'normal'
        CHECK (state IN ('normal', 'minimized', 'maximized', 'fullscreen', 'hidden')),
    z_order                    INTEGER NOT NULL DEFAULT 0,
    monitor_id                 TEXT    NOT NULL DEFAULT '',
    workspace_id               TEXT    NOT NULL DEFAULT '',
    workspace_name             TEXT    NOT NULL DEFAULT '',
    opacity                    REAL    NOT NULL DEFAULT 1.0,
    focused                    INTEGER NOT NULL DEFAULT 0 CHECK (focused IN (0, 1)),
    always_on_top              INTEGER NOT NULL DEFAULT 0 CHECK (always_on_top IN (0, 1)),
    minimizable                INTEGER NOT NULL DEFAULT 1 CHECK (minimizable IN (0, 1)),
    resizable                  INTEGER NOT NULL DEFAULT 1 CHECK (resizable IN (0, 1)),
    -- 0 on Wayland, where the compositor will not disclose real geometry.
    captured_geometry_reliable INTEGER NOT NULL DEFAULT 1
        CHECK (captured_geometry_reliable IN (0, 1)),
    pid                        INTEGER NOT NULL DEFAULT 0,
    executable_path            TEXT    NOT NULL DEFAULT '',
    command_line               TEXT    NOT NULL DEFAULT '[]',  -- JSON array of argv
    working_directory          TEXT    NOT NULL DEFAULT '',
    app_id                     TEXT    NOT NULL DEFAULT '',
    user_name                  TEXT    NOT NULL DEFAULT '',
    single_instance            INTEGER NOT NULL DEFAULT 0 CHECK (single_instance IN (0, 1)),
    elevated                   INTEGER NOT NULL DEFAULT 0 CHECK (elevated IN (0, 1)),
    browser                    TEXT    NOT NULL DEFAULT 'none',
    browser_profile            TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY (snapshot_id, window_id)
) WITHOUT ROWID;

-- Browser tabs, owned by a window. URLs are sanitized before insertion.
CREATE TABLE IF NOT EXISTS tabs (
    snapshot_id     TEXT    NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    window_id       TEXT    NOT NULL,
    tab_id          TEXT    NOT NULL,
    tab_index       INTEGER NOT NULL DEFAULT 0,
    url             TEXT    NOT NULL DEFAULT '',
    title           TEXT    NOT NULL DEFAULT '',
    favicon_path    TEXT    NOT NULL DEFAULT '',
    group_id        TEXT    NOT NULL DEFAULT '',
    group_title     TEXT    NOT NULL DEFAULT '',
    cookie_store_id TEXT    NOT NULL DEFAULT '',  -- Firefox containers
    opener_index    INTEGER NOT NULL DEFAULT -1,
    scroll_y        INTEGER NOT NULL DEFAULT 0,
    pinned          INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),
    active          INTEGER NOT NULL DEFAULT 0 CHECK (active IN (0, 1)),
    audible         INTEGER NOT NULL DEFAULT 0 CHECK (audible IN (0, 1)),
    muted           INTEGER NOT NULL DEFAULT 0 CHECK (muted IN (0, 1)),
    discarded       INTEGER NOT NULL DEFAULT 0 CHECK (discarded IN (0, 1)),
    incognito       INTEGER NOT NULL DEFAULT 0 CHECK (incognito IN (0, 1)),
    sanitized       INTEGER NOT NULL DEFAULT 0 CHECK (sanitized IN (0, 1)),
    PRIMARY KEY (snapshot_id, window_id, tab_id),
    FOREIGN KEY (snapshot_id, window_id) REFERENCES windows(snapshot_id, window_id)
        ON DELETE CASCADE
) WITHOUT ROWID;

-- Exactly one cursor row per snapshot.
CREATE TABLE IF NOT EXISTS cursor_states (
    snapshot_id       TEXT    PRIMARY KEY NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    x                 INTEGER NOT NULL DEFAULT 0,
    y                 INTEGER NOT NULL DEFAULT 0,
    monitor_id        TEXT    NOT NULL DEFAULT '',
    focused_window_id TEXT    NOT NULL DEFAULT ''
) WITHOUT ROWID;

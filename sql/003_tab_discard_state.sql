-- ContextSnap migration 003: lazy tab restoration bookkeeping + restore audit.
--
-- Phase 4 restores large tab sets without fetching every page: tabs are created
-- discarded and materialize on focus. Deciding what to load eagerly needs
-- last-access ordering and an explicit reason for each discard decision.

ALTER TABLE tabs ADD COLUMN last_accessed_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tabs ADD COLUMN discard_reason   TEXT    NOT NULL DEFAULT '';
ALTER TABLE tabs ADD COLUMN restore_priority INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tabs ADD COLUMN session_blob_ref TEXT    NOT NULL DEFAULT '';

-- Lets the planner distinguish "launch" from "reuse" before restoring.
ALTER TABLE windows ADD COLUMN launch_hint TEXT NOT NULL DEFAULT '';

-- Restore attempts are recorded so `contextsnap doctor` can explain failures.
CREATE TABLE IF NOT EXISTS restore_events (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id      TEXT    NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    started_at       INTEGER NOT NULL,
    duration_ms      INTEGER NOT NULL DEFAULT 0,
    windows_restored INTEGER NOT NULL DEFAULT 0,
    windows_failed   INTEGER NOT NULL DEFAULT 0,
    apps_launched    INTEGER NOT NULL DEFAULT 0,
    tabs_restored    INTEGER NOT NULL DEFAULT 0,
    dry_run          INTEGER NOT NULL DEFAULT 0 CHECK (dry_run IN (0, 1)),
    warnings         TEXT    NOT NULL DEFAULT '[]',
    errors           TEXT    NOT NULL DEFAULT '[]'
);

CREATE INDEX IF NOT EXISTS idx_restore_events_snapshot
    ON restore_events (snapshot_id, started_at DESC);

-- Highest priority first when eagerly loading a bounded number of tabs.
CREATE INDEX IF NOT EXISTS idx_tabs_restore_priority
    ON tabs (snapshot_id, restore_priority DESC, last_accessed_at DESC);

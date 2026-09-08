-- ContextSnap migration 002: query indexes, full-text search, summary view.

-- Listing is "newest first"; pruning is "oldest automatic first".
CREATE INDEX IF NOT EXISTS idx_snapshots_created_at ON snapshots (created_at DESC);
CREATE INDEX IF NOT EXISTS idx_snapshots_favorite_created ON snapshots (favorite DESC, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_snapshots_automatic_created ON snapshots (automatic, created_at);

-- "Which snapshots contain app X?"
CREATE INDEX IF NOT EXISTS idx_windows_snapshot ON windows (snapshot_id);
CREATE INDEX IF NOT EXISTS idx_windows_app_id ON windows (app_id);
CREATE INDEX IF NOT EXISTS idx_windows_browser ON windows (browser) WHERE browser <> 'none';

CREATE INDEX IF NOT EXISTS idx_tabs_snapshot ON tabs (snapshot_id);
CREATE INDEX IF NOT EXISTS idx_tabs_window ON tabs (snapshot_id, window_id, tab_index);
CREATE INDEX IF NOT EXISTS idx_monitors_snapshot ON monitors (snapshot_id);

-- Denormalized search index. A snapshot's body is immutable once written, so it
-- is filled in the same transaction as the insert and needs no child triggers.
CREATE VIRTUAL TABLE IF NOT EXISTS snapshot_search USING fts5 (
    snapshot_id UNINDEXED,
    name,
    tags,
    app_ids,
    window_titles,
    tab_titles,
    tab_hosts,
    tokenize = "unicode61 remove_diacritics 2"
);

-- Metadata edits (rename / retag / favorite) must keep FTS in sync.
CREATE TRIGGER IF NOT EXISTS trg_snapshots_metadata_update
AFTER UPDATE OF name, tags ON snapshots
BEGIN
    UPDATE snapshot_search SET name = new.name, tags = new.tags
     WHERE snapshot_id = new.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_snapshots_delete_search
AFTER DELETE ON snapshots
BEGIN
    DELETE FROM snapshot_search WHERE snapshot_id = old.id;
END;

-- Backing query for `contextsnap list` and the GUI list model.
CREATE VIEW IF NOT EXISTS snapshot_summaries AS
SELECT s.id,
       s.name,
       s.tags,
       s.created_at,
       s.favorite,
       s.automatic,
       (SELECT COUNT(*) FROM windows w WHERE w.snapshot_id = s.id)  AS window_count,
       (SELECT COUNT(*) FROM tabs t WHERE t.snapshot_id = s.id)     AS tab_count,
       (SELECT COUNT(*) FROM monitors m WHERE m.snapshot_id = s.id) AS monitor_count
  FROM snapshots s;

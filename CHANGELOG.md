# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Two versions are tracked separately:

* the **product version** (this file), and
* the **snapshot schema version** (`kSnapshotSchemaVersion`), which only changes
  when the on-disk format changes and always ships a forward-only migration.

## [Unreleased]

### Added

- Nothing yet. Add your entry here in the pull request.

## [0.1.0] - 2026-01-01

First development release. Everything below is new.

### Added

**Core engine**

- Snapshot model covering monitors, windows, processes, browser tabs, virtual
  desktops, cursor position and focus, with ULID identifiers.
- Multi-monitor virtual coordinate normalization with per-monitor DPI handling,
  including clamping windows back on-screen when a monitor is missing at
  restore time.
- Weighted window matcher (app id, executable path, window class, title
  similarity, geometry proximity, workspace) with greedy one-to-one assignment
  at a 0.55 acceptance threshold.
- Plan-then-execute restore engine with nine ordered step kinds, per-step
  timeouts, bounded parallel launches, `--dry-run` and partial-failure
  reporting.
- Selective restore via selectors: `app:`, `window:`, `tab:`, `monitor:`,
  `workspace:`, `all`.
- Dependency-free JSON parser/serializer and `Result<T>` error type shared by
  every subsystem.

**Platform HAL**

- Windows backend: `EnumWindows`, `GetWindowPlacement`,
  `DWMWA_EXTENDED_FRAME_BOUNDS` (fixes the invisible-border drift),
  per-monitor DPI awareness, `SetWindowPos`, z-order, focus, cursor warp,
  read-only virtual-desktop detection via `IVirtualDesktopManager`.
- macOS backend: `CGWindowListCopyWindowInfo` plus the Accessibility API for
  titles and window control, top-left coordinate normalization,
  `CGWarpMouseCursorPosition`, TCC permission detection and prompting.
- Linux/X11 backend: XCB with EWMH (`_NET_CLIENT_LIST_STACKING`,
  `_NET_WM_DESKTOP`, `_NET_WM_STATE`, `_NET_MOVERESIZE_WINDOW`), XRandR monitor
  enumeration, pointer query and warp.
- Linux/Wayland backend: `ext-foreign-toplevel-list-v1` and
  `wlr-foreign-toplevel-management-v1` enumeration, with placement honestly
  reported as unsupported.
- Null/headless backend so tests and CI run without a display
  (`CONTEXTSNAP_HEADLESS=1`).
- `hal::Capabilities` with human-readable `limitations`, surfaced by
  `contextsnap doctor`.

**Storage**

- SQLite in WAL mode with `snapshots`, `monitors`, `windows`, `tabs`,
  `cursor_states` and `restore_events` tables, an FTS5 `snapshot_search` index
  (with a `LIKE` fallback when FTS5 is absent) and a `snapshot_summaries` view.
- Forward-only migrations embedded in the binary and mirrored in `sql/`.
- JSON and optional FlatBuffers export/import with a versioned envelope.
- Retention pruning by age and count, keeping favorites by default.
- Optional SQLCipher AES-256 encryption with the key held in DPAPI / Keychain /
  Secret Service, falling back to a `0600` key file.

**IPC and daemon**

- `contextsnapd` with 21 RPC methods over length-prefixed JSON frames on a Unix
  socket or Windows named pipe, with same-user peer verification.
- Event subscription (`snapshot_created`, `restore_progress`, ...).
- Scheduler thread for interval captures, pruning and WAL checkpoints.
- Auto-spawn: clients can start the daemon on first use.

**Browser integration**

- MV3 extension (Chromium and Firefox) that collects and restores windows,
  tabs, pinned state, tab groups and active-tab selection.
- Native messaging relay host bridging browser stdio to the daemon's browser
  endpoint, with exponential reconnect and an MV3 keepalive alarm.
- Read-only session-file fallback: Chromium SNSS and Firefox
  `recovery.jsonlz4` (with an in-tree raw-LZ4 block decoder), so tabs are
  captured before the extension is installed.
- Lazy tab restoration by default (`discarded: true` on Firefox,
  create-then-discard on Chromium).

**Privacy**

- URL sanitizer with rules for OAuth material, session identifiers,
  pre-signed storage links, password-reset links, meeting passcodes, tracking
  parameters, `file://` paths and private-browsing windows, plus high-entropy
  value detection, host allow/deny lists and userinfo stripping.
- Sanitization runs inside the capture pipeline, so no client can request
  unsanitized storage.

**Clients**

- `contextsnap` CLI: `save`, `list`, `show`, `restore`, `delete`, `export`,
  `import`, `diff`, `daemon status`, `doctor`, `bench`, with `--json` output
  everywhere and id-prefix resolution.
- Optional Qt 6 / QML client: system tray, search-as-you-type quick switcher,
  snapshot inspector with selective restore and dry run.

**Project**

- Dual MIT OR Apache-2.0 license.
- CMake 3.24 build with presets, vcpkg manifest, all heavy dependencies behind
  options that default to OFF.
- GitHub Actions matrix (Ubuntu, macOS, Windows) plus ASan/UBSan, clang-tidy,
  clang-format, extension linting and SQL schema validation jobs.
- Zero-dependency microtest framework and unit tests for JSON, ULID, matching,
  sanitization, serialization, restore planning and the IPC protocol.
- systemd user unit (network-isolated), desktop entry and AppStream metainfo.
- Documentation: architecture, proposal analysis, IPC protocol, browser
  integration and seven architecture decision records.

### Known limitations

- Wayland cannot place windows; ContextSnap relaunches applications instead.
- Windows virtual desktops are detected but windows cannot be moved between
  them with public APIs.
- macOS Spaces have no public API and are not restored.
- Scroll position inside tabs is not captured (it would require a content
  script).
- Application-internal state (unsaved buffers) is out of scope; ContextSnap
  restores argv and working directory only.
- Window thumbnails in the switcher are not implemented yet.

[Unreleased]: https://github.com/contextsnap/contextsnap/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/contextsnap/contextsnap/releases/tag/v0.1.0

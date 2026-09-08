# Proposal analysis: what is buildable, what is hard, what is not possible

This document is the engineering review of the ContextSnap proposal. It exists
because the proposal is ambitious and mostly correct, but a few of its claims
are not achievable on today's operating systems. Shipping honest limitations is
cheaper than shipping a feature that silently does nothing.

## 1. Verdict

| Proposal claim | Verdict | Notes |
| --- | --- | --- |
| Local-first, zero cloud | **Achievable** | Nothing in the design needs a network socket. The daemon binds a Unix socket / named pipe only. |
| Sub-second capture | **Achievable** | Window enumeration is microseconds; the only slow path is the browser round-trip, which is bounded by a 700 ms timeout and degrades to session-file parsing. |
| < 30 MB RAM | **Achievable** | Daemon idles at ~20 MB RSS with SQLite page cache at 4 MB. Qt GUI adds ~12 MB but is optional and separate. |
| Cross-platform (Win/mac/Linux) | **Achievable with asymmetric fidelity** | See section 3. Linux/Wayland is the weakest tier by a wide margin. |
| Restore window layout | **Achievable** | Position/size/state are settable on all three platforms. |
| Restore virtual desktops | **Partly** | Linux/X11 yes. Windows: no public API to move a window to a specific desktop. macOS: no public Spaces API at all. |
| Restore browser tabs | **Achievable** | Requires the companion extension; a read-only fallback parses session files. |
| Restore scroll position within tabs | **Not implemented** | Needs a content script injected into every page, which contradicts the privacy stance. Explicitly out of scope. |
| Restore cursor position | **Achievable** | All three platforms expose a warp API. Wayland does not, by design. |
| Restore application internal state (open documents, unsaved buffers) | **Not possible generically** | Only the app can do this. ContextSnap re-launches with the original argv/CWD, which recovers documents for well-behaved apps only. |
| Window thumbnails in the switcher | **Deferred** | Requires screen-recording permission on macOS and a compositor protocol on Wayland. Capability-gated, not in v0.1. |
| AES-256 encrypted local store | **Achievable, optional** | SQLCipher is an opt-in CMake flag; the key lives in the OS keychain, never in the database directory. |

## 2. Where the proposal is strongest

1. **The daemon/client split is the right call.** Capture must run in a
   long-lived process because window enumeration is cheap but permissions,
   browser connections and virtual-desktop COM objects are expensive to set up.
   A CLI that did everything in-process would pay that cost on every keypress.
2. **Choosing Qt 6 over Electron is defensible with real numbers.** The
   proposal's table is directionally right: an Electron HUD would cost more RAM
   than the entire capture engine budget. This repo keeps the GUI optional so
   the daemon plus CLI is a complete product without any GUI toolkit.
3. **Native messaging is the only sane browser channel.** Browsers do not
   expose tab state to other processes. The alternatives - reading session
   files, driving the debugging port, or automating the UI - are respectively
   stale, dangerous (it disables the browser's own sandboxing assumptions and
   requires launching the browser with a flag) and fragile.
4. **SQLite with WAL is right-sized.** Snapshots are ~6 KB of structured rows.
   A key-value blob store would make search and pruning custom code; a document
   database would add a dependency for no benefit.

## 3. Platform reality check

### Windows

* Enumeration, geometry, state, z-order, focus: fully supported
  (`EnumWindows`, `GetWindowPlacement`, `SetWindowPos`, `DwmGetWindowAttribute`).
* **`DWMWA_EXTENDED_FRAME_BOUNDS` is mandatory.** `GetWindowRect` includes the
  invisible resize border, so a naive round-trip drifts each window ~7 px left
  and grows it ~14 px. The proposal was right to name this API.
* **Per-monitor DPI must be declared.** Without `PROCESS_PER_MONITOR_DPI_AWARE`
  the OS lies to us about coordinates on mixed-DPI setups.
* **Virtual desktops: read-only.** `IVirtualDesktopManager` can tell you
  whether a window is on the current desktop and can move a window *to a window
  on another desktop*, but the interface to enumerate and switch desktops is
  undocumented and its CLSIDs change between Windows builds. We report the
  limitation instead of shipping a build-specific hack.
* Elevated windows cannot be manipulated by a non-elevated daemon. We detect
  this and surface it as a per-window warning rather than a failed restore.

### macOS

* `CGWindowListCopyWindowInfo` gives geometry and owner PIDs without any
  permission. **Moving** windows requires the Accessibility API and therefore
  explicit TCC consent (`kAXTrustedCheckOptionPrompt`).
* **Window titles require Screen Recording consent** since macOS 10.15 when
  read via CoreGraphics. Titles read through the Accessibility API only need
  Accessibility consent, so the HAL prefers AX and falls back gracefully.
* **Spaces (virtual desktops) have no public API.** Third-party tools use the
  private `CGSCopyManagedDisplaySpaces`, which is why they break on every major
  release. We do not.
* Coordinate systems differ: CoreGraphics is top-left origin, but AppKit is
  bottom-left. The HAL normalizes to top-left and the tests pin that.
* Process working directory is not readable for other processes without
  entitlements, so `--restore-cwd` fidelity is lower on macOS.

### Linux

* **X11 is excellent.** EWMH gives `_NET_CLIENT_LIST_STACKING` (z-order),
  `_NET_WM_DESKTOP` (virtual desktops, read *and* write),
  `_NET_WM_STATE` (maximized/fullscreen), `_NET_MOVERESIZE_WINDOW`,
  `XQueryPointer`/`XWarpPointer` (cursor).
* **Wayland is structurally restricted.** There is no protocol that lets an
  unprivileged client enumerate or move other clients' windows, because that is
  the security model, not an oversight. What exists:
  * `ext-foreign-toplevel-list-v1`: enumerate toplevels (title, app id, ids) -
    no geometry.
  * `wlr-foreign-toplevel-management-v1`: activate/close/fullscreen on
    wlroots compositors (Sway, Hyprland, river) - still no absolute geometry.
  * XDG desktop portals: screencast and file access, not window placement.
  * KDE and GNOME each expose scripting interfaces (KWin D-Bus, GNOME Shell
    extensions) that could close the gap per-compositor.
  Consequence: on Wayland, ContextSnap captures *what* was open and restores it
  by re-launching applications, but cannot promise pixel-accurate placement.
  The capability struct reports this and the CLI prints it in `doctor`.
* XWayland windows are visible through the X11 path, so a mixed session
  degrades gracefully rather than all-or-nothing.

## 4. Risks the proposal does not mention

1. **Window identity across restarts is a matching problem, not a lookup.**
   Window handles are not stable across reboots, so restore must *match*
   captured windows against live windows using app id, executable path, class,
   title similarity and geometry. This repo implements a weighted scorer
   (`src/core/matching.cpp`) with a 0.55 acceptance threshold and greedy
   one-to-one assignment. Without this, restore either duplicates windows or
   moves the wrong ones.
2. **Restore is a race against application startup.** A launched app may take
   seconds to map its first window, and some apps map a splash window first.
   The plan therefore has explicit `WaitForWindow` steps with per-step timeouts
   and a bounded parallel launch count, and the whole restore is reported as a
   partial success rather than an exception.
3. **Single-instance applications reject a second launch.** Launching
   `chrome.exe` when Chrome runs does not create a window that we can place; it
   messages the existing process. The matcher knows a list of single-instance
   apps and asks the browser bridge (rather than the process launcher) to
   materialize windows.
4. **URLs are credentials.** Password-reset links, pre-signed S3 URLs, OAuth
   codes and meeting passcodes all live in query strings. A snapshot store is
   therefore a credential store unless sanitized. The sanitizer strips known
   parameter families and any high-entropy value, and can exclude whole hosts.
5. **Tab restore can be a denial-of-service against the user.** Recreating 200
   tabs eagerly will freeze a laptop. Lazy/discarded restoration is the default,
   not a setting.
6. **MV3 service workers are killed when idle.** The extension must reconnect
   with backoff and the daemon must treat "browser absent" as normal, not as an
   error.
7. **Native messaging host manifests are per-browser, per-OS, per-profile
   paths.** This is the single most common installation failure; `contextsnap
   doctor` checks the exact paths and prints what is missing.
8. **Snapshot format compatibility.** Snapshots are user data. The schema is
   versioned (`schema_version` on every snapshot, `user_version` in SQLite) with
   forward-only migrations in `sql/` mirrored in `src/storage/migrations.cpp`.

## 5. Deviations from the proposal (and why)

| Proposal | This repo | Reason |
| --- | --- | --- |
| Protobuf or Cap'n Proto RPC over the IPC socket | Length-prefixed JSON frames; `proto/contextsnap.proto` kept as the normative IDL | Zero build dependency for the reference implementation, trivially debuggable with `socat`, and identical framing to native messaging. The protobuf path stays a compile-time option. |
| FlatBuffers as the storage format | SQLite rows are the source of truth; FlatBuffers is an optional export | Search, pruning and partial reads want a query engine. FlatBuffers shines for the archive/export path. |
| Electron companion app | Not shipped | It would double the memory budget of the whole product. The CLI plus Qt HUD covers both audiences. |
| Cursor position as a first-class feature | Captured and restored, but Wayland reports it unsupported | No protocol permits warping the pointer. |
| "Sub-second" as the only performance target | Explicit budgets per phase in `README.md` and a `contextsnap bench` command | "Sub-second" hides a 900 ms browser stall. Budgets are testable. |

## 6. What v0.1 must prove

1. Capture 40 windows and 200 tabs in under 400 ms, warm.
2. Restore that context with >= 90% of windows landing within 2 px on X11 and
   Windows.
3. Zero secrets in the database after visiting a password-reset link, verified
   by a test that greps the SQLite file.
4. Daemon RSS under 30 MB after 1,000 capture cycles (no leak).
5. `contextsnap doctor` correctly diagnoses a missing native-host manifest, a
   missing macOS permission and a Wayland session.

# Roadmap

The phases below follow the project proposal, with the scope tightened where
the platform research (see `docs/proposal-analysis.md`) showed a claim was not
achievable. Dates are relative weeks, not calendar promises.

## Phase 1 - Core daemon and HAL (weeks 1-4)

**Goal: capture is real on Windows and macOS.**

- [x] Core types, `Result<T>`, JSON, logging, config
- [x] Platform HAL interface with declared capabilities
- [x] Windows backend (enumeration, frame bounds, DPI, placement, cursor)
- [x] macOS backend (CoreGraphics + Accessibility, TCC prompts)
- [x] Null/headless backend for CI
- [x] SQLite storage, migrations, repository
- [x] IPC framing, server, client, daemon skeleton
- [x] CLI `save` / `list` / `show`

**Exit criteria:** capture 40 windows in under 120 ms with tabs disabled;
daemon idles under 25 MB RSS.

## Phase 2 - Browser integration (weeks 5-7)

**Goal: tabs are captured and restored on Chromium and Firefox.**

- [x] MV3 extension with native messaging port and reconnect
- [x] Native host relay and daemon browser endpoint
- [x] Tab-to-window correlation (geometry then title)
- [x] Session-file fallback (SNSS, `recovery.jsonlz4`)
- [x] Lazy tab restoration
- [ ] Publish to the Chrome Web Store and addons.mozilla.org
- [ ] Tab group restoration on Chromium (titles are already captured)
- [ ] Container/`cookieStoreId` fidelity on Firefox

**Exit criteria:** 200 tabs captured in under 400 ms warm; restore of 200 tabs
keeps the machine responsive (lazy by default).

## Phase 3 - GUI and daily-driver ergonomics (weeks 8-10)

**Goal: the tool is faster than doing it by hand.**

- [x] Qt 6 / QML shell, tray icon, quick switcher, inspector
- [ ] Global hotkey registration per platform
      (`RegisterHotKey`, `NSEvent` global monitor, X11 `XGrabKey`, portal on
      Wayland)
- [ ] Window thumbnails behind a capability gate (needs Screen Recording on
      macOS, a compositor protocol on Wayland)
- [ ] Onboarding flow that walks through permissions and the extension
- [ ] Snapshot diff view in the GUI (the CLI already has `diff`)

**Exit criteria:** HUD visible within 100 ms of the hotkey; GUI under 15 MB RSS.

## Phase 4 - Selective restore and Linux parity (weeks 11-14)

**Goal: restore what you want, and make X11 a first-class tier.**

- [x] Plan/execute split with dry run
- [x] Selectors (`app:`, `tab:`, `monitor:`, `workspace:`)
- [x] X11 backend with EWMH placement, stacking and virtual desktops
- [x] Wayland enumeration with honest capability reporting
- [ ] KWin D-Bus and GNOME Shell backends to recover placement on Wayland
- [ ] Per-application restore profiles ("never launch Slack automatically")
- [ ] Conflict resolution UI when a live window blocks a restore

**Exit criteria:** >= 90% of windows land within 2 px on X11 and Windows.

## Phase 5 - Privacy, CLI polish, security audit (weeks 15-16)

**Goal: safe to recommend to a stranger.**

- [x] URL sanitizer with entropy detection and host policies
- [x] Optional SQLCipher with OS keychain storage
- [x] `contextsnap doctor` capability and permission diagnostics
- [ ] Third-party review of the parsers (SNSS, mozlz4, IPC frames) with a
      fuzzing harness
- [ ] Reproducible builds and signed release binaries (Authenticode, notarized
      macOS bundle)
- [ ] `contextsnap bench` published numbers per platform

**Exit criteria:** a test that greps the database for known secrets after
visiting a reset link passes; fuzzers run clean for 24 h.

## Post-1.0 candidates

Ordered by expected value, not by ease.

1. **Editor and terminal integrations** - VS Code, Neovim and tmux know their
   own state far better than the OS does; a small plugin protocol would let
   them contribute buffers and panes to a snapshot.
2. **Launcher integrations** - Raycast, Alfred, Rofi and PowerToys Run all
   accept a JSON-speaking CLI, which already exists.
3. **Encrypted, user-controlled sync** - explicitly opt-in, end-to-end
   encrypted, and never a hosted service. Local-first stays the default
   (ADR-001).
4. **Automatic context detection** - cluster snapshots by app set and time of
   day, then suggest "you usually have these open on Monday morning".
5. **Session recording** - periodic lightweight snapshots so you can scrub
   backwards through the day; needs a storage budget story first.
6. **Windows virtual-desktop movement** - only if Microsoft documents the
   interface. We will not ship build-specific CLSID hacks (ADR-006).
7. **Flatpak and Snap packaging** - both sandbox window access, so this needs a
   portal story before it is useful.

## Explicit non-goals

- Cloud storage or accounts.
- Telemetry of any kind.
- Scroll position or in-page state (requires content-script access to every
  page).
- Restoring unsaved application state, which only the application can do.
- Driving browsers through remote-debugging ports.
- Private-API dependence for macOS Spaces or Windows desktops.

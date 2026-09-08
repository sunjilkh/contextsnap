# Architecture decision records

Short, dated, and reversible-with-cost. Each ADR states the decision, why the
alternatives lost, and what would make us revisit it.

## ADR-001: A long-lived daemon owns all state

**Status:** accepted

**Decision.** `contextsnapd` owns the HAL, the database and the browser
connection. The CLI and GUI are clients that speak a local socket protocol.

**Why.** Permissions (macOS TCC), COM apartments (Windows virtual desktops),
X11 connections and the browser's native-messaging port are all expensive or
impossible to establish per-invocation. A hotkey must produce a HUD in one
frame, which rules out cold-starting a process that then asks for permissions.

**Alternatives rejected.**
* *Pure CLI, no daemon.* Every capture would re-request permissions, re-open
  the browser channel and re-open SQLite; the browser channel in particular
  cannot be established at all, because the browser starts the native host, not
  us.
* *GUI owns state.* Then a headless server or a scripted workflow could not
  capture anything, and quitting the GUI would stop scheduled captures.

**Revisit if** we ever drop browser integration and scheduled captures, which
would make a daemon unnecessary.

## ADR-002: Length-prefixed JSON is the reference IPC, protobuf stays optional

**Status:** accepted

**Decision.** The wire format is a 4-byte little-endian length prefix followed
by a UTF-8 JSON object. `proto/contextsnap.proto` is maintained as the
normative service description and can be enabled with
`-DCONTEXTSNAP_WITH_PROTOBUF=ON`, but no build requires it.

**Why.** It is the same framing the browsers already impose on native
messaging, so one `FrameReader` serves both transports. It is debuggable with
`socat` and `jq`, it adds zero build dependencies, and at our message sizes
(a few KB) the encoding cost is irrelevant next to the OS calls.

**Alternatives rejected.**
* *Protobuf/Cap'n Proto everywhere.* Adds a code generator to every build for a
  protocol that is not on a hot path.
* *Plain newline-delimited JSON.* Breaks the moment a payload contains a raw
  newline and makes partial reads ambiguous.

**Revisit if** we add a streaming event firehose or an out-of-process HAL
where per-message cost starts to matter.

## ADR-003: SQLite rows, not a serialized blob, are the source of truth

**Status:** accepted

**Decision.** Snapshots are normalized across `snapshots`, `monitors`,
`windows`, `tabs` and `cursor_states`. FlatBuffers and JSON are export formats.

**Why.** The product needs search ("the snapshot with the Figma tab"),
retention pruning, partial reads and cheap metadata listing. Those are queries,
and SQLite already has a query planner, transactions and a crash-safe WAL.
Storing one blob per snapshot would push all of that into hand-written code.

**Alternatives rejected.**
* *One FlatBuffers file per snapshot.* Fast to load whole, useless to search,
  and pruning becomes a directory walk.
* *A key-value store.* Same problem plus a dependency.

**Revisit if** snapshots grow to include thumbnails; large binaries belong in
content-addressed files next to the database, not in it.

## ADR-004: Browser tabs come from an extension, with session files as fallback

**Status:** accepted

**Decision.** A minimal MV3 extension answers `collect_tabs` and
`restore_tabs` over native messaging via a relay host. If the extension is
absent, ContextSnap parses on-disk session files read-only (Chromium SNSS,
Firefox `recovery.jsonlz4`) and marks the snapshot's tab source accordingly.

**Why.** There is no OS-level way to read tab state. The extension is the only
supported interface, and it is also the only way to *restore* tabs. The
fallback exists so a fresh install produces something useful before the user
installs anything, and so restore-to-a-new-machine can still show what was
open.

**Alternatives rejected.**
* *DevTools protocol / remote debugging port.* Requires launching the browser
  with a debugging flag, which weakens the browser's own security posture and
  is a plausible attack surface for any local process.
* *UI automation.* Fragile and visibly moves the user's mouse.
* *Session files only.* Stale by up to the browser's flush interval and
  write-only in one direction: you cannot restore with them.

**Revisit if** browsers ever ship a stable local tab API.

## ADR-005: Qt 6 + QML for the optional GUI; the CLI is the baseline

**Status:** accepted

**Decision.** The GUI is a separate optional target
(`-DCONTEXTSNAP_BUILD_GUI=ON`) built with Qt 6 Quick. The CLI plus daemon is a
complete product on its own.

**Why.** The HUD must appear instantly and idle at a few MB, because it is a
tool that exists to save the user seconds. Qt renders on the GPU with a ~12 MB
footprint and links directly against the same C++ core, with no serialization
bridge. An Electron HUD would exceed the entire product's memory budget by 5x
and cold-start visibly slowly.

**Alternatives rejected.**
* *Electron.* Memory and startup cost, plus a second language runtime for a
  C++ project.
* *Tauri.* Much better than Electron, but still a webview per platform and a
  Rust toolchain added to a C++ build.
* *Native per-platform UI (WinUI/SwiftUI/GTK).* Three UIs to maintain for a
  single list-with-search surface.

**Revisit if** the UI grows into something document-like where web layout wins,
or if Qt licensing becomes a problem for a downstream distributor.

## ADR-006: Capabilities are declared, not discovered by failing

**Status:** accepted

**Decision.** Every HAL backend returns a `Capabilities` struct and a list of
human-readable `limitations`. Callers check capabilities before planning steps,
and `contextsnap doctor` prints them. Unsupported operations return
`ErrorCode::Unsupported`, never a silent success.

**Why.** The platform matrix is genuinely uneven: Wayland cannot place windows,
Windows cannot move a window to an arbitrary virtual desktop, macOS has no
public Spaces API and needs two separate consents for titles and control. A
platform layer that pretends to be uniform produces restores that quietly do
nothing, which is worse than an explicit "not supported here".

**Alternatives rejected.**
* *Try and catch.* Users cannot tell a permission problem from a bug, and the
  planner cannot reorder around a failure it did not anticipate.
* *Lowest common denominator.* Punishes X11 and Windows users for Wayland's
  security model.

**Revisit if** compositor scripting interfaces (KWin D-Bus, GNOME Shell) become
stable enough to promote to first-class backends, which would upgrade the
Wayland tier rather than remove the mechanism.

## ADR-007: Sanitize by default, encrypt on request

**Status:** accepted

**Decision.** URL sanitization is on by default and runs before anything is
written. At-rest encryption (SQLCipher) is opt-in via a build flag plus a
config switch, with the key in the OS keychain.

**Why.** A snapshot store is a URL store, and URLs carry OAuth codes,
pre-signed links, reset tokens and meeting passcodes. Sanitization is cheap,
lossless for normal browsing, and protects the common case (a plaintext file in
the user's home directory). Encryption is not on by default because it changes
the dependency footprint and creates a key-loss failure mode; the honest
default is "do not store secrets" rather than "store secrets encrypted".

**Alternatives rejected.**
* *Encrypt everything by default.* Bundles SQLCipher into every build and makes
  a lost keychain entry equal a lost snapshot history.
* *No sanitization, rely on encryption.* Encryption protects the file, not the
  user; the daemon still hands unsanitized URLs to clients and exports.

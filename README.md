<div align="center">

# ContextSnap

**Save your entire desktop context. Restore it in under a second. 100% local.**

Window layouts · browser tabs · running apps · virtual desktops · cursor & focus — captured as a single versioned snapshot.

[![CI](https://github.com/contextsnap/contextsnap/actions/workflows/ci.yml/badge.svg)](https://github.com/contextsnap/contextsnap/actions/workflows/ci.yml)
[![CodeQL](https://github.com/contextsnap/contextsnap/actions/workflows/codeql.yml/badge.svg)](https://github.com/contextsnap/contextsnap/actions/workflows/codeql.yml)
[![License: MIT OR Apache-2.0](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg)](#license)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)
![C++20](https://img.shields.io/badge/C%2B%2B-20-informational)

</div>

---

## Table of contents

- [Why ContextSnap](#why-contextsnap)
- [What it captures](#what-it-captures)
- [Quick start](#quick-start)
- [CLI tour](#cli-tour)
- [Architecture at a glance](#architecture-at-a-glance)
- [Repository layout](#repository-layout)
- [Performance budget](#performance-budget)
- [Platform support matrix](#platform-support-matrix)
- [Privacy & security](#privacy--security)
- [Building from source](#building-from-source)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Why ContextSnap

Every reboot, OS update, or task switch destroys something that took you 20 minutes to assemble: the *shape* of your work. Eleven windows across three monitors, 40 tabs in four browser windows, a terminal in the right working directory, a PDF on the second virtual desktop.

Today's tools each own one silo:

| Tool class | Captures | Misses |
| --- | --- | --- |
| Window managers (Rectangle, komorebi, i3) | geometry | tabs, processes, cursor, cross-app relationships |
| Browser session managers | tabs | everything outside the browser |
| OS "reopen windows" features | best effort, same-boot only | named/reusable states, selective restore, portability |
| Cloud workspace tools | metadata | your data leaves the machine |

**ContextSnap is one snapshot for the whole environment**, stored locally in SQLite, restorable in full or in part, with a documented on-disk format you can export, diff, and version.

### Design principles

1. **Local-first.** No account, no telemetry, no network calls at runtime. Snapshots never leave the machine unless you export them.
2. **Minimal overhead.** Idle daemon target: < 30 MB RSS, < 0.3% CPU. GUI target: < 15 MB RSS.
3. **Cross-platform, natively.** Win32, CoreGraphics/Accessibility, X11 and Wayland — behind one HAL, never a lowest-common-denominator layer.
4. **Privacy preserving.** URL/credential sanitization before persistence; optional SQLCipher (AES-256-GCM) at-rest encryption.
5. **Honest about limits.** Where the OS makes restoration impossible (hello, Wayland), we say so in [`docs/permissions.md`](docs/permissions.md) instead of silently failing.

---

## What it captures

```
snapshot
├── metadata        id (ULID), name, tags, created_at, platform, session type, capture duration
├── monitors[]      resolution, DPI, scale, orientation, virtual offset, primary flag, EDID hash
├── windows[]       geometry (extended frame bounds), state, z-order, workspace, focus, opacity
│   ├── process     exe path, argv, cwd, app id, single-instance heuristic
│   └── tabs[]      url, title, favicon, pinned/active/audible, discard state, tab group, container
└── cursor          x/y in virtual coordinates, owning monitor, focused window
```

Full field-by-field reference: [`docs/data-model.md`](docs/data-model.md).

---

## Quick start

```bash
# 1. Build (see docs/build.md for dependency bootstrap)
cmake --preset release
cmake --build --preset release

# 2. Start the daemon (foreground for a first run)
./build/release/bin/contextsnapd --foreground --log-level=info

# 3. Install the browser native-messaging host manifest
./scripts/install-native-host.sh            # macOS / Linux
powershell -File scripts/install-native-host.ps1   # Windows

# 4. Load the extension (unpacked) from ./extension, then:
contextsnap save --name "deep work" --tags focus,writing
contextsnap list
contextsnap restore --latest
```

Default hotkeys (configurable in `~/.config/contextsnap/config.toml`):

| Action | Shortcut |
| --- | --- |
| Quick Switcher HUD | <kbd>Ctrl/⌘</kbd> + <kbd>Shift</kbd> + <kbd>Space</kbd> |
| Save snapshot | <kbd>Ctrl/⌘</kbd> + <kbd>Shift</kbd> + <kbd>S</kbd> |
| Restore last snapshot | <kbd>Ctrl/⌘</kbd> + <kbd>Shift</kbd> + <kbd>R</kbd> |

---

## CLI tour

```bash
contextsnap save --name "code review" --tags work --no-tabs
contextsnap list --limit 20 --tag work --json
contextsnap show 01J8Z2QK9TMS3P4V6WCA7B0XYZ --tree
contextsnap restore 01J8Z... --only app:code,app:firefox --dry-run
contextsnap export 01J8Z... --format json --out ~/snapshots/review.json
contextsnap import ~/snapshots/review.json --rename "review (imported)"
contextsnap diff 01J8Z... 01J8Y... --format table
contextsnap doctor          # permissions, browser host, daemon health
contextsnap bench capture --iterations 50
```

Every command is documented in [`docs/cli.md`](docs/cli.md) and every command is scriptable: add `--json` for machine-readable output on stdout and human logs on stderr.

---

## Architecture at a glance

```
                        ┌──────────────────────────────┐
                        │  Qt 6 / QML client + tray    │  <15 MB RSS
                        │  Quick Switcher · Inspector  │
                        └──────────────┬───────────────┘
                                       │ IPC (length-prefixed frames)
                                       │ Unix domain socket / Named pipe
┌───────────────────┐   stdio frames   │
│ WebExtension (MV3)│◄────────────────►├── contextsnap-native-host
└───────────────────┘                  │
                        ┌──────────────▼───────────────┐
                        │        contextsnapd          │
                        │  SnapshotManager · Scheduler │
                        ├──────────────┬───────────────┤
            ┌───────────┤ Platform HAL │ Storage       ├───────────┐
            │           └──────────────┴───────────────┘           │
   ┌────────▼───────┐   ┌──────────────┐   ┌───────────────┐  ┌────▼─────────┐
   │ Win32          │   │ CoreGraphics │   │ X11 (xcb)     │  │ SQLite (WAL) │
   │ IVirtualDesktop│   │ AX API       │   │ Wayland/portal│  │ + SQLCipher  │
   └────────────────┘   └──────────────┘   └───────────────┘  └──────────────┘
```

Six components, one boundary each — details in [`docs/architecture.md`](docs/architecture.md), sequence diagrams in [`docs/diagrams/`](docs/diagrams).

---

## Repository layout

| Path | Contents |
| --- | --- |
| `include/contextsnap/` | Public headers: `core/`, `hal/`, `storage/`, `ipc/`, `browser/`, `privacy/` |
| `src/core/` | Snapshot model, capture/restore orchestration, matching, geometry, JSON, ULID |
| `src/hal/` | Platform backends: `windows/`, `macos/`, `linux/` (X11 + Wayland + portals) |
| `src/storage/` | SQLite schema/migrations, repository, JSON & FlatBuffers export |
| `src/ipc/` | Frame codec, server/client, request routing |
| `src/browser/` | Native messaging host, Chromium SNSS & Firefox `recovery.jsonlz4` parsers |
| `src/privacy/` | URL/credential sanitizer, key management, crypto |
| `src/daemon/` `src/cli/` `src/native_host/` | Executable entry points |
| `frontend/qt/` | Qt 6 + QML client (tray, HUD, inspector) |
| `extension/` | Cross-browser WebExtension + host manifests |
| `sql/` `schemas/` `proto/` | Storage migrations, FlatBuffers/JSON schemas, RPC IDL |
| `tests/` `benchmarks/` | Zero-dependency unit/integration tests, capture benchmarks |
| `packaging/` | systemd user unit, launchd plist, Polkit policy, MSIX/WiX notes |
| `docs/` | Architecture, data model, IPC, privacy, permissions, ADRs, roadmap, GTM |

---

## Performance budget

Budgets are enforced in CI by `benchmarks/bench_capture` against synthetic 40-window/200-tab fixtures.

| Metric | Target | Hard ceiling |
| --- | --- | --- |
| Capture (no tabs) | ≤ 120 ms | 250 ms |
| Capture (with 200 tabs) | ≤ 400 ms | 900 ms |
| Restore to first painted window | ≤ 300 ms | 800 ms |
| Daemon idle RSS | ≤ 22 MB | 30 MB |
| GUI idle RSS | ≤ 12 MB | 15 MB |
| Snapshot row size (40 windows) | ≤ 64 KB | 256 KB |

Methodology: [`docs/performance.md`](docs/performance.md).

---

## Platform support matrix

| Capability | Windows 10/11 | macOS 13+ | Linux/X11 | Linux/Wayland |
| --- | --- | --- | --- | --- |
| Enumerate windows + geometry | ✅ `EnumWindows` | ✅ `CGWindowList` + AX | ✅ `_NET_CLIENT_LIST` | ⚠️ `ext-foreign-toplevel-list-v1` (no geometry) |
| Restore geometry | ✅ `SetWindowPlacement` | ✅ AX `kAXPositionAttribute` | ✅ EWMH `_NET_MOVERESIZE_WINDOW` | ❌ compositor-dependent |
| Virtual desktops | ✅ `IVirtualDesktopManager` | ⚠️ Spaces (private API free path limited) | ✅ `_NET_WM_DESKTOP` | ⚠️ per-compositor |
| Cursor query/warp | ✅ | ✅ (TCC) | ✅ | ❌ (no global warp) |
| Focus / z-order | ✅ | ✅ (`kAXRaiseAction`) | ✅ | ⚠️ activation tokens |
| Browser tabs | ✅ | ✅ | ✅ | ✅ |

Legend: ✅ supported · ⚠️ partial/compositor-dependent · ❌ blocked by platform. Rationale and workarounds: [`docs/permissions.md`](docs/permissions.md).

---

## Privacy & security

- **Sanitize before persist.** OAuth tokens, `access_token`/`id_token`/`code` params, session IDs, `utm_*`/`gclid`/`fbclid` trackers, and userinfo in URLs are stripped or hashed by `privacy::Sanitizer` before a tab row is written. Rules are declarative and user-extensible.
- **Deny-list & scopes.** Domains, apps, and incognito windows can be excluded entirely; excluded items are never serialized, not merely hidden.
- **At rest.** Optional SQLCipher (AES-256-GCM, 256 KB KDF iterations) with the key sealed by DPAPI (Windows), Keychain (macOS), or libsecret/kwallet (Linux).
- **No ambient authority.** The daemon runs unprivileged in the user session; Polkit is used only for optional system-wide install actions.
- **Threat model & audit scope:** [`SECURITY.md`](SECURITY.md) and [`docs/privacy-security.md`](docs/privacy-security.md).

---

## Building from source

Requirements: CMake ≥ 3.24, a C++20 compiler (MSVC 19.36+, Clang 15+, GCC 12+), SQLite3, and — for the GUI — Qt 6.5+.

```bash
# vcpkg (recommended)
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release -DCONTEXTSNAP_BUILD_GUI=ON
cmake --build --preset release -j
ctest --preset release --output-on-failure

# Nix
nix develop     # then cmake --preset release
```

Core builds with **no third-party runtime dependency other than SQLite** — JSON, ULID, IPC framing, and the test harness are in-tree. Protobuf, FlatBuffers, SQLCipher, and OpenSSL are opt-in feature flags. Full matrix: [`docs/build.md`](docs/build.md).

---

## Roadmap

| Phase | Weeks | Deliverable |
| --- | --- | --- |
| 1 | 1–4 | Core daemon, C++ HAL, Win32 + macOS window serializers |
| 2 | 5–7 | WebExtension + native messaging pipeline, passive session fallback |
| 3 | 8–10 | Qt 6 UI, system tray, Quick Switcher overlay |
| 4 | 11–14 | Selective restoration engine, Linux/Wayland expansion |
| 5 | 15–16 | Privacy sanitizer, CLI, security audit, 1.0 release |

Exit criteria per phase: [`ROADMAP.md`](ROADMAP.md).

---

## Contributing

Good first issues are labeled and scoped; architectural changes go through the RFC template in [`.github/ISSUE_TEMPLATE/rfc.yml`](.github/ISSUE_TEMPLATE/rfc.yml). Read [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build/format/test loop and [`docs/adr/`](docs/adr) for the decisions already made (and why).

## License

Dual-licensed under either of

- MIT license ([`LICENSE-MIT`](LICENSE-MIT))
- Apache License, Version 2.0 ([`LICENSE-APACHE`](LICENSE-APACHE))

at your option. Contributions are accepted under the same dual license.

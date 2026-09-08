# Contributing to ContextSnap

Thanks for considering it. ContextSnap is a systems project: most of the value
is in getting operating-system details exactly right, so small, well-scoped
patches with a clear "I ran this on X" note are worth more than large rewrites.

## Ground rules

1. **Local-first is not negotiable.** No feature may require a network call.
   The daemon's systemd unit sets `PrivateNetwork=yes` and CI should keep
   passing with that in place.
2. **Never guess an OS API's behaviour.** If a call has a documented quirk
   (frame bounds, DPI awareness, coordinate origin), cite it in a comment.
3. **Declare capabilities, do not discover them by failing.** New abilities go
   into `hal::Capabilities` and are surfaced by `contextsnap doctor`
   (see ADR-006 in `docs/decisions.md`).
4. **Anything that touches URLs, titles or process arguments must pass through
   the sanitizer** before it is written to disk or handed to a client.
5. **Snapshots are user data.** Schema changes are forward-only migrations,
   never in-place edits of an existing migration.

## Getting set up

```bash
git clone https://github.com/contextsnap/contextsnap
cd contextsnap
scripts/dev-build.sh              # configure + build + test (Debug)
scripts/dev-build.sh --release    # optimized
scripts/dev-build.sh --gui        # adds the Qt 6 client
scripts/dev-build.sh --asan       # ASan + UBSan
```

Requirements: a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.36+), CMake 3.24+,
SQLite 3.35+. Everything else is optional and behind a CMake flag. On Linux
you also want the X11/XCB development packages listed in
`.github/workflows/ci.yml`.

No network? `-DCONTEXTSNAP_WITH_PROTOBUF=OFF -DCONTEXTSNAP_WITH_FLATBUFFERS=OFF`
is the default, and vcpkg is only bootstrapped when you ask for it with the
`vcpkg` preset.

## Repository map

| Path | What lives there |
| --- | --- |
| `include/contextsnap/` | Public headers. Changing one is an API change. |
| `src/core/` | Portable logic: types, JSON, matching, geometry, restore planning. No OS calls. |
| `src/hal/` | One directory per platform plus `common/` (null backend, factory). **All OS calls live here.** |
| `src/storage/` | SQLite access, migrations, serialization. |
| `src/browser/` | Tab bridge, native messaging, session-file parsers. |
| `src/privacy/` | URL sanitizer and key handling. |
| `src/ipc/` | Framing, protocol, client, server, transports. |
| `src/daemon/`, `src/cli/`, `src/native_host/` | The three binaries. |
| `frontend/qt/` | Optional Qt 6 / QML client. |
| `extension/` | MV3 browser extension. |
| `tests/unit/` | Microtest-based unit tests. |
| `sql/`, `schemas/`, `proto/` | Schema sources of truth. |

A good first change usually touches exactly one of these directories.

## Workflow

1. Open an issue first for anything larger than a bug fix - platform work often
   has a constraint that is not obvious (`docs/proposal-analysis.md` section 3
   lists the known ones).
2. Branch from `main`. Keep one logical change per PR.
3. Write or update a test. If the change is platform-specific and cannot be
   tested headlessly, say so in the PR and describe the manual verification.
4. Run `clang-format` (the repo `.clang-format` is authoritative) and
   `ctest --output-on-failure`.
5. Add a line to `CHANGELOG.md` under `## Unreleased`.
6. Fill in the PR template, including which OS you actually ran it on.

## Code style

* C++20, but conservatively: no `std::format` or `std::expected` yet, because
  GCC 11 is a supported compiler. Use `Result<T>` for anything fallible.
* Exceptions are not used for control flow. `Result<T>` / `Status` everywhere;
  the only `try` blocks are at OS or third-party boundaries.
* `snake_case` for functions and variables, `PascalCase` for types,
  `kPascalCase` for constants, trailing `_` for private members.
* 4 spaces, 100-column limit, `#pragma once`.
* Comments explain *why*. If a magic number came from an OS quirk, name the
  quirk.
* Prefer `std::string_view` parameters and `[[nodiscard]]` on anything
  returning `Result<T>`.

## Adding a platform backend

1. Implement `hal::Platform` in `src/hal/<platform>/`.
2. Fill in `capabilities()` honestly, including human-readable `limitations`.
3. Report unavailable OS permissions from `missing_permissions()`.
4. Register it in `src/hal/common/platform_factory.cpp`.
5. Normalize coordinates to a top-left-origin virtual desktop space; the
   geometry tests in `tests/unit/` pin that contract.
6. Add the compositor/OS to the table in `docs/proposal-analysis.md`.

## Adding an IPC method

1. Add the enum value and name mapping in `src/ipc/protocol.cpp`.
2. Register the handler in `src/daemon/daemon.cpp`.
3. Add a convenience wrapper to `ipc::Client` if clients will use it often.
4. Document it in `docs/ipc-protocol.md` and mirror it in
   `proto/contextsnap.proto`.
5. Unknown methods must keep returning `ProtocolError`; never change an
   existing method's params incompatibly - add a new one.

## Reporting security issues

Do not open a public issue. See `SECURITY.md`.

## Licensing

Contributions are accepted under the project's dual MIT OR Apache-2.0 license.
By opening a pull request you certify you have the right to submit the code
(Developer Certificate of Origin 1.1).

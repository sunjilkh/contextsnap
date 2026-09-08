## What this changes

<!-- One paragraph. Link the issue if there is one: Fixes #123 -->

## Why

## Platforms touched

- [ ] Windows (`src/hal/windows`)
- [ ] macOS (`src/hal/macos`)
- [ ] Linux / X11 (`src/hal/linux`)
- [ ] Linux / Wayland
- [ ] Portable core only (`src/core`, `src/storage`, `src/ipc`, `src/privacy`)
- [ ] Browser extension
- [ ] Docs / packaging only

## Verification

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

<!-- Paste the test summary. If you changed a HAL backend, say which OS you
     actually ran it on - CI can build all three but only exercises the null
     backend. -->

- [ ] Unit tests pass locally
- [ ] Added or updated tests for the behaviour I changed
- [ ] Manually verified on: <!-- e.g. Fedora 41 / GNOME 47 / Wayland -->

## Checklist

- [ ] `clang-format` clean (`.clang-format` is in the repo root)
- [ ] No new dependency, or the dependency is behind a CMake option and
      documented in `vcpkg.json`
- [ ] New OS capabilities are reflected in `hal::Capabilities` and surfaced by
      `contextsnap doctor` (see ADR-006)
- [ ] Anything that touches URLs, titles or process arguments still passes
      through the sanitizer before it is persisted
- [ ] Snapshot schema changes ship a forward-only migration in `sql/` **and**
      the mirrored constant in `src/storage/migrations.cpp`, with
      `kSnapshotSchemaVersion` bumped
- [ ] IPC changes are reflected in `docs/ipc-protocol.md` and
      `proto/contextsnap.proto`
- [ ] `CHANGELOG.md` updated under `## Unreleased`

## Screenshots or terminal output

<!-- GUI or CLI changes: a short asciinema/GIF or pasted output helps review. -->

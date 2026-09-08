# Security policy

## Reporting a vulnerability

Please report privately, not in a public issue:

* GitHub Security Advisories: **Security -> Report a vulnerability** on the
  repository (preferred - it gives us a private fork to patch in).
* Email: `security@contextsnap.dev`

Include the version (`contextsnap --version`), platform, and a reproduction if
you have one. You will get an acknowledgement within 72 hours, an assessment
within 7 days, and credit in the release notes unless you prefer otherwise.
We aim to ship a fix within 30 days for high-severity issues and coordinate
disclosure with you.

## Supported versions

Until 1.0, only the latest tagged release receives security fixes.

## Threat model

ContextSnap runs entirely as the logged-in user with no network access. The
assets worth protecting are:

| Asset | Where it lives | Protection |
| --- | --- | --- |
| Snapshot database (URLs, window titles, executable paths, argv) | `~/.local/share/contextsnap/snapshots.db` (`%LOCALAPPDATA%` on Windows, `~/Library/Application Support` on macOS) | `0600` file mode, `0700` directory, URL sanitization before write, optional SQLCipher AES-256 |
| Database encryption key | OS keychain (DPAPI / Keychain / Secret Service), or a `0600` fallback file | Never written next to the database; never logged |
| IPC socket | `$XDG_RUNTIME_DIR/contextsnap/` or `\\.\pipe\contextsnapd` | `0600` socket, peer uid check (`SO_PEERCRED`, `LOCAL_PEERCRED`, `GetNamedPipeClientProcessId`), same-user requirement on by default |
| Native messaging channel | stdio between browser and relay | Started by the browser, no listening socket; the relay forwards frames only |

### In scope

* Any local privilege escalation, e.g. a non-owner user reading snapshots or
  driving the daemon through the socket.
* Secrets surviving sanitization and reaching the database, logs or exports.
* Frame-parsing memory-safety bugs in IPC, native messaging or the session-file
  parsers (these parse untrusted, browser-controlled bytes).
* A restore that executes an attacker-chosen command line.
* Path traversal or symlink attacks in config, export and key-file handling.

### Out of scope

* An attacker who already has the user's uid: they can read the database
  directly, and encryption cannot fix that because the key must be available to
  the daemon.
* Malicious browser extensions - the browser's own permission model governs
  who may talk to the native host.
* Root or an admin on the machine.
* Window titles being visible in daemon logs at `debug` level (documented; do
  not paste debug logs into public issues).

## Hardening notes

* **No network.** No sockets are opened beyond the local IPC endpoint. The
  systemd unit sets `PrivateNetwork=yes` and `IPAddressDeny=any` so a future
  regression fails loudly.
* **Bounded parsing.** IPC frames are capped at 16 MiB and native messages at
  64 MiB, checked *before* allocation. The SNSS and mozlz4 parsers validate
  every length field against the remaining buffer and never trust the declared
  decompressed size.
* **Restore executes only captured argv.** Command lines come from the
  snapshot, are never passed through a shell, and are launched with the
  captured executable path rather than a `PATH` lookup.
* **Sanitize before persist.** `privacy::Sanitizer` runs inside the capture
  pipeline, so a client cannot request unsanitized storage.
* **Secrets never logged.** The logger has no access to raw URLs at `info`
  level, and keys are zeroed with `secure_zero` on destruction.

## Permissions ContextSnap asks for, and why

| Platform | Permission | Why | If denied |
| --- | --- | --- | --- |
| macOS | Accessibility | Move, resize, raise and focus windows | Capture still works; restore reports `PermissionDenied` |
| macOS | Screen Recording | Read window titles via CoreGraphics | Titles come from the Accessibility API instead, or are empty |
| Windows | none (user scope) | Win32 window APIs need no elevation | Elevated windows cannot be moved; reported per window |
| Linux/X11 | none | EWMH is available to any client on the display | - |
| Linux/Wayland | none available | No protocol permits placing other clients' windows | Placement is reported unsupported |
| Browser | `tabs`, `nativeMessaging` | Read tab URLs/titles, restore tabs | Tabs come from read-only session files or not at all |

ContextSnap never requests `<all_urls>` host permissions and ships no content
script, so it cannot read page contents.

---
name: Bug report
about: Something captured or restored incorrectly
title: "[bug] "
labels: [bug, needs-triage]
---

## What happened

<!-- One or two sentences. "Restoring moved my Chrome window 7 px left" is perfect. -->

## Expected

## Steps to reproduce

1. `contextsnap save --name repro`
2. ...
3. `contextsnap restore --latest`

## Diagnostics

Please paste the output of:

```bash
contextsnap doctor --json
contextsnap --version
```

<details>
<summary>doctor output</summary>

```json

```

</details>

## Environment

| Field | Value |
| --- | --- |
| OS + version | e.g. Fedora 41, Windows 11 23H2, macOS 15.1 |
| Session type | X11 / Wayland (+ compositor) / Win32 / Quartz |
| Monitors | e.g. 2x 1920x1080 @ 100% + 1x 3840x2160 @ 200% |
| Browser + version | e.g. Firefox 131 |
| Extension installed | yes / no |
| Install method | source / release binary / package |

## Logs

Run the daemon in the foreground with debug logging and attach the relevant
lines (redact anything private - logs can contain window titles):

```bash
contextsnapd --foreground --log-level debug
```

<details>
<summary>daemon log</summary>

```

```

</details>

## Checklist

- [ ] I confirmed the daemon is running (`contextsnap daemon status`)
- [ ] For tab issues: the extension shows "Connected to contextsnapd"
- [ ] I checked `docs/proposal-analysis.md` for a documented platform limitation

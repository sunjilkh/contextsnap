# IPC protocol (v1)

Clients talk to `contextsnapd` over a Unix domain socket (`$XDG_RUNTIME_DIR/contextsnap/contextsnapd.sock`
or `~/.local/state/contextsnap/contextsnapd.sock`) or a Windows named pipe
(`\\.\pipe\contextsnapd`). There is no TCP listener and no authentication
token: the socket is `0600` and the server additionally verifies the peer's uid
(`SO_PEERCRED` / `LOCAL_PEERCRED` / `GetNamedPipeClientProcessId`).

## Framing

```
+--------+--------+--------+--------+----------------------------+
| len[0] | len[1] | len[2] | len[3] |  UTF-8 JSON body (len B)   |
+--------+--------+--------+--------+----------------------------+
  little-endian uint32, body length only (prefix excluded)
```

* Maximum frame: 16 MiB (`ipc::kMaxFrameBytes`). Larger frames are rejected
  before allocation with `ProtocolError`.
* Exactly the same framing as Chrome/Firefox native messaging, so
  `ipc::FrameReader` is reused on both transports.
* Frames may be split across reads; `FrameReader::feed` buffers and
  `FrameReader::next` yields complete frames only.

## Message shapes

Request:

```json
{ "id": 7, "method": "capture_snapshot", "params": { "include_tabs": true } }
```

Response (success and failure):

```json
{ "id": 7, "ok": true, "result": { "snapshot": { "...": "..." } } }
{ "id": 7, "ok": false, "error": { "code": 1, "message": "snapshot not found" } }
```

Event (server-initiated, only after `subscribe`):

```json
{ "event": "snapshot_created", "payload": { "snapshot_id": "01J..." } }
```

A client distinguishes events from responses by the presence of the `event`
key; responses always carry `id`.

## Methods

| Method | Params | Result |
| --- | --- | --- |
| `hello` | - | `{ daemon, version, protocol, schema_version, platform, session_type }` |
| `capture_snapshot` | capture options (below) | `{ snapshot }` |
| `list_snapshots` | `{ limit, offset, tag, favorite_only, include_automatic, order }` | `{ snapshots: [summary] }` |
| `get_snapshot` | `{ snapshot_id }` (id prefix accepted) | `{ snapshot }` |
| `delete_snapshot` | `{ snapshot_id }` | `{ deleted: true }` |
| `rename_snapshot` | `{ snapshot_id, name }` | `{ renamed: true }` |
| `tag_snapshot` | `{ snapshot_id, tags: [string] }` | `{ tagged: true }` |
| `favorite_snapshot` | `{ snapshot_id, favorite }` | `{ favorite: bool }` |
| `search_snapshots` | `{ query, limit }` | `{ snapshots: [summary] }` |
| `plan_restore` | restore options | `{ plan }` |
| `restore_snapshot` | restore options | `{ report }` |
| `export_snapshot` | `{ snapshot_id, format }` | `{ format, document }` |
| `import_snapshot` | `{ document }` | `{ snapshot_id }` |
| `prune_snapshots` | `{ retention_days, max_snapshots, keep_favorites }` | `{ removed: n }` |
| `get_health` | - | health object (below) |
| `get_config` / `set_config` | config object | config object |
| `subscribe` / `unsubscribe` | `{ events: [string] }` | `{ subscribed: true }` |
| `shutdown` | - | `{ stopping: true }` |

### Capture params

```json
{
  "name": "Deep work",
  "tags": ["focus"],
  "include_tabs": true,
  "include_minimized": true,
  "include_cursor": true,
  "include_incognito": false,
  "include_all_workspaces": true,
  "sanitize_urls": true,
  "automatic": false,
  "browser_timeout_ms": 700,
  "excluded_app_ids": ["1password"],
  "excluded_url_patterns": ["*.bank.example"]
}
```

### Restore params

```json
{
  "snapshot_id": "01J8Z",
  "launch_missing_apps": true,
  "restore_tabs": true,
  "lazy_load_tabs": true,
  "restore_cursor": true,
  "restore_focus": true,
  "restore_z_order": true,
  "restore_workspaces": true,
  "close_conflicting_windows": false,
  "dry_run": false,
  "app_launch_timeout_ms": 8000,
  "window_settle_timeout_ms": 1500,
  "max_parallel_launches": 4,
  "selectors": [{ "kind": 1, "pattern": "code" }, "tab:*.figma.com"]
}
```

Selectors accept both the structured form and the CLI's textual form
(`app:`, `window:`, `tab:`, `monitor:`, `workspace:`, `all`).

### Health result

```json
{
  "version": "0.1.0",
  "endpoint": "/run/user/1000/contextsnap/contextsnapd.sock",
  "browser_endpoint": "/run/user/1000/contextsnap/contextsnapd.sock-browser",
  "database_ok": true,
  "platform_ok": true,
  "browser_bridge_connected": true,
  "database_bytes": 262144,
  "snapshot_count": 42,
  "platform": "linux",
  "session_type": "x11",
  "missing_permissions": [],
  "warnings": [],
  "limitations": ["wayland: absolute window placement unavailable"],
  "clients": 2
}
```

## Error codes

`0 Ok`, `1 NotFound`, `2 AlreadyExists`, `3 InvalidArgument`,
`4 PermissionDenied`, `5 Unsupported`, `6 IoError`, `7 DatabaseError`,
`8 SerializationError`, `9 ProtocolError`, `10 Timeout`, `11 Cancelled`,
`12 Conflict`, `13 PartialFailure`, `14 Internal`.

`PartialFailure` is the normal outcome of a restore where some windows could
not be matched; the `report` still carries per-item detail.

## Events

`snapshot_created`, `snapshot_deleted`, `snapshot_updated`,
`restore_started`, `restore_progress`, `restore_finished`,
`browser_connected`, `browser_disconnected`, `config_changed`,
`daemon_shutdown`.

## Talking to the daemon by hand

```bash
# 4-byte little-endian length prefix + body
printf '%s' '{"id":1,"method":"get_health","params":{}}' \
  | python3 -c 'import sys,struct;b=sys.stdin.buffer.read();sys.stdout.buffer.write(struct.pack("<I",len(b))+b)' \
  | socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/contextsnap/contextsnapd.sock" \
  | tail -c +5 | jq .
```

## Compatibility rules

1. `protocol` is bumped only for breaking changes; clients refuse a higher
   major version rather than guessing.
2. Unknown params are ignored, unknown methods return `ProtocolError`.
3 New result fields may be added at any time; clients must ignore extras.
4. `schema_version` in `hello` describes the snapshot format, not the wire
   protocol; the two version independently.

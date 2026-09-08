#!/usr/bin/env bash
# Install the ContextSnap native messaging host manifest for every browser
# found on this machine.
#
#   scripts/install-native-host.sh [--extension-id ID] [--host-path PATH]
#                                  [--firefox] [--chromium] [--uninstall]
#                                  [--dry-run]
#
# Chromium requires the extension origin (with trailing slash) in
# allowed_origins; Firefox requires the add-on id in allowed_extensions. This
# is the single most common installation failure, so `contextsnap doctor`
# re-checks the result.
set -euo pipefail

HOST_ID="com.contextsnap.native_host"
FIREFOX_EXTENSION_ID="contextsnap@contextsnap.dev"
EXTENSION_ID="REPLACE_WITH_EXTENSION_ID"
HOST_PATH=""
DO_FIREFOX=1
DO_CHROMIUM=1
UNINSTALL=0
DRY_RUN=0

die() { printf 'error: %s\n' "$1" >&2; exit 1; }
note() { printf '  %s\n' "$1"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --extension-id) EXTENSION_ID="${2:-}"; shift 2 ;;
    --host-path)    HOST_PATH="${2:-}"; shift 2 ;;
    --firefox)      DO_CHROMIUM=0; shift ;;
    --chromium)     DO_FIREFOX=0; shift ;;
    --uninstall)    UNINSTALL=1; shift ;;
    --dry-run)      DRY_RUN=1; shift ;;
    -h|--help)      sed -n '2,14p' "$0"; exit 0 ;;
    *)              die "unknown argument: $1" ;;
  esac
done

# Locate the relay binary: explicit flag, then PATH, then a local build tree.
if [[ -z "$HOST_PATH" ]]; then
  if command -v contextsnap-native-host >/dev/null 2>&1; then
    HOST_PATH="$(command -v contextsnap-native-host)"
  else
    for candidate in \
      build/release/contextsnap-native-host \
      build/debug/contextsnap-native-host \
      build/ci-linux/contextsnap-native-host \
      build/ci-macos/contextsnap-native-host; do
      if [[ -x "$candidate" ]]; then HOST_PATH="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"; break; fi
    done
  fi
fi

if [[ $UNINSTALL -eq 0 ]]; then
  [[ -n "$HOST_PATH" ]] || die "contextsnap-native-host not found; build it first or pass --host-path"
  [[ -x "$HOST_PATH" ]] || die "not executable: $HOST_PATH"
fi

case "$(uname -s)" in
  Darwin) PLATFORM=macos ;;
  Linux)  PLATFORM=linux ;;
  *)      die "unsupported platform: use the PowerShell installer on Windows" ;;
esac

chromium_dirs() {
  if [[ $PLATFORM == macos ]]; then
    local base="$HOME/Library/Application Support"
    printf '%s\n' \
      "$base/Google/Chrome" \
      "$base/Google/Chrome Beta" \
      "$base/Chromium" \
      "$base/Microsoft Edge" \
      "$base/BraveSoftware/Brave-Browser" \
      "$base/Vivaldi"
  else
    printf '%s\n' \
      "$HOME/.config/google-chrome" \
      "$HOME/.config/google-chrome-beta" \
      "$HOME/.config/chromium" \
      "$HOME/.config/microsoft-edge" \
      "$HOME/.config/BraveSoftware/Brave-Browser" \
      "$HOME/.config/vivaldi" \
      "$HOME/.var/app/com.google.Chrome/config/google-chrome" \
      "$HOME/.var/app/org.chromium.Chromium/config/chromium"
  fi
}

firefox_dirs() {
  if [[ $PLATFORM == macos ]]; then
    printf '%s\n' "$HOME/Library/Application Support/Mozilla"
  else
    printf '%s\n' \
      "$HOME/.mozilla" \
      "$HOME/.var/app/org.mozilla.firefox/.mozilla"
  fi
}

write_manifest() {
  local target="$1" flavour="$2"
  local dir
  dir="$(dirname "$target")"

  if [[ $UNINSTALL -eq 1 ]]; then
    if [[ -f "$target" ]]; then
      [[ $DRY_RUN -eq 1 ]] && note "would remove $target" || { rm -f "$target"; note "removed $target"; }
    fi
    return
  fi

  local allow
  if [[ $flavour == firefox ]]; then
    allow="\"allowed_extensions\": [\"$FIREFOX_EXTENSION_ID\"]"
  else
    allow="\"allowed_origins\": [\"chrome-extension://$EXTENSION_ID/\"]"
  fi

  if [[ $DRY_RUN -eq 1 ]]; then
    note "would write $target"
    return
  fi

  mkdir -p "$dir"
  cat > "$target" <<JSON
{
  "name": "$HOST_ID",
  "description": "ContextSnap tab bridge",
  "path": "$HOST_PATH",
  "type": "stdio",
  $allow
}
JSON
  chmod 0644 "$target"
  note "wrote $target"
}

found=0

if [[ $DO_CHROMIUM -eq 1 ]]; then
  echo "Chromium-family browsers:"
  while IFS= read -r profile_root; do
    [[ -d "$profile_root" ]] || continue
    found=$((found + 1))
    write_manifest "$profile_root/NativeMessagingHosts/$HOST_ID.json" chromium
  done < <(chromium_dirs)
  [[ $found -gt 0 ]] || note "none found"
fi

if [[ $DO_FIREFOX -eq 1 ]]; then
  echo "Firefox:"
  firefox_found=0
  while IFS= read -r mozilla_root; do
    [[ -d "$mozilla_root" ]] || continue
    firefox_found=$((firefox_found + 1))
    found=$((found + 1))
    write_manifest "$mozilla_root/native-messaging-hosts/$HOST_ID.json" firefox
  done < <(firefox_dirs)
  [[ $firefox_found -gt 0 ]] || note "none found"
fi

if [[ $found -eq 0 ]]; then
  die "no supported browser directories found; install a browser first or pass --host-path with --dry-run to inspect"
fi

if [[ $UNINSTALL -eq 0 && "$EXTENSION_ID" == "REPLACE_WITH_EXTENSION_ID" && $DO_CHROMIUM -eq 1 ]]; then
  cat <<'WARN'

Note: Chromium manifests were written with a placeholder extension id.
Load extension/ unpacked, copy the id from chrome://extensions and re-run:

  scripts/install-native-host.sh --extension-id <THE_ID>

Firefox needs no id: the add-on id is pinned in the manifest.
WARN
fi

echo
echo "Verify with: contextsnap doctor"

#!/usr/bin/env bash
# One-command developer build: configure, build, test, and print a summary.
#
#   scripts/dev-build.sh [--release] [--gui] [--asan] [--clean]
#                        [--no-tests] [--jobs N] [--filter SUBSTRING]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PRESET="debug"
WITH_GUI=0
WITH_ASAN=0
CLEAN=0
RUN_TESTS=1
JOBS=""
FILTER=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release)   PRESET="release"; shift ;;
    --gui)       WITH_GUI=1; shift ;;
    --asan)      WITH_ASAN=1; shift ;;
    --clean)     CLEAN=1; shift ;;
    --no-tests)  RUN_TESTS=0; shift ;;
    --jobs)      JOBS="${2:-}"; shift 2 ;;
    --filter)    FILTER="${2:-}"; shift 2 ;;
    -h|--help)   sed -n '2,7p' "$0"; exit 0 ;;
    *)           printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done

command -v cmake >/dev/null 2>&1 || { echo "cmake 3.24+ is required" >&2; exit 1; }

BUILD_DIR="build/$PRESET"
if [[ $WITH_GUI -eq 1 && $PRESET == "release" ]]; then
  PRESET="release-gui"
  BUILD_DIR="build/$PRESET"
fi

if [[ $CLEAN -eq 1 ]]; then
  echo "==> removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

EXTRA_ARGS=()
[[ $WITH_GUI -eq 1 ]] && EXTRA_ARGS+=(-DCONTEXTSNAP_BUILD_GUI=ON)
[[ $WITH_ASAN -eq 1 ]] && EXTRA_ARGS+=(-DCONTEXTSNAP_ENABLE_SANITIZERS=ON)

echo "==> configure ($PRESET)"
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
  cmake --preset "$PRESET" "${EXTRA_ARGS[@]}"
else
  cmake --preset "$PRESET"
fi

echo "==> build"
if [[ -n "$JOBS" ]]; then
  cmake --build --preset "$PRESET" --parallel "$JOBS"
else
  cmake --build --preset "$PRESET"
fi

if [[ $RUN_TESTS -eq 1 ]]; then
  echo "==> test"
  # The null HAL keeps tests display-independent.
  export CONTEXTSNAP_HEADLESS=1
  export CONTEXTSNAP_HOST="dev-$(hostname -s 2>/dev/null || echo local)"
  if [[ -n "$FILTER" ]]; then
    ctest --preset "$PRESET" --output-on-failure -R "$FILTER"
  else
    ctest --preset "$PRESET" --output-on-failure
  fi
fi

echo
echo "==> artifacts in $BUILD_DIR"
for binary in contextsnap contextsnapd contextsnap-native-host contextsnap-gui; do
  if [[ -x "$BUILD_DIR/$binary" ]]; then
    size="$(du -h "$BUILD_DIR/$binary" | cut -f1)"
    printf '    %-26s %s\n' "$binary" "$size"
  fi
done

cat <<EOF

Next steps:
    $BUILD_DIR/contextsnapd --foreground --log-level debug   # run the daemon
    $BUILD_DIR/contextsnap doctor                            # check capabilities
    $BUILD_DIR/contextsnap save --name "first snapshot"
    $BUILD_DIR/contextsnap list
    scripts/install-native-host.sh                           # enable tab capture
EOF

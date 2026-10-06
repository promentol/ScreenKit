#!/usr/bin/env bash
# Build screenkit-host.app for the tvOS simulator.
#   runtime/scripts/build-tvos-simulator.sh [extra cmake args...]
#
# SCREENKIT_APP_PKG=<app.skpkg> embeds a package (`screenkit bundle` output),
# which the app runs instead of the built-in fixtures. Unset, it runs them.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="$ROOT/build/tvos-simulator"

if [[ -n "${SCREENKIT_APP_DIR:-}" ]]; then
  echo "SCREENKIT_APP_DIR is gone: build a package and pass SCREENKIT_APP_PKG=<app.skpkg> instead" >&2
  exit 1
fi
APP_PKG="${SCREENKIT_APP_PKG:-}"
if [[ -n "$APP_PKG" ]]; then
  [[ -d "$APP_PKG" ]] || { echo "SCREENKIT_APP_PKG=$APP_PKG is not a directory" >&2; exit 1; }
  # Absolute, because CMake resolves a relative path against the build tree.
  APP_PKG="$(cd "$APP_PKG" && pwd)"
fi

node "$REPO/tools/prebuilts/fetch.mjs" --dep hermes apple-tvos-simulator-arm64 >/dev/null

# The generator, toolchain settings and tree are the `tvos-simulator` preset's
# (CMakePresets.json); only the embedded package is chosen here.
cmake -S "$ROOT" --preset tvos-simulator -DSCREENKIT_APP_PKG="$APP_PKG" "$@"
cmake --build "$BUILD"

echo
echo "built: $BUILD/screenkit-host.app"
echo "run:   runtime/scripts/run-tvos-simulator.sh"

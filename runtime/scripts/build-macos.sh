#!/usr/bin/env bash
# Build the runtime and screenkit-host for macOS.
#   runtime/scripts/build-macos.sh [extra cmake args...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="$ROOT/build/macos"

node "$REPO/tools/prebuilts/fetch.mjs" --dep hermes >/dev/null

# The generator, build type and tree are the `macos` preset's (CMakePresets.json).
cmake -S "$ROOT" --preset macos "$@"
cmake --build "$BUILD"

echo
echo "built: $BUILD/screenkit-host"
echo "try:   $BUILD/screenkit-host $BUILD/fixtures/hello.hbc"

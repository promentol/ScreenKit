#!/usr/bin/env bash
# Build the ANGLE hello-world app for the tvOS simulator.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tvos-simulator"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=tvOS \
  -DCMAKE_OSX_SYSROOT=appletvsimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0

cmake --build "$BUILD"

APP="$BUILD/angle_hello.app"
codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1 || true
echo
echo "built: $APP"

#!/usr/bin/env bash
# Build the app for the iOS or tvOS simulator.
#   scripts/build.sh [ios|tvos]
set -euo pipefail

PLATFORM="${1:-ios}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$PLATFORM" in
  ios)  SYSNAME="iOS";  SYSROOT="iphonesimulator";  DEPLOY="16.0" ;;
  tvos) SYSNAME="tvOS"; SYSROOT="appletvsimulator"; DEPLOY="16.0" ;;
  *) echo "usage: $(basename "$0") [ios|tvos]" >&2; exit 2 ;;
esac

BUILD="$ROOT/build/$PLATFORM"
SDL_SRC="$ROOT/build/sdl3-src"

# One SDL checkout shared by both platforms, but a separate *build* tree each --
# FETCHCONTENT_BASE_DIR would share _deps/sdl3-build too, and the two platforms
# would silently clobber each other's libSDL3.a. The tag is read from
# CMakeLists.txt so the pin stays defined in exactly one place.
if [[ ! -d "$SDL_SRC/.git" ]]; then
  SDL_TAG="$(sed -n 's/.*GIT_TAG[[:space:]]*\(release-[0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
  echo "cloning SDL3 $SDL_TAG ..."
  git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$SDL_SRC"
fi

# Ninja, not Xcode: SDL3 runs ~200 check_symbol_exists probes at configure time
# and the Xcode generator spawns a whole xcodebuild for each one, turning a
# 40-second configure into a 20-minute one.
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME="$SYSNAME" \
  -DCMAKE_OSX_SYSROOT="$SYSROOT" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$SDL_SRC"

cmake --build "$BUILD"

APP="$BUILD/sdlhello.app"
[[ -d "$APP" ]] || { echo "build succeeded but $APP was not found" >&2; exit 1; }

# Xcode ad-hoc signs simulator builds for you; Ninja doesn't, and simctl wants
# a signature on the bundle.
codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1

echo
echo "built: $APP"

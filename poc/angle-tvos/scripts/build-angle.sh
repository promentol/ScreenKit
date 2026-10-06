#!/usr/bin/env bash
# Build ANGLE's libEGL + libGLESv2 for the tvOS simulator (arm64).
#   poc/angle-tvos/scripts/build-angle.sh [simulator|device]
set -euo pipefail

ENVIRONMENT="${1:-simulator}"
ANGLE_ROOT="${ANGLE_ROOT:-$HOME/.screenkit/angle}"
export PATH="$ANGLE_ROOT/depot_tools:$PATH"
cd "$ANGLE_ROOT/angle"

OUT="out/tvos-$ENVIRONMENT"

# target_platform="tvos" is a real Chromium GN value (build/config/apple/mobile_config.gni)
# and selects the AppleTVSimulator/AppleTVOS SDK in build/config/ios/ios_sdk.gni.
# use_blink=true is forced only to satisfy that file's tvOS assert -- ANGLE never uses Blink.
GN_ARGS="target_os=\"ios\"
target_platform=\"tvos\"
target_environment=\"$ENVIRONMENT\"
target_cpu=\"arm64\"
use_blink=true
is_debug=false
is_component_build=false
ios_enable_code_signing=false
angle_enable_metal=true
angle_enable_vulkan=false
angle_build_tests=false
angle_enable_gl=false"

echo ">>> gn gen $OUT"
gn gen "$OUT" --args="$(echo "$GN_ARGS" | tr '\n' ' ')"

echo ">>> building libEGL + libGLESv2"
autoninja -C "$OUT" libEGL libGLESv2

echo
find "$OUT" -maxdepth 1 \( -name 'libEGL*' -o -name 'libGLESv2*' \) -print0 | xargs -0 ls -la

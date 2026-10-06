#!/usr/bin/env bash
# Boot a simulator, install the app and launch it with the console attached.
#   scripts/run.sh [ios|tvos] ["Device Name"]
set -euo pipefail

PLATFORM="${1:-ios}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_ID="com.example.sdlhello"

case "$PLATFORM" in
  ios)  DEVICE="${2:-iPhone 17 Pro}" ;;
  tvos) DEVICE="${2:-Apple TV 4K (3rd generation)}" ;;
  *) echo "usage: $(basename "$0") [ios|tvos] [device name]" >&2; exit 2 ;;
esac

APP="$ROOT/build/$PLATFORM/sdlhello.app"
if [[ ! -d "$APP" ]]; then
  echo "no build found -- run: scripts/build.sh $PLATFORM" >&2
  exit 1
fi

UDID="$(xcrun simctl list devices available | grep -m1 -F "$DEVICE (" | sed -E 's/.*\(([0-9A-Fa-f-]{36})\).*/\1/')"
if [[ -z "$UDID" ]]; then
  echo "no available simulator named '$DEVICE'." >&2
  if [[ "$PLATFORM" == "tvos" ]]; then
    echo "the tvOS simulator runtime is probably not installed. install it with:" >&2
    echo "  xcodebuild -downloadPlatform tvOS" >&2
  fi
  exit 1
fi

echo "device: $DEVICE ($UDID)"
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
open -a Simulator --args -CurrentDeviceUDID "$UDID"

xcrun simctl uninstall "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP"
echo "launching $BUNDLE_ID -- ctrl-c to detach"
exec xcrun simctl launch --console-pty "$UDID" "$BUNDLE_ID"

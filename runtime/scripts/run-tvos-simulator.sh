#!/usr/bin/env bash
# Install screenkit-host.app on a tvOS simulator, launch it and assert the app
# actually ran. Exits non-zero unless the expected lines reach the platform log,
# so this is usable as a CI gate rather than just a launcher.
#
# Two modes, chosen by what the app embeds:
#
#   triangle  (no app.skpkg) The needle is the GL_RENDERER line the *bundle*
#             logs, not the one the native GlSurface logs: it is only reachable
#             through `gl.getString(gl.RENDERER)`, so matching it proves the
#             whole M4 path -- ANGLE's Metal backend over SDL's CAMetalLayer,
#             the gl.def entry points, and JS calling them.
#
#   package   (built with SCREENKIT_APP_PKG) The needle is the host's
#             `package <dir>: runtimeVersion ...` line, which it logs only once
#             the embedded app.skpkg passed the manifest gate -- so the lookup
#             cannot silently fall back to a fixture -- and the package's `dir`
#             must be the embedded app.skpkg. It also waits for the packed
#             script's `screenkit bundle: entry "<entry>" ready`, logged once the
#             entry module and its imports have executed, so an entry that
#             rejects after the first frames does not pass. A failure the app
#             reports, or a refused package, fails the run at once -- and the
#             app must then have exited on its own.
#
# Both then wait for `frame time`, which says frames kept arriving.
#
#   runtime/scripts/run-tvos-simulator.sh [device-udid-or-name]
#
# A screenshot is saved while it runs (the path is printed).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/tvos-simulator/screenkit-host.app"
BUNDLE_ID="dev.screenkit.host"
FRAME_NEEDLE="frame time"
DEADLINE=60

[[ -d "$APP" ]] || { echo "not built: $APP -- run build-tvos-simulator.sh" >&2; exit 1; }

# The app is only meaningful if it carries both what it runs and the framework
# it links; without either it launches and logs a failure instead.
if [[ -f "$APP/app.skpkg/manifest.json" ]]; then
  MODE=package
  # The host logs the package's directory as it resolved it, inside the
  # installed .app -- so match the tail, which only the embedded one has.
  NEEDLE="screenkit-host.app/app.skpkg: runtimeVersion"
  READY_PATTERN='screenkit bundle: entry ".*" ready'
  SHOT_NAME=package
elif [[ -d "$APP/app.skpkg" ]]; then
  echo "$APP/app.skpkg has no manifest.json -- rebuild with SCREENKIT_APP_PKG=<app.skpkg>" >&2
  exit 1
else
  MODE=triangle
  NEEDLE="triangle: GL_RENDERER = ANGLE"
  READY_PATTERN=''
  SHOT_NAME=triangle
  [[ -f "$APP/triangle.hbc" ]] || { echo "missing $APP/triangle.hbc" >&2; exit 1; }
fi
# A line that ends the wait early: the app failed and is not coming back. Each
# is the host's own message, anchored at its level and first words, so an app's
# console output that merely mentions a manifest does not match.
FAIL_PATTERN='\[error\] (cannot open package manifest "|package manifest "|Hermes bytecode version mismatch in "|runtime version mismatch in "|package entry "|the app reported a fatal failure; exiting with )'
[[ -x "$APP/Frameworks/hermesvm.framework/hermesvm" ]] || {
  echo "missing $APP/Frameworks/hermesvm.framework" >&2; exit 1; }
echo "mode: $MODE"

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
  # `|| true` so a no-match does not kill the script under `set -e` before the
  # message below can explain what happened.
  DEVICE="$(xcrun simctl list devices available --json \
    | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{
        const j=JSON.parse(s);
        for (const [rt,ds] of Object.entries(j.devices))
          if (rt.includes("tvOS")) for (const d of ds) { console.log(d.udid); process.exit(0); }
        process.exit(1);
      })' || true)"
  [[ -n "$DEVICE" ]] || { echo "no tvOS simulator available" >&2; exit 1; }
fi

xcrun simctl boot "$DEVICE" 2>/dev/null || true
xcrun simctl bootstatus "$DEVICE" -b >/dev/null
xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$DEVICE" "$APP"

# simctl launch prints "<bundle-id>: <pid>". Keying on that pid is what makes
# this an assertion rather than a coincidence: a --start timestamp is subject to
# the simulator's clock and timezone, and a stale line from an earlier run would
# satisfy the check while this launch did nothing at all.
LAUNCH="$(xcrun simctl launch "$DEVICE" "$BUNDLE_ID")"
PID="${LAUNCH##*: }"
[[ "$PID" =~ ^[0-9]+$ ]] || { echo "could not read pid from: $LAUNCH" >&2; exit 1; }
echo "launched $BUNDLE_ID as pid $PID"

# Poll the platform log rather than streaming: `simctl launch --console-pty`
# never returns for a UIApplication, so a stream has no natural end.
#
# Two needles, because they prove different halves. The GL_RENDERER line says
# the bundle reached ANGLE's Metal backend through the gl entry points; the
# frame-time line says frames kept arriving afterwards, which a bundle that
# drew once and stalled would not produce. The frame-time line only appears
# about a second in, so it is also the thing worth waiting for.
SHOT="$ROOT/build/tvos-simulator/$SHOT_NAME.png"
LOG=""
deadline=$((SECONDS + DEADLINE))
while (( SECONDS < deadline )); do
  LOG="$(xcrun simctl spawn "$DEVICE" log show --style compact \
           --predicate "subsystem == \"dev.screenkit\" AND processIdentifier == $PID" \
           --last "${DEADLINE}s" 2>/dev/null || true)"
  if fail="$(grep -m1 -oE "$FAIL_PATTERN" <<<"$LOG")"; then
    echo "$LOG" >&2
    # The host ends the app itself on a failure; give it ~2 s to have done so
    # before this script does, so a host that only logs is caught.
    exited=0
    for _ in $(seq 1 20); do
      if ! kill -0 "$PID" 2>/dev/null; then exited=1; break; fi
      sleep 0.1
    done
    if (( ! exited )); then
      xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || true
      echo "FAILED: pid $PID reported a failure but did not exit (\"$fail\", $MODE mode)" >&2
      exit 1
    fi
    echo "FAILED: \"$fail\" in the platform log from pid $PID, which then exited ($MODE mode)" >&2
    exit 1
  fi
  if grep -qF "$NEEDLE" <<<"$LOG" && grep -qF "$FRAME_NEEDLE" <<<"$LOG" &&
     { [[ -z "$READY_PATTERN" ]] || grep -qE "$READY_PATTERN" <<<"$LOG"; }; then
    # While it is still running and still drawing.
    xcrun simctl io "$DEVICE" screenshot "$SHOT" >/dev/null 2>&1 || true
    echo "$LOG"
    xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || true
    echo
    echo "ok ($MODE mode): \"$NEEDLE\" reached the platform log from pid $PID on $DEVICE"
    grep -F "$FRAME_NEEDLE" <<<"$LOG" | tail -3
    [[ -f "$SHOT" ]] && echo "screenshot: $SHOT"
    exit 0
  fi
  sleep 2
done

echo "${LOG:-<no dev.screenkit log entries for pid $PID>}" >&2
xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || true
if [[ "$MODE" == package ]] && ! grep -qF "$NEEDLE" <<<"$LOG"; then
  echo "FAILED: the package line (\"$NEEDLE\") never appeared from pid $PID within ${DEADLINE}s -- the embedded app.skpkg was not opened" >&2
  exit 1
fi
if [[ -n "$READY_PATTERN" ]] && ! grep -qE "$READY_PATTERN" <<<"$LOG"; then
  echo "FAILED: the entry never logged ready from pid $PID within ${DEADLINE}s -- a package built before runtime 2's packer, or an entry that never settled" >&2
  exit 1
fi
echo "FAILED: \"$NEEDLE\" + \"$FRAME_NEEDLE\" did not both appear from pid $PID within ${DEADLINE}s ($MODE mode)" >&2
exit 1

#!/bin/sh
# Run screenkit-host --window on a package until its output shows `needle`, then
# stop it. A windowed host runs until its window closes, so this waits on the
# line rather than on the exit, under a deadline.
#
#   host-window-row.sh <screenkit-host> <package> <needle> [deadline-seconds] [open-seconds]
#
# With open-seconds, the host must still be running that long after the line
# appeared -- an app with nothing scheduled is waiting for input, not finished
# -- and must then exit 0 when asked to quit (SIGTERM, which SDL turns into a
# quit event).
#
# Exit 77 (a CTest skip) when the host could not open a window or a GL context
# at all (exit 71, no window server) rather than failing a machine that cannot
# show one. Any other early exit fails the row.
host=$1
target=$2
needle=$3
deadline=${4:-30}
open=${5:-}

log=$(mktemp "${TMPDIR:-/tmp}/host-window-row.XXXXXX")
trap 'rm -f "$log"' EXIT

"$host" --window "$target" >"$log" 2>&1 &
pid=$!

found=0
ticks=$((deadline * 5))
while [ "$ticks" -gt 0 ]; do
  if grep -qF -- "$needle" "$log"; then
    found=1
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.2
  ticks=$((ticks - 1))
done
grep -qF -- "$needle" "$log" && found=1

if [ "$found" -eq 1 ] && [ -n "$open" ]; then
  sleep "$open"
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid"
    rc=$?
    cat "$log"
    echo "host-window-row: the host exited $rc within ${open}s of the line; a window stays open until it is closed" >&2
    exit 1
  fi
  kill -TERM "$pid"
  ticks=25
  while [ "$ticks" -gt 0 ] && kill -0 "$pid" 2>/dev/null; do
    sleep 0.2
    ticks=$((ticks - 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid"
    wait "$pid" 2>/dev/null
    cat "$log"
    echo "host-window-row: the host did not quit within 5s of SIGTERM" >&2
    exit 1
  fi
  wait "$pid"
  rc=$?
  cat "$log"
  if [ "$rc" -ne 0 ]; then
    echo "host-window-row: asked to quit, the host exited $rc" >&2
    exit 1
  fi
  exit 0
fi

if kill -0 "$pid" 2>/dev/null; then
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rc=stopped
else
  wait "$pid"
  rc=$?
fi
cat "$log"

if [ "$found" -eq 1 ]; then
  if [ "$rc" != stopped ] && [ "$rc" -ne 0 ]; then
    echo "host-window-row: the line appeared, but the host exited $rc" >&2
    exit 1
  fi
  exit 0
fi
if [ "$rc" = 71 ]; then
  echo "host-window-row: the host could not open a window or GL context (exit $rc); skipping" >&2
  exit 77
fi
echo "host-window-row: \"$needle\" did not appear within ${deadline}s (host: $rc)" >&2
exit 1

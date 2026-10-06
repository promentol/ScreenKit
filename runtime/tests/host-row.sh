#!/bin/sh
# One host smoke row: run screenkit-host on a bundle or package and check both
# the exit status and the output.
#
#   host-row.sh <screenkit-host> [--within <seconds>] [--window] <bundle|package> <expected-exit> [needle...]
#
# A needle must appear in the output; a needle starting with `!` must not. Exit
# status AND output, because either alone passes a broken host: a refusal that
# printed the right words and exited 0, or a run that exited 0 without running.
#
# --within fails the row when the host took longer than that to exit -- for a
# host that must stop promptly rather than wait out its idle deadline.
#
# With --window, a host that could not open a window or a GL context at all
# (exit 71 where something else was expected) skips the row with 77, as
# host-window-row.sh does, rather than failing a machine with no window server.
# Exit 70 -- the runtime or the DOM prelude would not start -- is a failure.
host=$1
shift
within=
if [ "$1" = "--within" ]; then
  within=$2
  shift 2
fi
mode=
if [ "$1" = "--window" ]; then
  mode=--window
  shift
fi
target=$1
want=$2
shift 2

started=$(date +%s)
out=$("$host" $mode "$target" 2>&1)
rc=$?
elapsed=$(( $(date +%s) - started ))
printf '%s\n' "$out"

if [ -n "$mode" ] && [ "$rc" -ne "$want" ] && [ "$rc" -eq 71 ]; then
  echo "host-row: the host could not open a window or GL context (exit $rc); skipping" >&2
  exit 77
fi
if [ "$rc" -ne "$want" ]; then
  echo "host-row: exit status $rc, expected $want" >&2
  exit 1
fi
if [ -n "$within" ] && [ "$elapsed" -gt "$within" ]; then
  echo "host-row: the host took ${elapsed}s to exit, more than ${within}s" >&2
  exit 1
fi
for needle in "$@"; do
  case $needle in
    '!'*)
      if printf '%s' "$out" | grep -qF -- "${needle#!}"; then
        echo "host-row: output must not contain: ${needle#!}" >&2
        exit 1
      fi
      ;;
    *)
      if ! printf '%s' "$out" | grep -qF -- "$needle"; then
        echo "host-row: output is missing: $needle" >&2
        exit 1
      fi
      ;;
  esac
done

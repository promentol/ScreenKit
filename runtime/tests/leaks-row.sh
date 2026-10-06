#!/bin/sh
# One test row under leaks(1): the "no leak" half of a teardown criterion on
# Darwin, where Apple's ASan has no LeakSanitizer (`detect_leaks` is a no-op).
#
#   leaks-row.sh <screenkit-runtime-tests> <case> <fixtures-dir>
#
# The row must pass on its own, and leaks must find nothing it does not expect.
# Expected, and only this: ANGLE's per-thread EGL bookkeeping -- an egl::Thread
# (allocated in egl::GetCurrentThread) and a display TLS slot
# (egl::Display::InitTLS), 48 bytes each, for every thread that ever called EGL.
# ANGLE keeps them in thread-local storage with no destructor on Apple, so they
# outlive the thread; nothing ScreenKit owns can free them.
bin=$1
case=$2
fixtures=$3

report=$(mktemp "${TMPDIR:-/tmp}/leaks-row.XXXXXX")
trap 'rm -f "$report"' EXIT

# Stack logging is what names the allocation site, which is what the allowance
# below matches on.
MallocStackLogging=1 leaks --atExit -- "$bin" "$case" "$fixtures" >"$report" 2>&1
rc=$?

if grep -qF "== $case: skipped" "$report"; then
  grep -F "== $case" "$report"
  exit 77
fi
if ! grep -qF "== $case: ok ==" "$report"; then
  cat "$report"
  echo "leaks-row: $case itself failed" >&2
  exit 1
fi
# leaks: 0 no leaks, 1 leaks found, anything else it could not look.
if [ "$rc" -gt 1 ]; then
  cat "$report"
  echo "leaks-row: leaks could not inspect the process (exit $rc)" >&2
  exit 1
fi

grep -E "leaks for .* total leaked bytes" "$report"
unexpected=$(grep -E "ROOT (LEAK|CYCLE): <" "$report" |
  grep -vE "ROOT LEAK: <(egl::Thread|malloc in egl::GetCurrentThread\(\)|malloc in egl::Display::InitTLS\(\))[ >]")
if [ -n "$unexpected" ]; then
  cat "$report"
  echo "leaks-row: leaks beyond ANGLE's per-thread EGL state:" >&2
  printf '%s\n' "$unexpected" >&2
  exit 1
fi

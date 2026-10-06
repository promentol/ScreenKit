#!/usr/bin/env bash
# Fetch depot_tools + a standalone ANGLE checkout into the shared cache.
#   poc/angle-tvos/scripts/fetch-angle.sh
# Checkout lives outside the repo (multi-GB); override with ANGLE_ROOT.
set -euo pipefail

ANGLE_ROOT="${ANGLE_ROOT:-$HOME/.screenkit/angle}"
mkdir -p "$ANGLE_ROOT"

# --- depot_tools ------------------------------------------------------------
if [[ ! -d "$ANGLE_ROOT/depot_tools" ]]; then
  echo ">>> cloning depot_tools"
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$ANGLE_ROOT/depot_tools"
fi
export PATH="$ANGLE_ROOT/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=1

# A shallow clone ships no vendored python/ninja; without this `fetch` prints
# "python3_bin_reldir.txt not found" and then exits 0, so set -e never fires.
echo ">>> bootstrapping depot_tools"
"$ANGLE_ROOT/depot_tools/ensure_bootstrap"

# --- ANGLE ------------------------------------------------------------------
# --no-history keeps this to a fraction of a full checkout; we never need the log.
if [[ ! -d "$ANGLE_ROOT/angle/.git" ]]; then
  echo ">>> fetching ANGLE (no history)"
  mkdir -p "$ANGLE_ROOT/angle"
  cd "$ANGLE_ROOT/angle"
  fetch --no-history angle
else
  echo ">>> syncing existing ANGLE checkout"
  cd "$ANGLE_ROOT/angle"
  gclient sync --no-history -D
fi

# `fetch` reports success even when it did nothing -- verify the checkout landed.
if [[ ! -d "$ANGLE_ROOT/angle/.git" ]]; then
  echo "ERROR: ANGLE checkout missing after fetch -- see output above" >&2
  exit 1
fi

echo
echo "angle:       $ANGLE_ROOT/angle"
echo "depot_tools: $ANGLE_ROOT/depot_tools"
du -sh "$ANGLE_ROOT"/* 2>/dev/null || true

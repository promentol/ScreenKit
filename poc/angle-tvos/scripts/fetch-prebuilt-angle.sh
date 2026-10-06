#!/usr/bin/env bash
# Thin wrapper: prebuilt handling lives in tools/prebuilts for all platforms.
#   poc/angle-tvos/scripts/fetch-prebuilt-angle.sh [target-key]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec node "$ROOT/tools/prebuilts/fetch.mjs" --dep angle "${1:-apple-tvos-simulator-arm64}"

#!/bin/sh
# Compile scripts to SpiderMonkey stencils with screenkit-smc, in the container that
# holds the pinned libmozjs (tools/spidermonkey/Dockerfile) -- a stencil loads only
# in the engine build that wrote it.
#
#   sh tools/spidermonkey/smc.sh [--eager] <in.js> <out.stencil>
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DOCKER_BIN=/Applications/Docker.app/Contents/Resources/bin
[ -d "$DOCKER_BIN" ] && PATH="$PATH:$DOCKER_BIN"
eager=""
[ "${1:-}" = "--eager" ] && { eager="--eager"; shift; }
in_dir=$(cd "$(dirname "$1")" && pwd)
out_dir=$(mkdir -p "$(dirname "$2")" && cd "$(dirname "$2")" && pwd)
docker build -q --platform linux/arm64 -t screenkit-spidermonkey "$ROOT/tools/spidermonkey" >/dev/null
docker run --rm --platform linux/arm64 -v "$ROOT:/screenkit" -v "$in_dir:/in:ro" -v "$out_dir:/out" \
    screenkit-spidermonkey sh -c '
    set -e
    B=/screenkit/tools/spidermonkey/build/smc
    if ! [ -x $B/screenkit-smc ] || [ /screenkit/runtime/core/src/spidermonkey/SpiderMonkeyRuntime.cpp -nt $B/screenkit-smc ]; then
        cmake -S /screenkit/tools/spidermonkey/smc -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release >/tmp/configure.log \
            || { cat /tmp/configure.log; exit 1; }
        cmake --build $B >/dev/null
    fi
    $B/screenkit-smc $0 --url "$1" "/in/$1" "/out/$2"' "$eager" "$(basename "$1")" "$(basename "$2")"

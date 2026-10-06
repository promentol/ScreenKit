#!/bin/sh
# Hermes against SpiderMonkey on a Raspberry Pi: the same Phaser-shaped JavaScript
# (js/workload.js) through two small C++ hosts, measured the same way -- frame times, RSS and
# its peak, GC, CPU, startup. No graphics.
#
#   sh poc/engine-bench/bench.sh build              both hosts for linux-arm64 in Docker, staged in build/stage
#   sh poc/engine-bench/bench.sh local [config...]  run in an arm64 container on this machine -- a check, not a measurement
#   sh poc/engine-bench/bench.sh push               copy the staged tree to the Pi
#   sh poc/engine-bench/bench.sh run [config...]    run on the Pi, one config after another, into results/
#   sh poc/engine-bench/bench.sh compare            results/*.json -> results/summary.md
#   sh poc/engine-bench/bench.sh all                build, push, run
#   sh poc/engine-bench/bench.sh clean              remove it all from the Pi
#
# The configs are run.sh's: hermes-interp hermes-jit hermes-jit-force, and sm-interp sm-baseline
# sm-jit from source, from a lazy stencil (-lazy) or an eager one (-eager). All but
# hermes-jit-force and the Baseline stencils run by default.
#
#   PI=batocera.local                                 the Pi
#   BENCH_ARGS="--frames 60 --scenes sprites,ui"      options for every run (bench-* --help)
#   COOL=20                                           seconds between runs, for the SoC to cool
#   BULK_KB=1400                                      size of the generated library code
#   RESULTS=results/other                            where reports and summary.md go
#   ALLOW_THROTTLED=1                                 run on a Pi whose firmware reports throttling (the numbers mean little)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PI="${PI:-batocera.local}"
DEST=/userdata/engine-bench
BUILD="$HERE/build"
STAGE="$BUILD/stage"
RESULTS="${RESULTS:-$HERE/results}"
CONFIGS="hermes-interp hermes-jit sm-interp sm-baseline sm-jit sm-interp-lazy sm-jit-lazy sm-interp-eager sm-jit-eager"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@$PI"
DOCKER_BIN=/Applications/Docker.app/Contents/Resources/bin
[ -d "$DOCKER_BIN" ] && PATH="$PATH:$DOCKER_BIN"

cmd=${1:-}
[ $# -gt 0 ] && shift

say() { printf '>>> %s\n' "$*"; }

# The two Hermes libraries: the shipping interpreter build and the JIT build of the same
# commit (tools/prebuilts/recipes/hermes-linux; HERMES_JIT=2 for the second).
hermes_archives() {
    version=$(node -p "require('$ROOT/tools/prebuilts/manifest.json').hermes.version")
    HERMES_INTERP="$ROOT/tools/prebuilts/dist/hermes-linux-arm64-$version.tar.gz"
    HERMES_JIT="$ROOT/tools/prebuilts/dist/hermes-jit-linux-arm64-$version.tar.gz"
    [ -f "$HERMES_INTERP" ] || { echo "no $HERMES_INTERP: tools/prebuilts/recipes/hermes-linux/build.sh linux-arm64" >&2; exit 1; }
    [ -f "$HERMES_JIT" ] || { echo "no $HERMES_JIT: HERMES_JIT=2 tools/prebuilts/recipes/hermes-linux/build.sh linux-arm64" >&2; exit 1; }
}

extract() {
    archive=$1 dir=$2
    [ -f "$dir/lib/libhermesvm.so" ] && [ "$dir/lib/libhermesvm.so" -nt "$archive" ] && return
    rm -rf "$dir"
    mkdir -p "$dir"
    tar -xzf "$archive" -C "$dir" --strip-components 1
}

build() {
    hermes_archives
    extract "$HERMES_INTERP" "$BUILD/hermes-interp"
    extract "$HERMES_JIT" "$BUILD/hermes-jit"
    mkdir -p "$BUILD/js"
    node "$HERE/js/gen-bulk.mjs" "$BUILD/js/bulk.js" "${BULK_KB:-1400}"
    cp "$HERE/js/workload.js" "$BUILD/js/"

    say "build: bench-hermes and the bytecode (Debian 12, the Hermes prebuilts' toolchain)"
    docker build -q --platform linux/arm64 -t screenkit-batocera-build "$ROOT/tools/batocera" >/dev/null
    docker run --rm --platform linux/arm64 -v "$HERE:/bench" screenkit-batocera-build sh -c '
        set -e
        cmake -S /bench -B /bench/build/hermes -G Ninja -DBENCH_HERMES=ON -DHERMES_DIR=/bench/build/hermes-jit \
            >/tmp/configure.log || { cat /tmp/configure.log; exit 1; }
        cmake --build /bench/build/hermes
        # The flags a ScreenKit package is compiled with (packages/@screenkit/cli/src/hermesc.js).
        for f in workload bulk; do
            /bench/build/hermes-jit/bin/hermesc -emit-binary -O -Xes6-block-scoping -w \
                -out /bench/build/js/$f.hbc /bench/build/js/$f.js
        done'

    say "build: bench-spidermonkey (Debian 13, SpiderMonkey 128 ESR)"
    docker build -q --platform linux/arm64 -t engine-bench-spidermonkey -f "$HERE/docker/spidermonkey.Dockerfile" "$HERE/docker" >/dev/null
    docker run --rm --platform linux/arm64 -v "$HERE:/bench" engine-bench-spidermonkey sh -c '
        set -e
        cmake -S /bench -B /bench/build/spidermonkey -G Ninja -DBENCH_SPIDERMONKEY=ON \
            >/tmp/configure.log || { cat /tmp/configure.log; exit 1; }
        cmake --build /bench/build/spidermonkey
        lib=/bench/build/spidermonkey-lib
        rm -rf $lib && mkdir -p $lib
        cp -L /usr/lib/aarch64-linux-gnu/libmozjs-128.so.0 $lib/
        # Batocera 42 has glibc 2.40 and GLIBCXX_3.4.32 (GCC 13). This is GCC 14: when the
        # binary needs a newer libstdc++ than the Pi has, ours goes beside it.
        max() { objdump -T "$@" | grep -o "$pattern" | sort -uV | tail -1; }
        pattern="GLIBC_2\.[0-9]*"; glibc=$(max /bench/build/spidermonkey/bench-spidermonkey $lib/libmozjs-128.so.0)
        pattern="GLIBCXX_3\.4\.[0-9]*"; glibcxx=$(max /bench/build/spidermonkey/bench-spidermonkey $lib/libmozjs-128.so.0)
        echo "bench-spidermonkey + libmozjs-128 need $glibc, $glibcxx"
        [ "${glibc#GLIBC_2.}" -le 40 ] || { echo "the Pi has glibc 2.40 -- this will not load there" >&2; exit 1; }
        if [ "${glibcxx#GLIBCXX_3.4.}" -gt 32 ]; then
            cp -L /usr/lib/aarch64-linux-gnu/libstdc++.so.6 $lib/
            echo "shipping libstdc++.so.6 beside it"
        fi'

    say "build: the stencils (screenkit-smc, tools/spidermonkey)"
    for f in bulk workload; do
        sh "$ROOT/tools/spidermonkey/smc.sh" "$BUILD/js/$f.js" "$BUILD/js/$f.stencil"
        sh "$ROOT/tools/spidermonkey/smc.sh" --eager "$BUILD/js/$f.js" "$BUILD/js/$f.eager.stencil"
    done
    stage
}

stage() {
    rm -rf "$STAGE"
    mkdir -p "$STAGE/hermes/bin" "$STAGE/hermes/lib" "$STAGE/hermes/lib-interp" "$STAGE/spidermonkey/bin" \
        "$STAGE/spidermonkey/lib" "$STAGE/js"
    cp "$BUILD/hermes/bench-hermes" "$STAGE/hermes/bin/"
    cp "$BUILD/hermes-jit/lib/"* "$BUILD/hermes-jit/BUILDINFO.json" "$STAGE/hermes/lib/"
    # The interpreter library runs against the same ICU; its runpath is $ORIGIN.
    cp "$BUILD/hermes-interp/lib/libhermesvm.so" "$BUILD/hermes-interp/BUILDINFO.json" "$STAGE/hermes/lib-interp/"
    for icu in "$BUILD/hermes-interp/lib/"libicu*; do ln -s "../lib/$(basename "$icu")" "$STAGE/hermes/lib-interp/"; done
    cp "$BUILD/spidermonkey/bench-spidermonkey" "$STAGE/spidermonkey/bin/"
    cp "$BUILD/spidermonkey-lib/"* "$STAGE/spidermonkey/lib/"
    for f in bulk workload; do
        cp "$BUILD/js/$f.js" "$BUILD/js/$f.hbc" "$BUILD/js/$f.stencil" "$BUILD/js/$f.eager.stencil" "$STAGE/js/"
    done
    cp "$HERE/run.sh" "$STAGE/"
    say "staged: $STAGE ($(du -sh "$STAGE" | cut -f1))"
    ( cd "$STAGE" && ls -la hermes/bin hermes/lib/libhermesvm.so hermes/lib-interp/libhermesvm.so spidermonkey/bin spidermonkey/lib js | grep -v '^total' )
}

# In an arm64 container on this machine (Debian 13, which loads both hosts). Numbers from here
# are an Apple-silicon core under Docker, not a Pi: a check that everything runs.
local_run() {
    [ -f "$STAGE/run.sh" ] || build
    configs=${*:-$CONFIGS}
    mkdir -p "$RESULTS/local"
    for c in $configs; do
        say "local: $c"
        docker run --rm --platform linux/arm64 -v "$STAGE:/bench" engine-bench-spidermonkey \
            sh -c "sh /bench/run.sh $c ${BENCH_ARGS:-}; status=\$?; cat /bench/results/$c.log; exit \$status" \
            || echo "  $c failed" >&2
        cp "$STAGE/results/$c.json" "$RESULTS/local/" 2>/dev/null || true
    done
    node "$HERE/compare.mjs" "$RESULTS/local"
}

push() {
    [ -f "$STAGE/run.sh" ] || { echo "nothing staged: sh $0 build" >&2; exit 1; }
    say "push: $(du -sh "$STAGE" | cut -f1) to $PI:$DEST"
    $SSH "mkdir -p $DEST"
    rsync -a --delete --exclude results "$STAGE/" "root@$PI:$DEST/"
}

# The firmware's throttle flags and the ARM's real clock. A weak supply drops a Pi 3 to 600 MHz
# and cpufreq does not show it; bits 0-3 are the state now, 16-19 since boot.
throttle_check() {
    state=$($SSH "vcgencmd get_throttled; vcgencmd measure_clock arm" 2>/dev/null | tr '\n' ' ')
    say "pi ($1): ${state:-no vcgencmd}"
    flags=$(printf '%s' "$state" | sed -n 's/.*throttled=\(0x[0-9a-fA-F]*\).*/\1/p')
    [ -n "$flags" ] || return 0
    if [ $((flags & 0xF)) -ne 0 ]; then
        echo "  the Pi is under-volted or throttled now ($flags): fix the supply" >&2
        [ "${ALLOW_THROTTLED:-0}" = 1 ] || [ "$1" = after ] || exit 1
    elif [ $((flags & 0xF0000)) -ne 0 ]; then
        echo "  the Pi has been throttled since boot ($flags): runs so far may have been slowed" >&2
    fi
}

run() {
    configs=${*:-$CONFIGS}
    mkdir -p "$RESULTS"
    throttle_check before
    # Every file read once, so that no config pays for a cold page cache (the first launch
    # after a boot or a push reads from the SD card): every run starts as a relaunch does.
    $SSH "cd $DEST && cat js/* hermes/bin/* hermes/lib/* spidermonkey/bin/* spidermonkey/lib/* >/dev/null"
    $SSH "cat /proc/loadavg; top -bn1 | sed -n '1,12p'" | sed 's/^/  pi: /'
    for c in $configs; do
        say "run: $c on $PI ($(date +%H:%M:%S))"
        $SSH "cd $DEST && sh run.sh $c ${BENCH_ARGS:-}; status=\$?; cat results/$c.log; exit \$status" || echo "  $c failed" >&2
        scp -q "root@$PI:$DEST/results/$c.json" "root@$PI:$DEST/results/$c.log" "$RESULTS/" || true
        sleep "${COOL:-20}"
    done
    throttle_check after
    node "$HERE/compare.mjs" "$RESULTS"
}

clean() {
    $SSH "rm -rf $DEST" && say "removed $PI:$DEST"
}

case "$cmd" in
    build) build ;;
    local) local_run "$@" ;;
    push) push ;;
    run) run "$@" ;;
    compare) node "$HERE/compare.mjs" "${1:-$RESULTS}" ;;
    all) build; push; run "$@" ;;
    clean) clean ;;
    *) sed -n '2,23p' "$0"; exit 2 ;;
esac

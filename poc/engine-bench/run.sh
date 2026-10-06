#!/bin/sh
# One configuration of the engine bench, where it runs: on the Pi, or in the build container.
# bench.sh stages this beside hermes/, spidermonkey/ and js/. The report goes to
# results/<config>.json and the engine's progress to results/<config>.log -- a file, not a pipe:
# output piped over ssh has been measured adding to frame times on the Pi
# (EMBEDDED_LINUX_EXPERIMENTS.md, "What the once-a-second hitch is").
#
#   sh run.sh <config> [bench options...]
#
#   hermes-interp     the shipping Hermes library (built without the JIT), interpreting bytecode
#   hermes-jit        the JIT build of the same Hermes commit, JIT on (a function compiles after 32 calls)
#   hermes-jit-force  the same, every function compiled on its first call (what VARIANT=jit ships)
#   sm-interp         SpiderMonkey 128, its C++ interpreter only
#   sm-baseline       + the Baseline Interpreter and Baseline JIT
#   sm-jit            + Ion/Warp: every tier, as Firefox runs it
#
# Hermes runs the bytecode hermesc compiled, as a ScreenKit package does. SpiderMonkey runs the
# source as it is, or a stencil compiled from it ahead of time by screenkit-smc, as a
# `--spidermonkey` package does -- the same three tiers, with a suffix:
#
#   sm-<tier>         source, parsed on the device
#   sm-<tier>-lazy    a stencil of top-level code; a function is compiled on its first call
#   sm-<tier>-eager   a stencil with every function compiled; nothing parsed on the device
set -eu
cd "$(dirname "$0")"
config=${1:?usage: run.sh <config> [bench options...]}
shift
mkdir -p results
case "$config" in
    hermes-interp)    set -- env LD_LIBRARY_PATH=hermes/lib-interp hermes/bin/bench-hermes --jit off --preload js/bulk.hbc "$@" js/workload.hbc ;;
    hermes-jit)       set -- hermes/bin/bench-hermes --jit on --preload js/bulk.hbc "$@" js/workload.hbc ;;
    hermes-jit-force) set -- hermes/bin/bench-hermes --jit force --preload js/bulk.hbc "$@" js/workload.hbc ;;
    sm-*)
        tier=${config#sm-}
        form=js
        case "$tier" in
            *-lazy) tier=${tier%-lazy}; form=stencil ;;
            *-eager) tier=${tier%-eager}; form=eager.stencil ;;
        esac
        case "$tier" in
            interp) jit=off ;;
            baseline) jit=baseline ;;
            jit) jit=on ;;
            *) echo "unknown config: $config" >&2; exit 2 ;;
        esac
        set -- spidermonkey/bin/bench-spidermonkey --jit $jit --preload js/bulk.$form "$@" js/workload.$form ;;
    *) echo "unknown config: $config" >&2; exit 2 ;;
esac
"$@" --label "$config" --out "results/$config.json" 2>"results/$config.log"

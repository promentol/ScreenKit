#!/bin/sh
# JSI's conformance suite against the SpiderMonkey runtime, in the arm64 SpiderMonkey
# container (tools/spidermonkey/Dockerfile). Arguments go to the test binary:
#
#   sh runtime/tests/jsi/run.sh
#   sh runtime/tests/jsi/run.sh --gtest_filter='*HostObject*'
#
# Three tests are left out, for one reason: they keep two runtimes alive on one
# thread (a decorator around a second runtime, and SetRuntimeData's rt2/rt3),
# and SpiderMonkey has one JSContext per thread. The runtime refuses a second
# one with an error rather than crash; ScreenKit runs one per JS thread.
set -eu
[ $# -gt 0 ] || set -- --gtest_filter='-*.DecoratorTest/*:*.MultiDecoratorTest/*:*.SetRuntimeData/*'
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
DOCKER_BIN=/Applications/Docker.app/Contents/Resources/bin
[ -d "$DOCKER_BIN" ] && PATH="$PATH:$DOCKER_BIN"
docker build -q --platform linux/arm64 -t screenkit-spidermonkey "$ROOT/tools/spidermonkey" >/dev/null
docker run --rm --platform linux/arm64 -v "$ROOT:/screenkit" screenkit-spidermonkey sh -c '
    set -e
    cmake -S /screenkit/runtime/tests/jsi -B /screenkit/runtime/build/jsi-conformance -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo >/tmp/configure.log || { cat /tmp/configure.log; exit 1; }
    cmake --build /screenkit/runtime/build/jsi-conformance
    /screenkit/runtime/build/jsi-conformance/jsi-conformance "$@"' sh "$@"

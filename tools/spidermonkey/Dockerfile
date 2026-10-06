# The SpiderMonkey toolchain: Debian 13 (trixie) for arm64, whose libmozjs-128 is
# SpiderMonkey 128 ESR with its JIT -- the engine behind runtime/core/src/spidermonkey/.
#
# Used for what needs the engine *natively*: JSI's conformance suite against the
# SpiderMonkey runtime (runtime/tests/jsi/) and `screenkit-smc`, which compiles a
# package's script to a stencil with the very library the device loads. The
# device build itself is Debian 12's toolchain (tools/batocera/Dockerfile) against
# this same library, unpacked from the same pinned .deb (tools/spidermonkey/fetch.sh).
#
# Pinned by digest (the multi-architecture index), as the other build images are.
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates cmake g++ libgtest-dev libmozjs-128-dev ninja-build pkg-config \
 && rm -rf /var/lib/apt/lists/*

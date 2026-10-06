# The build machine for bench-spidermonkey: Debian 13 (trixie) for arm64, whose libmozjs-128 is
# SpiderMonkey 128 ESR with its JIT (Baseline Interpreter, Baseline, Ion/Warp) -- no SpiderMonkey
# build of our own.
#
# Why trixie's and not a newer ESR: Debian's arm64 library needs GLIBC_2.38 and GLIBCXX_3.4.30 at
# most, and Batocera 42 on the Pi has glibc 2.40 and GLIBCXX_3.4.32, so the library runs there as
# is (bench.sh ships it beside the binary). bookworm's only choice is 102.
#
# Pinned by digest (the multi-architecture index), as the other build images in this repo.
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates cmake g++ libmozjs-128-dev ninja-build pkg-config \
 && rm -rf /var/lib/apt/lists/*

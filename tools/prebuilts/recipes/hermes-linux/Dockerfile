# The toolchain the Linux Hermes prebuilts are built with: Debian 12 (bookworm), GCC 12, CMake,
# Ninja. One image per architecture -- native on the build host, or emulated.
#
# bookworm is what Raspberry Pi OS is today, so a library built here needs nothing newer than
# the Pi already has: glibc 2.36 and libstdc++ from GCC 12. ICU 72 comes from here too, and
# travels inside the archive (build-in-container.sh says why). The base image is pinned by digest
# (the multi-architecture index), so the toolchain does not move under the pin; apt packages
# still can, which is why manifest.json pins the published archives by sha256 rather than
# promising a bit-identical rebuild.
FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates cmake file g++ libicu-dev make ninja-build patchelf python3 \
 && rm -rf /var/lib/apt/lists/*

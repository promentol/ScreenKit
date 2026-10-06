#!/bin/bash
# Runs inside the recipe's container (build.sh starts it): builds libhermesvm.so and hermesc
# from the pinned Hermes source, packages them with the embedder headers, smoke-tests the
# package, and writes the archive.
#
#   /src     the Hermes checkout at the pinned commit (read-only)
#   /recipe  this directory (read-only)
#   /smoke   smoke.hbc, compiled by the pinned hermesc on the host
#   /out     where the archive goes
#
#   build-in-container.sh <target> <version> <commit> <bytecode version>
set -euo pipefail

target=$1 version=$2 commit=$3 bytecode=$4
# Hermes' own HERMESVM_ALLOW_JIT: 0 off (what ships), 1 auto, 2 on. A JIT build is a separate
# archive so it never overwrites the shipping one.
jit=${HERMES_JIT:-0}
# HERMESVM_ALLOW_JIT as a CMake variable only decides whether the JIT unit tests
# are built (unittests/VMRuntime/CMakeLists.txt); include/hermes/VM/JIT/Config.h
# reads it as a *preprocessor* macro, and an undefined macro is 0 -- which is why
# passing it to cmake alone produces a library with no JIT in it. It has to reach
# the compiler.
jit_cxx_flags=""
[ "${jit}" = 0 ] || jit_cxx_flags="-DCMAKE_CXX_FLAGS=-DHERMESVM_ALLOW_JIT=${jit}"
name="hermes-${target}-${version}"
[ "${jit}" = 0 ] || name="hermes-jit-${target}-${version}"
build=/tmp/build
stage="/tmp/stage/${name}"
jobs=${JOBS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}

say() { echo ">>> ${target}: $*"; }

# The flags React Native builds its Apple framework with (utils/build-apple-framework.sh):
#   HERMES_ENABLE_INTL=ON -- web builds assume Intl, as every browser has it; Pixi's text reads
#     `Intl?.Segmenter`, which throws where `Intl` is not defined at all. Apple's Intl comes from
#     the OS; here it is ICU. The system's ICU will not do -- its soname differs between
#     distributions (72 on Debian 12, 73 on Batocera 42) -- and Hermes cannot link a static ICU
#     into a shared library, so Debian's ICU 72 shared libraries travel in the archive's lib/,
#     found through an $ORIGIN runpath. ICU versions its symbols (ucol_open_72), so a system ICU
#     loaded by something else in the process does not collide. About 34 MB, most of it data.
#   HERMES_ENABLE_DEBUGGER=OFF -- as in React Native's release build.
# Hermes links ICU into its executables but not into the shared hermesvm on Linux, so the
# shared link names ICU itself; --no-as-needed keeps it in the library's NEEDED list.
# Tools stay on: the build compiles its own internal JavaScript with the hermesc it builds.
say "configuring ($(g++ --version | head -1), ${jobs} jobs)"
cmake -S /src -B "${build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DHERMES_RELEASE_VERSION="${version}" \
  -DHERMES_ENABLE_DEBUGGER=OFF \
  -DHERMES_ENABLE_INTL=ON \
  -DHERMES_USE_STATIC_ICU=OFF \
  "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-as-needed -licui18n -licuuc -licudata" \
  -DHERMES_ENABLE_TEST_SUITE=OFF \
  -DHERMES_ENABLE_LIBFUZZER=OFF \
  -DHERMES_ENABLE_FUZZILLI=OFF \
  -DHERMES_BUILD_SHARED_JSI=OFF \
  -DHERMESVM_ALLOW_JIT="${jit}" \
  ${jit_cxx_flags} \
  > /tmp/configure.log || { tail -40 /tmp/configure.log; exit 1; }

say "building hermesvm and hermesc"
cmake --build "${build}" --target hermesvm hermesc -j "${jobs}" > /tmp/build.log 2>&1 || { tail -60 /tmp/build.log; exit 1; }

say "staging"
rm -rf "${stage}"
mkdir -p "${stage}/lib" "${stage}/bin" "${stage}/include/hermes/Public" "${stage}/include/hermes/cdp" "${stage}/include/jsi"
cp "${build}/lib/libhermesvm.so" "${stage}/lib/"
cp "${build}/bin/hermesc" "${stage}/bin/"
strip --strip-unneeded "${stage}/lib/libhermesvm.so"
strip "${stage}/bin/hermesc"
multiarch=$(gcc -dumpmachine)
for icu in icuuc icui18n icudata; do
  cp -L "/usr/lib/${multiarch}/lib${icu}.so.72" "${stage}/lib/"
done
# The build tree's runpaths point at /usr/lib; the archive's point at itself.
patchelf --set-rpath '$ORIGIN' "${stage}/lib/libhermesvm.so" "${stage}/lib/libicuuc.so.72" "${stage}/lib/libicui18n.so.72"
patchelf --set-rpath '$ORIGIN/../lib' "${stage}/bin/hermesc"
# The same headers, in the same layout, as the Apple destroot's include/.
cp /src/public/hermes/Public/*.h "${stage}/include/hermes/Public/"
cp /src/API/hermes/*.h "${stage}/include/hermes/"
cp /src/API/hermes/cdp/*.h "${stage}/include/hermes/cdp/"
cp /src/API/jsi/jsi/*.h "${stage}/include/jsi/"
cp /src/LICENSE "${stage}/"

say "smoke test"
# Against a copy with the system ICU moved aside, so the bundled one is what loads.
mkdir -p /tmp/icu-aside && mv /usr/lib/${multiarch}/libicu* /tmp/icu-aside/
g++ -std=c++17 -O1 -DSCREENKIT_SMOKE_JIT="${jit}" -I"${stage}/include" /recipe/smoke.cpp -o /tmp/smoke \
  -L"${stage}/lib" -lhermesvm -Wl,-rpath,"${stage}/lib"
/tmp/smoke /smoke/smoke.hbc /recipe/smoke.js "${bytecode}" /smoke/hot.hbc
# And the library's own hermesc agrees with the pinned one about the bytecode it writes.
"${stage}/bin/hermesc" -emit-binary -O -Xes6-block-scoping -out /tmp/native.hbc /recipe/smoke.js
/tmp/smoke /tmp/native.hbc /recipe/smoke.js "${bytecode}" > /dev/null
"${stage}/bin/hermesc" -version | grep -q "HBC bytecode version: ${bytecode}" \
  || { echo "hermesc does not report bytecode version ${bytecode}"; "${stage}/bin/hermesc" -version; exit 1; }

mv /tmp/icu-aside/* /usr/lib/${multiarch}/

# JSI's implementation is inside the library, as it is in the Apple framework: an embedder
# links libhermesvm.so alone.
jsi_symbols=$(nm -DC --defined-only "${stage}/lib/libhermesvm.so" | grep -c 'facebook::jsi::' || true)
[ "${jsi_symbols}" -gt 0 ] || { echo "libhermesvm.so exports no facebook::jsi symbols"; exit 1; }

needed=$(readelf -d "${stage}/lib/libhermesvm.so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | paste -sd, -)
glibc=$(objdump -T "${stage}/lib/libhermesvm.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
glibcxx=$(objdump -T "${stage}/lib/libhermesvm.so" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)
cat > "${stage}/BUILDINFO.json" <<EOF
{
  "hermesVersion": "${version}",
  "commit": "${commit}",
  "bytecodeVersion": ${bytecode},
  "target": "${target}",
  "machine": "$(uname -m)",
  "toolchain": "$(g++ --version | head -1)",
  "base": "debian bookworm ($(cat /etc/debian_version))",
  "jit": "HERMESVM_ALLOW_JIT=${jit}",
  "cmake": "-DCMAKE_BUILD_TYPE=MinSizeRel -DHERMES_ENABLE_DEBUGGER=OFF -DHERMES_ENABLE_INTL=ON -DHERMES_USE_STATIC_ICU=OFF -DHERMES_BUILD_SHARED_JSI=OFF -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-as-needed -licui18n -licuuc -licudata",
  "icu": "$(dpkg -s libicu-dev | sed -n 's/^Version: //p'), in lib/",
  "needed": "${needed}",
  "requiresGlibc": "${glibc}",
  "requiresGlibcxx": "${glibcxx}",
  "jsiSymbols": ${jsi_symbols}
}
EOF

say "packaging"
# Deterministic packaging: sorted, owned by root, stamped with the commit's time, gzip without a
# timestamp. The compile itself is not guaranteed bit-identical across toolchain updates.
mtime=$(date -u -d "@${SOURCE_DATE_EPOCH}" '+%Y-%m-%d %H:%M:%S')
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="${mtime}" -C /tmp/stage -cf - "${name}" \
  | gzip -n -9 > "/out/${name}.tar.gz"
( cd /out && sha256sum "${name}.tar.gz" && stat -c '%s bytes' "${name}.tar.gz" )
cat "${stage}/BUILDINFO.json"

#!/bin/bash
# Builds the Linux Hermes prebuilts -- the one dependency no upstream publishes for Linux
# (tools/prebuilts/README.md, "Hermes on Linux") -- from the commit manifest.json pins, in Docker.
#
#   tools/prebuilts/recipes/hermes-linux/build.sh                 # linux-arm64 and linux-x86_64
#   tools/prebuilts/recipes/hermes-linux/build.sh linux-arm64     # one target
#   HERMES_JIT=2 ... build.sh linux-arm64                         # the same, with Hermes' JIT
#
# HERMES_JIT is Hermes' own HERMESVM_ALLOW_JIT: 0 off (the default, what ships), 1 if the
# platform supports it, 2 on. A JIT build is written as a separate archive, hermes-jit-<target>-
# <version>.tar.gz, and keeps its own build volume, so the shipping prebuilt is never overwritten
# by an experiment. The library still has to be asked for the JIT at runtime (RuntimeConfig's
# EnableJIT; the host reads SCREENKIT_HERMES_JIT).
#
# Writes tools/prebuilts/dist/hermes-<target>-<version>.tar.gz and prints the sha256 and size
# that manifest.json records. Each archive is smoke-tested in its own architecture before it
# is written: bytecode from the pinned hermesc must load and run through the new library.
#
# Needs Docker. On Apple silicon linux-arm64 builds natively and linux-x86_64 runs under
# emulation (Rosetta when Docker Desktop has it on), which is several times slower. The build
# directory is kept in a Docker volume, screenkit-hermes-build-<target>-<version>, so a rerun is
# incremental; `docker volume rm` it to start clean.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../../.." && pwd)"
MANIFEST="${ROOT}/tools/prebuilts/manifest.json"
PREBUILTS="${SCREENKIT_PREBUILTS:-${HOME}/.screenkit/prebuilts}"
DIST="${ROOT}/tools/prebuilts/dist"

field() { node -e "const m = require('${MANIFEST}'); console.log($1)"; }
version=$(field 'm.hermes.version')
jit=${HERMES_JIT:-0}
variant=""
[ "${jit}" = 0 ] || variant="-jit"
commit=$(field 'm.hermes.linux.commit')
bytecode=$(field 'm.hermes.bytecodeVersion')

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(linux-arm64 linux-x86_64)

# The source, pinned by commit id: fetching by id is the verification.
source="${PREBUILTS}/hermes/${version}/source"
if [ "$(git -C "${source}" rev-parse HEAD 2>/dev/null)" != "${commit}" ]; then
  echo ">>> fetching facebook/hermes at ${commit}"
  rm -rf "${source}"
  mkdir -p "${source}"
  git -C "${source}" init -q
  git -C "${source}" remote add origin https://github.com/facebook/hermes.git
  git -C "${source}" fetch -q --depth 1 origin "${commit}"
  git -C "${source}" checkout -q FETCH_HEAD
fi
if [ -n "$(git -C "${source}" status --porcelain)" ]; then
  echo "${source} has local changes; refusing to build a prebuilt from it" >&2
  exit 1
fi
epoch=$(git -C "${source}" log -1 --format=%ct)

# The smoke test's bytecode comes from the pinned hermesc, not the one being built.
hermesc=$(node "${ROOT}/tools/prebuilts/fetch.mjs" --hermesc 2>/dev/null)
smoke=$(mktemp -d)
trap 'rm -rf "${smoke}"' EXIT
"${hermesc}" -emit-binary -O -Xes6-block-scoping -out "${smoke}/smoke.hbc" "${HERE}/smoke.js"
# The JIT measurement's workload, as a device runs it: optimised bytecode.
[ "${jit}" = 0 ] || "${hermesc}" -emit-binary -O -Xes6-block-scoping -out "${smoke}/hot.hbc" "${HERE}/hot.js"

mkdir -p "${DIST}"
for target in "${targets[@]}"; do
  case "${target}" in
    linux-arm64) platform=linux/arm64 ;;
    linux-x86_64) platform=linux/amd64 ;;
    *) echo "unknown target ${target} (linux-arm64 | linux-x86_64)" >&2; exit 1 ;;
  esac
  image="screenkit-hermes-linux:${target}"
  [ "${jit}" = 0 ] && echo ">>> ${target}: interpreter build" || echo ">>> ${target}: JIT build (HERMESVM_ALLOW_JIT=${jit})"
  echo ">>> ${target}: toolchain image (${platform})"
  docker build -q --platform "${platform}" -t "${image}" "${HERE}" > /dev/null
  docker run --rm --platform "${platform}" \
    -e SOURCE_DATE_EPOCH="${epoch}" -e JOBS="${JOBS:-}" -e HERMES_JIT="${jit}" \
    -v "${source}:/src:ro" -v "${HERE}:/recipe:ro" -v "${smoke}:/smoke:ro" -v "${DIST}:/out" \
    -v "screenkit-hermes-build-${target}${variant}-${version}:/tmp/build" \
    "${image}" bash /recipe/build-in-container.sh "${target}" "${version}" "${commit}" "${bytecode}"
done

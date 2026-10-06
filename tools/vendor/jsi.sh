#!/bin/sh
# Vendor JSI's own sources into runtime/third_party/jsi/.
#
# JSI is the interface every binding in the runtime is written against. With
# Hermes, its implementation (jsi.cpp) is compiled into libhermesvm.so and the
# headers come with the prebuilt. A runtime built on another engine
# (runtime/core/src/spidermonkey/) links no Hermes at all, so it needs JSI's
# sources itself -- and they must be the *same* JSI the Hermes build uses, or a
# binding compiled against one vtable would run against another. So they are
# taken from the Hermes commit tools/prebuilts/manifest.json pins, file by file,
# each pinned by sha256, byte for byte: no patches.
#
# The test suite comes along (test/testlib.*): it is JSI's own conformance
# suite, the one Hermes and JSC run, and it is how the SpiderMonkey runtime is
# held to the same contract (runtime/tests/jsi/).
#
#   sh tools/vendor/jsi.sh            (re-)import at the pinned commit
#   sh tools/vendor/jsi.sh --check    verify the tree matches the pins; write nothing
#
# A Hermes bump moves COMMIT (it must equal manifest.json's hermes.linux.commit)
# and every sha256 below; the script refuses a commit that disagrees with the
# manifest, so the two JSIs cannot drift apart.
set -eu

# ---- the pin ----------------------------------------------------------------
UPSTREAM="https://raw.githubusercontent.com/facebook/hermes"
COMMIT="4188b63f24026ca62b79dc81d95db122a59c6089"
# sha256 <upstream path> <path under runtime/third_party/jsi>
FILES="
db2f1ee9291340f3d05b11abdcdc3d12ac84c239e58599d4ef2ac06dbaa14d92 API/jsi/jsi/jsi.h jsi/jsi.h
a6cbf72ee25991a25cd43049e96fb275c900e24da67d5c2b9581faaca11f06a4 API/jsi/jsi/jsi-inl.h jsi/jsi-inl.h
0177c5f86be636c13fc88fc60b10d5d0977a0ca89040d1267799454eab0499e7 API/jsi/jsi/jsi.cpp jsi/jsi.cpp
1395779ad53ec372518b977d077e65a876215bc31a94cf049ed55d0a3d2426f4 API/jsi/jsi/instrumentation.h jsi/instrumentation.h
155fa250fc68b7e9e3d17a7d75f3760a6bd103af8ecc4352ac4d59e3a91d0613 API/jsi/jsi/decorator.h jsi/decorator.h
7291dc7681f285b2706523246052a6b727ef1ae93bed83d20faed99469c14ce6 API/jsi/jsi/test/testlib.h jsi/test/testlib.h
5976993807c96263d71e248cbc058f4853758910319933832c2eed2d17e3c4c8 API/jsi/jsi/test/testlib.cpp jsi/test/testlib.cpp
da6d3703ed11cbe42bd212c725957c98da23cbff1998c05fa4b3d976d1a58e93 LICENSE LICENSE
"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/runtime/third_party/jsi"

CHECK_ONLY=0
case "${1:-}" in
    --check) CHECK_ONLY=1 ;;
    "") ;;
    -h|--help) sed -n '2,23p' "$0" | sed 's|^# \{0,1\}||'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
esac

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
fi

pinned=$(node -p "require('$ROOT/tools/prebuilts/manifest.json').hermes.linux.commit" 2>/dev/null || echo "")
if [ -n "$pinned" ] && [ "$pinned" != "$COMMIT" ]; then
    echo "jsi.sh pins Hermes $COMMIT but tools/prebuilts/manifest.json pins $pinned:" >&2
    echo "the vendored JSI would not be the JSI the Hermes build runs. Move both together." >&2
    exit 1
fi

bad=0
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
echo "$FILES" | while read -r sum src dst; do
    [ -n "$sum" ] || continue
    if [ "$CHECK_ONLY" = 1 ]; then
        if [ ! -f "$OUT/$dst" ] || [ "$(sha256 "$OUT/$dst")" != "$sum" ]; then
            echo "runtime/third_party/jsi/$dst does not match the pin (Hermes $COMMIT)" >&2
            exit 1
        fi
        continue
    fi
    mkdir -p "$stage/$(dirname "$dst")"
    curl -fsSL -o "$stage/$dst" "$UPSTREAM/$COMMIT/$src"
    got=$(sha256 "$stage/$dst")
    [ "$got" = "$sum" ] || { echo "sha256 mismatch for $src at $COMMIT: $got" >&2; exit 1; }
done || bad=1
[ "$bad" = 0 ] || exit 1

if [ "$CHECK_ONLY" = 1 ]; then
    echo "runtime/third_party/jsi matches Hermes $COMMIT"
    exit 0
fi

rm -rf "$OUT"
mkdir -p "$OUT"
cp -R "$stage/." "$OUT/"
cat >"$OUT/VENDOR.md" <<EOF
# JSI, vendored

JSI's headers, its implementation (\`jsi/jsi.cpp\`) and its conformance suite
(\`jsi/test/\`), byte for byte from facebook/hermes at \`$COMMIT\` -- the
commit \`tools/prebuilts/manifest.json\` pins for the Hermes prebuilts. MIT,
\`LICENSE\`.

Written by \`tools/vendor/jsi.sh\`; do not edit. A Hermes bump re-runs it.
The Hermes build takes JSI from the prebuilt and never compiles these; they
exist for runtimes on other engines (\`runtime/core/src/spidermonkey/\`).
EOF
echo "runtime/third_party/jsi: JSI from Hermes $COMMIT"

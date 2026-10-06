#!/bin/sh
# SpiderMonkey for the Linux runtime's SpiderMonkey engine (SCREENKIT_ENGINE=spidermonkey):
# Debian 13's libmozjs-128 -- SpiderMonkey 128 ESR, JIT included -- unpacked into
# tools/spidermonkey/dist/<target>/ as include/mozjs-128/ and lib/libmozjs-128.so.0,
# where runtime/cmake/SpiderMonkey.cmake looks.
#
#   sh tools/spidermonkey/fetch.sh [linux-arm64]
#
# Not built by us: Debian's arm64 library needs glibc 2.38 and GLIBCXX_3.4.30, and
# Batocera 42 has 2.40 and 3.4.32, so it runs on the Pi as it is and ships beside
# the host. The packages are pinned by sha256, as the Pi's SDL headers are.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:-linux-arm64}
VERSION="128.14.0-1~deb13u1"
case "$TARGET" in
    linux-arm64)
        ARCH=arm64 TRIPLE=aarch64-linux-gnu
        LIB_SHA256=dd5d224c3331e934b4d027cccfbced2a8bf475bdfbbdced0765166b95e7d93fb
        DEV_SHA256=77d2f13c02026ccd9a5f862fae12276a986e8ad8293953195e174b725e081b5d
        ;;
    *) echo "no SpiderMonkey pin for $TARGET (linux-arm64)" >&2; exit 1 ;;
esac
POOL="http://deb.debian.org/debian/pool/main/m/mozjs128"
OUT="$HERE/dist/$TARGET"

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
fi

[ -f "$OUT/lib/libmozjs-128.so.0" ] && [ -f "$OUT/include/mozjs-128/jsapi.h" ] && { echo "$OUT"; exit 0; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for pkg in "libmozjs-128-0:$LIB_SHA256" "libmozjs-128-dev:$DEV_SHA256"; do
    name=${pkg%%:*} sum=${pkg#*:}
    file="${name}_${VERSION}_${ARCH}.deb"
    curl -fsSL -o "$tmp/$file" "$POOL/$file"
    got=$(sha256 "$tmp/$file")
    [ "$got" = "$sum" ] || { echo "sha256 mismatch for $file: $got" >&2; exit 1; }
    mkdir -p "$tmp/$name"
    (cd "$tmp/$name" && ar x "../$file" && tar -xf data.tar.*)
done
rm -rf "$OUT"
mkdir -p "$OUT/include" "$OUT/lib"
cp -R "$tmp/libmozjs-128-dev/usr/include/$TRIPLE/mozjs-128" "$OUT/include/"
cp -L "$tmp/libmozjs-128-0/usr/lib/$TRIPLE/libmozjs-128.so.0" "$OUT/lib/"
echo "SpiderMonkey $VERSION ($TARGET)" >"$OUT/VERSION"
echo "$OUT"

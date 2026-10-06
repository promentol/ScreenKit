#!/bin/sh
# Vendor yhirose/cpp-httplib's single header into runtime/third_party/httplib/.
#
# This is the Linux HTTP client's one dependency (runtime/core/src/net/NetServiceLinux.cpp).
# It is a single MIT-licensed header, so it is taken whole from one immutable tag
# and pinned by the sha256 of the file itself -- the same shape as the SDL
# headers tools/batocera/pi.sh fetches and the commit tools/vendor/expo-gl.sh
# imports at.
#
# One table of patches is applied on the way in, tools/vendor/httplib.rules, and
# the patched header is pinned too: HEADER_SHA256 is upstream's file as fetched,
# PATCHED_SHA256 is what lands in the tree. Each patch row must match exactly
# once or the import stops.
#
#   sh tools/vendor/httplib.sh            re-import at the pinned version
#   sh tools/vendor/httplib.sh --check    verify the tree matches the pin; write nothing
#   sh tools/vendor/httplib.sh --version v0.57.0
#                                         bump: fetch that tag and print the new sha256s,
#                                         which then go into the block below
#
# The tree is never hand-edited (Architecture.md 10.3, "no edits to vendored
# code"): a change is a new pin here and a re-run. `--check` is what
# runtime/cmake/HttplibVendored.cmake does at configure time, so a hand-edit
# fails the Linux build rather than shipping.
set -eu

# ---- the pin ----------------------------------------------------------------
# The tag and the commit it resolves to: a tag can be moved, an object cannot,
# and the sha256 below is what actually decides. v0.56.0 is the release current
# when the Linux client was written (2026-09-18).
UPSTREAM_REPO="https://github.com/yhirose/cpp-httplib"
VERSION="v0.56.0"
COMMIT="278c2979e8c68468960c3073e28e1c51b098d6a4"
HEADER_SHA256="1f99e51881c4c9d0649b27c611442c2f4d9bcfec5a22a14d5fcd1f8106f730b4"
LICENSE_SHA256="4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b"
# httplib.h after tools/vendor/httplib.rules: what is in the tree, and what
# `--check` compares against.
PATCHED_SHA256="ed56be21ef4224cda0d84dc85205307fe748fc51edc063af02739a728ba0d0c1"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/runtime/third_party/httplib"
RULES="$ROOT/tools/vendor/httplib.rules"

CHECK_ONLY=0
BUMP_TO=""
while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK_ONLY=1; shift ;;
        --version) [ $# -ge 2 ] || { echo "--version needs a value" >&2; exit 64; }; BUMP_TO="$2"; shift 2 ;;
        --version=*) BUMP_TO="${1#--version=}"; shift ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's|^# \{0,1\}||'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

# `--check` verifies the tree against the pin in this file. `--version` asks for
# a *different* version, which by definition has no pin here yet, so the two
# together could only ever compare against nothing and report a mismatch.
if [ "$CHECK_ONLY" = 1 ] && [ -n "$BUMP_TO" ]; then
    echo "--check verifies the pinned version ($VERSION); --version asks for another." >&2
    echo "Run them separately: \`--version $BUMP_TO\` prints a new pin, \`--check\` verifies the current one." >&2
    exit 64
fi

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
    echo "neither shasum nor sha256sum is on PATH" >&2
    exit 1
fi

say() { printf '>>> %s\n' "$*"; }

# --check reads only what is already here; it must work offline and in a
# container with no network, because that is where the Linux build runs it.
if [ "$CHECK_ONLY" = 1 ]; then
    failed=0
    for pair in "httplib.h $PATCHED_SHA256" "LICENSE $LICENSE_SHA256"; do
        file=${pair%% *}
        want=${pair##* }
        if [ ! -f "$OUT/$file" ]; then
            echo "missing $OUT/$file -- run: sh tools/vendor/httplib.sh" >&2
            failed=1
            continue
        fi
        got=$(sha256 "$OUT/$file")
        if [ "$got" != "$want" ]; then
            echo "$OUT/$file does not match the pin for cpp-httplib $VERSION (+ $RULES)" >&2
            echo "  expected $want" >&2
            echo "  found    $got" >&2
            echo "  vendored code is never hand-edited: re-run sh tools/vendor/httplib.sh" >&2
            failed=1
        fi
    done
    [ "$failed" = 0 ] || exit 1
    say "cpp-httplib $VERSION: httplib.h (patched) and LICENSE match the pin"
    exit 0
fi

# Apply tools/vendor/httplib.rules to $1 in place. Every row must match exactly
# once: a patch whose target upstream has reshaped must stop the import, not
# silently fall away.
apply_rules() {
    perl -e '
use strict;
use warnings;
my ($rules_path, $file) = @ARGV;
open(my $rh, "<", $rules_path) or die "cannot read $rules_path: $!\n";
my @rows;
while (my $line = <$rh>) {
  next if $line =~ /^\s*(#|$)/;
  chomp $line;
  my @f = split(/\s*\|\s*/, $line, 4);
  for (@f) { s/^\s+//; s/\s+$//; }
  die "$rules_path:$.: not a patch row\n" unless @f == 4 && $f[0] eq "patch";
  my $re = eval { qr/$f[1]/m };
  die "$rules_path:$.: bad regex: $@" if $@;
  push @rows, [$re, $f[2], $f[3], $.];
}
close $rh;
die "$rules_path lists no patch rows\n" unless @rows;
sub expand {
  my ($repl) = @_;
  my @cap = ($1, $2, $3, $4, $5, $6, $7, $8, $9);
  $repl =~ s/\\n/\n/g;
  $repl =~ s/\\t/\t/g;
  $repl =~ s/\$(\d)/defined $cap[$1 - 1] ? $cap[$1 - 1] : ""/ge;
  return $repl;
}
open(my $fh, "<", $file) or die "cannot read $file: $!\n";
my $text = do { local $/; <$fh> };
close $fh;
for my $row (@rows) {
  my ($re, $repl, $why, $lineno) = @$row;
  my $n = 0;
  $n++ while $text =~ /$re/g;
  die "$rules_path:$lineno ($why) matched $n times, not once -- upstream changed under it\n" unless $n == 1;
  $text =~ s/$re/expand($repl)/e;
}
open($fh, ">", $file) or die "cannot write $file: $!\n";
print $fh $text;
close $fh;
' "$RULES" "$1"
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

if [ -n "$BUMP_TO" ]; then
    # A bump: resolve the tag to the commit it names *now*, fetch that object and
    # print the pin it would become. Nothing is written -- this script only ever
    # writes a tree it can verify.
    say "cpp-httplib $BUMP_TO from $UPSTREAM_REPO (resolving)"
    commit=$(curl -fsSL "https://api.github.com/repos/yhirose/cpp-httplib/git/ref/tags/$BUMP_TO" \
             | sed -n 's/.*"sha": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1)
    [ -n "$commit" ] || { echo "cannot resolve $BUMP_TO to a commit" >&2; exit 1; }
    curl -fsSL -o "$tmp/httplib.h" "https://raw.githubusercontent.com/yhirose/cpp-httplib/$commit/httplib.h"
    curl -fsSL -o "$tmp/LICENSE" "https://raw.githubusercontent.com/yhirose/cpp-httplib/$commit/LICENSE"
    got_header=$(sha256 "$tmp/httplib.h")
    got_license=$(sha256 "$tmp/LICENSE")
    cat >&2 <<EOF
cpp-httplib $BUMP_TO fetched but not written: this script writes only a pinned tree.
Put these in the pin block at the top of $0, then run it again with no arguments:

  VERSION="$BUMP_TO"
  COMMIT="$commit"
  HEADER_SHA256="$got_header"
  LICENSE_SHA256="$got_license"

...and update runtime/third_party/httplib/VENDOR.md with the same values.
EOF
    exit 2
fi

# By COMMIT, never by tag. A tag is a movable reference and the whole point of
# recording the commit is that this fetch cannot be pointed somewhere else
# without the pin block changing.
say "cpp-httplib $VERSION ($COMMIT) from $UPSTREAM_REPO"
curl -fsSL -o "$tmp/httplib.h" "https://raw.githubusercontent.com/yhirose/cpp-httplib/$COMMIT/httplib.h"
curl -fsSL -o "$tmp/LICENSE" "https://raw.githubusercontent.com/yhirose/cpp-httplib/$COMMIT/LICENSE"

got_header=$(sha256 "$tmp/httplib.h")
got_license=$(sha256 "$tmp/LICENSE")

[ "$got_header" = "$HEADER_SHA256" ] || {
    echo "checksum mismatch for httplib.h at $COMMIT: $got_header" >&2
    exit 1
}
[ "$got_license" = "$LICENSE_SHA256" ] || {
    echo "checksum mismatch for LICENSE at $COMMIT: $got_license" >&2
    exit 1
}

apply_rules "$tmp/httplib.h"
got_patched=$(sha256 "$tmp/httplib.h")
[ "$got_patched" = "$PATCHED_SHA256" ] || {
    echo "httplib.h after $RULES is $got_patched; the pin says $PATCHED_SHA256." >&2
    echo "Nothing was written. If the rules table changed on purpose, read the diff it makes," >&2
    echo "then put the new value in PATCHED_SHA256 at the top of $0." >&2
    exit 1
}

mkdir -p "$OUT"
cp "$tmp/httplib.h" "$OUT/httplib.h"
cp "$tmp/LICENSE" "$OUT/LICENSE"
say "wrote $OUT/httplib.h ($(wc -l <"$OUT/httplib.h" | tr -d ' ') lines, $(grep -c '^patch' "$RULES") patches) and LICENSE, commit $COMMIT"

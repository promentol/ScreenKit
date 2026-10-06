#!/usr/bin/env bash
#
# Vendor expo/expo's packages/expo-gl/common/ into runtime/third_party/gl/,
# rewritten for this project.
#
# This script is the deliverable. The tree it writes is derived, never edited:
# a fix is a change to tools/vendor/expo-gl.rules (or tools/vendor/seam/) and a
# re-run. Re-running against a newer upstream ref is the whole point -- it keeps
# expo-gl a tracked port rather than a fork we merge by hand across 470 KB.
#
#   tools/vendor/expo-gl.sh                 # re-import at the pinned commit
#   tools/vendor/expo-gl.sh --ref sdk-59    # bump: resolve a ref, report the diff
#   tools/vendor/expo-gl.sh --check         # verify the tree matches; write nothing
#   tools/vendor/expo-gl.sh --force         # overwrite local edits (last resort)
#
# Guarantees, in the order they are enforced below:
#   - fails at the start on a missing tool or an unreachable upstream, never
#     half-way through a write;
#   - refuses to clobber a hand-edited output tree, naming the files;
#   - byte-identical output for the same commit, so a re-run is no diff at all;
#   - offline once the commit is in the cache.
#
# Written for bash 3.2, which is what macOS ships: no associative arrays.

set -euo pipefail

# ---- the pin ----------------------------------------------------------------
# The commit, not just the branch: sdk-58 is a moving branch, and "byte-identical
# on a re-run" is only true against an immutable object. The branch name is here
# for humans and for `--ref`.
UPSTREAM_REPO="https://github.com/expo/expo.git"
UPSTREAM_REF="sdk-58"
UPSTREAM_COMMIT="4b2e2a789d1acd03a3f22f23706481a38d17e904"
UPSTREAM_PATH="packages/expo-gl/common"
UPSTREAM_LICENSE="LICENSE"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RULES="$HERE/expo-gl.rules"
SEAM_DIR="$HERE/seam"
OUT_DIR="$ROOT/runtime/third_party/gl"

# Outside the repo, next to the prebuilts cache and for the same reasons
# (Architecture.md 10.1): a checkout stays small, and `rm -rf build/` costs no
# re-download.
CACHE_ROOT="${SCREENKIT_VENDOR_CACHE:-$HOME/.screenkit/vendor}"

FORCE=0
CHECK_ONLY=0
DISCARDED=0
REF_OVERRIDE=""

TAB="$(printf '\t')"

die() { printf 'expo-gl.sh: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*" >&2; }

usage() {
  sed -n '3,22p' "${BASH_SOURCE[0]}" | sed 's|^# \{0,1\}||'
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --ref)     [ $# -ge 2 ] || die "--ref needs a value"; REF_OVERRIDE="$2"; shift 2 ;;
    --ref=*)   REF_OVERRIDE="${1#--ref=}"; shift ;;
    --out)     [ $# -ge 2 ] || die "--out needs a value"; OUT_DIR="$2"; shift 2 ;;
    --out=*)   OUT_DIR="${1#--out=}"; shift ;;
    --force)   FORCE=1; shift ;;
    --check)   CHECK_ONLY=1; shift ;;
    -h|--help) usage 0 ;;
    *)         say "unknown argument: $1"; usage 64 ;;
  esac
done

# ---- preflight: tools, then upstream ----------------------------------------
# Everything that can fail without touching the output tree fails here, so a
# missing tool or a dead network is never a half-written tree.

for tool in git perl find comm; do
  command -v "$tool" >/dev/null 2>&1 \
    || die "\`$tool\` is not on PATH; this script cannot run without it."
done

if command -v shasum >/dev/null 2>&1; then
  sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
  sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
  die "neither \`shasum\` nor \`sha256sum\` is on PATH; the tree cannot be checksummed."
fi

[ -f "$RULES" ]   || die "the rules table is missing: $RULES"
[ -d "$SEAM_DIR" ] || die "the seam sources are missing: $SEAM_DIR"

if [ -n "$REF_OVERRIDE" ]; then
  UPSTREAM_REF="$REF_OVERRIDE"
  say ">>> resolving $UPSTREAM_REF at $UPSTREAM_REPO"
  resolved="$(git ls-remote "$UPSTREAM_REPO" "$UPSTREAM_REF" 2>/dev/null | head -n1 | cut -f1 || true)"
  [ -n "$resolved" ] \
    || die "could not resolve \"$UPSTREAM_REF\" at $UPSTREAM_REPO -- no such ref, or no network."
  UPSTREAM_COMMIT="$resolved"
  say "    $UPSTREAM_REF -> $UPSTREAM_COMMIT"
  say "    (this does not move the pin: edit UPSTREAM_REF/UPSTREAM_COMMIT at the top of this script)"
fi

# ---- the upstream checkout --------------------------------------------------
# One directory per commit, mirroring tools/prebuilts' <dep>/<version>/ shape,
# so several pinned versions coexist and moving the pin invalidates nothing. A
# blobless, sparse, single-commit fetch: ~1 MB rather than a 2 GB monorepo.

SRC_ROOT="$CACHE_ROOT/expo-gl/$UPSTREAM_COMMIT"
SRC_DIR="$SRC_ROOT/$UPSTREAM_PATH"

if [ ! -f "$SRC_ROOT/.fetched" ]; then
  say ">>> fetching expo/expo $UPSTREAM_COMMIT ($UPSTREAM_PATH)"
  rm -rf "$SRC_ROOT"
  mkdir -p "$SRC_ROOT"
  if ! (
    cd "$SRC_ROOT"
    git init --quiet
    git remote add origin "$UPSTREAM_REPO"
    git sparse-checkout init --cone
    # Cone mode always keeps the files at the repo root, which is how LICENSE
    # comes across without a second pattern.
    git sparse-checkout set "$UPSTREAM_PATH"
    git fetch --quiet --depth 1 --filter=blob:none origin "$UPSTREAM_COMMIT"
    git checkout --quiet FETCH_HEAD
    git log -1 --format=%cs FETCH_HEAD > .date
  ) >/dev/null 2>&1; then
    rm -rf "$SRC_ROOT"
    die "could not fetch $UPSTREAM_COMMIT from $UPSTREAM_REPO -- no network, or the commit is gone."
  fi
  touch "$SRC_ROOT/.fetched"
else
  say ">>> expo/expo $UPSTREAM_COMMIT: cached"
fi

[ -d "$SRC_DIR" ] || die "$UPSTREAM_PATH is not in the checkout at $SRC_ROOT -- upstream moved it."
# The upstream commit date, not today's: better provenance, and a today-stamp
# would make two runs of the same import differ.
UPSTREAM_DATE="$(cat "$SRC_ROOT/.date")"

# ---- read the rules ---------------------------------------------------------
# What each kind means is documented in expo-gl.rules. Here they are only split
# into the lists this script walks.

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/expo-gl-vendor.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

read_rules() { # $1 = kind; prints "from<TAB>to" per matching row
  RULE_KIND="$1" perl -ne '
    next if /^\s*(#|$)/;
    my @f = split(/\s*\|\s*/, $_, 4);
    next unless @f >= 3;
    for (@f) { s/^\s+//; s/\s+$//; }
    next unless $f[0] eq $ENV{RULE_KIND};
    print "$f[1]\t$f[2]\n";
  ' "$RULES"
}

FILE_RULES="$(read_rules file)"
VERBATIM_RULES="$(read_rules verbatim)"
SEAM_RULES="$(read_rules seam)"
SKIP_RULES="$(read_rules skip)"

[ -n "$FILE_RULES" ] || die "the rules table lists no \`file\` rows -- $RULES is empty or malformed."

# Every file upstream ships must be accounted for by some row. A new file in a
# newer expo is the likeliest thing to slip through a bump, and silently not
# importing it is how a tracked port turns back into a fork.
printf '%s\n%s\n%s\n%s\n' "$FILE_RULES" "$VERBATIM_RULES" "$SEAM_RULES" "$SKIP_RULES" \
  | cut -d"$TAB" -f1 | grep . | LC_ALL=C sort -u > "$STAGE/accounted"
( cd "$SRC_DIR" && find . -type f | sed 's|^\./||' ) | LC_ALL=C sort > "$STAGE/upstream"

unaccounted="$(comm -13 "$STAGE/accounted" "$STAGE/upstream")"
if [ -n "$unaccounted" ]; then
  say "expo-gl.sh: $UPSTREAM_REF ships files no rule mentions:"
  printf '%s\n' "$unaccounted" | sed "s|^|    $UPSTREAM_PATH/|" >&2
  die "add a \`file\`, \`verbatim\`, \`seam\` or \`skip\` row for each to $RULES."
fi

# ---- the rewriter -----------------------------------------------------------
# One pass per `sub` row, in table order, over the whole file at once so a rule
# may span lines. Replacements interpolate \n, \t and $1..$9 and nothing else --
# a rules table should not be able to run code.

REWRITE="$STAGE/rewrite.pl"
cat > "$REWRITE" <<'PERL'
use strict;
use warnings;

my ($rules_path, $src_path, $hits_path) = @ARGV;

# `sub` and `patch` rows run in one sequence, in table order. A `patch` also
# records how often it matched, keyed by its line in the table, so the caller can
# insist on exactly once across the whole import.
open(my $rh, '<', $rules_path) or die "cannot read $rules_path: $!\n";
my @subs;
while (my $line = <$rh>) {
  my $lineno = $.;
  next if $line =~ /^\s*(#|$)/;
  chomp $line;
  my @f = split(/\s*\|\s*/, $line, 4);
  next unless @f >= 3;
  for (@f) { s/^\s+//; s/\s+$//; }
  next unless $f[0] eq 'sub' || $f[0] eq 'patch';
  my $re = eval { qr/$f[1]/m };
  die "bad regex in the rules table:\n  $f[1]\n  $@" if $@;
  push @subs, [$re, $f[2], $f[0] eq 'patch' ? $lineno : 0];
}
close $rh;
die "the rules table lists no `sub` rows\n" unless @subs;

sub expand {
  my ($repl) = @_;
  # Snapshot the captures first: the escape substitutions below reset $1..$9.
  my @cap = ($1, $2, $3, $4, $5, $6, $7, $8, $9);
  $repl =~ s/\\n/\n/g;
  $repl =~ s/\\t/\t/g;
  $repl =~ s/\$(\d)/defined $cap[$1 - 1] ? $cap[$1 - 1] : ''/ge;
  return $repl;
}

open(my $sh, '<', $src_path) or die "cannot read $src_path: $!\n";
my $text = do { local $/; <$sh> };
close $sh;

my $hits;
if (defined $hits_path) {
  open($hits, '>>', $hits_path) or die "cannot append to $hits_path: $!\n";
}
for my $rule (@subs) {
  my ($re, $repl, $patch_line) = @$rule;
  my $n = ($text =~ s/$re/expand($repl)/ge) || 0;
  print $hits "$patch_line\t$n\n" if $patch_line && $n && $hits;
}
close $hits if $hits;

print $text;
PERL

# ---- provenance -------------------------------------------------------------

vendored_header() { # $1 = upstream file name
  cat <<EOF
// Vendored from expo/expo -- $UPSTREAM_PATH/$1
// Upstream:  $UPSTREAM_REF @ $UPSTREAM_COMMIT ($UPSTREAM_DATE)
// Licence:   MIT, (c) 650 Industries, Inc. -- full text in LICENSE-expo.
//
// GENERATED by tools/vendor/expo-gl.sh. Do not edit this file: a fix is a change
// to tools/vendor/expo-gl.rules and a re-run. See VENDOR.md.

EOF
}

seam_header() { # $1 = the upstream file it replaces, $2 = the seam source name
  cat <<EOF
// ScreenKit seam file. Replaces expo/expo -- $UPSTREAM_PATH/$1
// Upstream:  $UPSTREAM_REF @ $UPSTREAM_COMMIT ($UPSTREAM_DATE)
// Source:    tools/vendor/seam/$2
//
// GENERATED by tools/vendor/expo-gl.sh. Do not edit this file: a fix is a change
// to tools/vendor/seam/$2 and a re-run. See VENDOR.md.

EOF
}

# ---- emit into the staging tree ---------------------------------------------

NEW="$STAGE/out"
mkdir -p "$NEW"
emitted=0
: > "$STAGE/patch-hits"

while IFS="$TAB" read -r from to; do
  [ -n "$from" ] || continue
  [ -f "$SRC_DIR/$from" ] \
    || die "$UPSTREAM_PATH/$from is not in $UPSTREAM_REF -- the \`file\` row for $to is stale."
  vendored_header "$from" > "$NEW/$to"
  perl "$REWRITE" "$RULES" "$SRC_DIR/$from" "$STAGE/patch-hits" >> "$NEW/$to"
  emitted=$((emitted + 1))
done <<EOF
$FILE_RULES
EOF

# A `patch` replaces one specific piece of upstream code. Matching nothing means
# upstream reshaped it and our fix quietly fell away; matching twice means the
# pattern is not the piece it claims to be. Either way the import stops.
unmatched="$(HITS="$STAGE/patch-hits" perl -ne '
  BEGIN {
    open(my $h, "<", $ENV{HITS}) or die "cannot read $ENV{HITS}: $!\n";
    while (<$h>) { chomp; my ($l, $n) = split /\t/; $main::total{$l} += $n; }
  }
  next if /^\s*(#|$)/;
  my @f = split(/\s*\|\s*/, $_, 4);
  next unless @f >= 3;
  for (@f) { s/^\s+//; s/\s+$//; }
  next unless $f[0] eq "patch";
  my $n = $main::total{$.} // 0;
  print "    line $.: matched $n times, want exactly 1 -- $f[1]\n" if $n != 1;
' "$RULES")"
if [ -n "$unmatched" ]; then
  say "expo-gl.sh: a \`patch\` row did not match exactly once:"
  printf '%s\n' "$unmatched" >&2
  die "upstream changed under a patch. Re-read the upstream code and update the row in $RULES."
fi

while IFS="$TAB" read -r from to; do
  [ -n "$from" ] || continue
  [ -f "$SRC_DIR/$from" ] \
    || die "$UPSTREAM_PATH/$from is not in $UPSTREAM_REF -- the \`verbatim\` row for $to is stale."
  vendored_header "$from" > "$NEW/$to"
  cat "$SRC_DIR/$from" >> "$NEW/$to"
  emitted=$((emitted + 1))
done <<EOF
$VERBATIM_RULES
EOF

while IFS="$TAB" read -r from to; do
  [ -n "$from" ] || continue
  [ -f "$SEAM_DIR/$to" ] || die "the seam source tools/vendor/seam/$to is missing."
  # The upstream file must still exist: a seam that no longer replaces anything
  # is a seam that should have been deleted.
  [ -f "$SRC_DIR/$from" ] \
    || die "$UPSTREAM_PATH/$from is not in $UPSTREAM_REF -- the \`seam\` row for $to is stale."
  seam_header "$from" "$to" > "$NEW/$to"
  cat "$SEAM_DIR/$to" >> "$NEW/$to"
  emitted=$((emitted + 1))
done <<EOF
$SEAM_RULES
EOF

[ -f "$SRC_ROOT/$UPSTREAM_LICENSE" ] \
  || die "$UPSTREAM_LICENSE is not at the root of $UPSTREAM_REF -- the licence cannot be carried across."
cp "$SRC_ROOT/$UPSTREAM_LICENSE" "$NEW/LICENSE-expo"

# ---- VENDOR.md --------------------------------------------------------------

{
  cat <<EOF
<!--
GENERATED by tools/vendor/expo-gl.sh. Do not edit this file, or anything beside
it: a fix is a change to tools/vendor/expo-gl.rules and a re-run.
-->

# expo-gl, vendored

This directory is a **tracked port**, not a fork. Everything in it is produced
mechanically by \`tools/vendor/expo-gl.sh\` from one pinned upstream commit, and
every transformation applied is a row in \`tools/vendor/expo-gl.rules\`.

| | |
|---|---|
| Upstream | ${UPSTREAM_REPO%.git} -- \`$UPSTREAM_PATH\` |
| Ref | \`$UPSTREAM_REF\` |
| Commit | \`$UPSTREAM_COMMIT\` |
| Commit date | $UPSTREAM_DATE |
| Licence | MIT, (c) 2015-present 650 Industries, Inc. (aka Expo) -- see \`LICENSE-expo\` |

## Never hand-edit this tree

An edit here is lost at the next import, and \`expo-gl.sh\` refuses to run over
one: it checksums everything it writes into \`.manifest\` and stops, naming the
modified files, rather than overwriting work. To change something in here:

1. change \`tools/vendor/expo-gl.rules\` (or a file under \`tools/vendor/seam/\`);
2. run \`tools/vendor/expo-gl.sh\`;
3. run \`tools/vendor/verify.sh\` and review the diff.

To take a newer expo: \`tools/vendor/expo-gl.sh --ref sdk-59\`, then move
\`UPSTREAM_REF\` / \`UPSTREAM_COMMIT\` at the top of the script to match.

## What was changed, and why

- **Namespace.** \`expo::gl_cpp\` becomes \`screenkit::gl\`, closing comments
  included, so a symbol in a backtrace names the tree it came from.
- **Prefix.** expo's \`EX\` prefix becomes \`SK\`, in symbols and in file names.
  The rewrite is anchored per identifier family rather than a blanket
  \`s/EX/SK/\`, which would quietly corrupt \`GL_EXT_*\` extension names.
- **GL headers.** Upstream's \`pch.h\` reaches for \`<OpenGLES/EAGL.h>\` and
  \`<OpenGLES/ES3/gl.h>\` under \`__APPLE__\` -- Apple's deprecated EAGL GLES,
  not ANGLE. ANGLE spells the same API the way upstream already spells it on
  Android, so the platform branch collapses onto an unconditional
  \`<GLES3/gl3.h>\` plus \`<GLES2/gl2ext.h>\` (ANGLE ships no
  \`GLES3/gl3ext.h\`). This is the transformation whose silent failure costs the
  most -- it compiles, then fights ANGLE at link or run time -- so
  \`tools/vendor/verify.sh\` fails the run on any surviving \`OpenGLES/\` include.
- **Logging.** \`EXGLSysLog\` reached \`EXiOSLog\`, which is implemented outside
  \`common/\` and therefore outside this import: vendoring that header unchanged
  would compile and then fail to link. It now lands on \`screenkit::log\`
  (\`runtime/core/include/screenkit/Log.h\`).
- **Context management is left behind.** \`EXGLContextManager.cpp\` and
  \`EXGLNativeApi.cpp\` are not imported. This runtime already owns the display,
  context and surface in \`runtime/core/src/gfx/GlSurface\`, and a second owner
  of context lifetime is the seam to adapt rather than import. What remains are
  three small seam files, marked as ScreenKit's in their own headers:
  \`SKGLTypes.h\` (the two id typedefs), \`SKGLContextSeam.h\` (the id -> context
  lookup, **declared and not defined**) and \`SKPlatformUtils.h\` (the log macro).

## Status

This tree is the GL path. It compiles as \`screenkit-gl-vendored\` against
ANGLE's headers and is linked into \`screenkit-core\`, whose
\`runtime/core/src/gfx/VendoredWebGL.cpp\` installs the
\`WebGLRenderingContext\` as \`gl\` and defines every function
\`SKGLContextSeam.h\` declares. The archive leaves those symbols undefined on
purpose, so nothing here can link without the runtime that owns the context.

## Patches

Behaviour changes to upstream methods, each a \`patch\` row in the rules table.
A \`patch\` must match exactly once, or the import stops.

EOF

  perl -ne '
    next if /^\s*(#|$)/;
    my @f = split(/\s*\|\s*/, $_, 4);
    next unless @f >= 4;
    for (@f) { s/^\s+//; s/\s+$//; }
    next unless $f[0] eq "patch";
    print "- $f[3] (rules line $.)\n";
  ' "$RULES"

  cat <<EOF

## Files

| File | Upstream | |
|---|---|---|
EOF

  while IFS="$TAB" read -r from to; do
    [ -n "$from" ] || continue
    printf '| `%s` | `%s` | rewritten |\n' "$to" "$from"
  done <<RULEROWS
$FILE_RULES
RULEROWS
  while IFS="$TAB" read -r from to; do
    [ -n "$from" ] || continue
    printf '| `%s` | `%s` | verbatim |\n' "$to" "$from"
  done <<RULEROWS
$VERBATIM_RULES
RULEROWS
  while IFS="$TAB" read -r from to; do
    [ -n "$from" ] || continue
    printf '| `%s` | `%s` | **seam** -- ours, replacing upstream'"'"'s |\n' "$to" "$from"
  done <<RULEROWS
$SEAM_RULES
RULEROWS

  printf '\nNot imported:\n\n'
  while IFS="$TAB" read -r from to; do
    [ -n "$from" ] || continue
    printf -- '- `%s`\n' "$from"
  done <<RULEROWS
$SKIP_RULES
RULEROWS
} > "$NEW/VENDOR.md"

# ---- the manifest -----------------------------------------------------------
# This project has no VCS yet, so "is the output tree untouched?" cannot be
# asked of git. It is asked of a checksum of every file this script wrote, which
# answers the same in a checkout, a tarball or CI.

{
  printf '# GENERATED by tools/vendor/expo-gl.sh -- sha256 of every file it wrote.\n'
  printf '# A mismatch means this tree was hand-edited. Fix the script, not the tree.\n'
  printf '# repo    %s\n' "$UPSTREAM_REPO"
  printf '# ref     %s\n' "$UPSTREAM_REF"
  printf '# commit  %s\n' "$UPSTREAM_COMMIT"
  printf '# path    %s\n' "$UPSTREAM_PATH"
  printf '# date    %s\n' "$UPSTREAM_DATE"
  ( cd "$NEW" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) | while IFS= read -r rel; do
    printf '%s  %s\n' "$(sha256 "$NEW/$rel")" "$rel"
  done
} > "$STAGE/manifest"
cp "$STAGE/manifest" "$NEW/.manifest"

# ---- refuse to clobber ------------------------------------------------------

modified=""
unknown=""
if [ -d "$OUT_DIR" ]; then
  if [ -f "$OUT_DIR/.manifest" ]; then
    while IFS= read -r line; do
      case "$line" in '#'*|'') continue ;; esac
      want="${line%%  *}"
      rel="${line#*  }"
      [ -f "$OUT_DIR/$rel" ] || continue   # missing is fine: we are about to write it
      if [ "$(sha256 "$OUT_DIR/$rel")" != "$want" ]; then
        modified="$modified$rel
"
      fi
    done < "$OUT_DIR/.manifest"

    # A file nobody recorded is either hand-added or the leftover of an older
    # import; swapping the tree out from under it is the same kind of loss.
    grep -v '^#' "$OUT_DIR/.manifest" | sed 's/^[0-9a-f]*  //' | LC_ALL=C sort > "$STAGE/recorded"
    ( cd "$OUT_DIR" && find . -type f ! -name '.manifest' | sed 's|^\./||' ) \
      | LC_ALL=C sort > "$STAGE/present"
    unknown="$(comm -13 "$STAGE/recorded" "$STAGE/present")"
  elif [ -n "$(ls -A "$OUT_DIR" 2>/dev/null || true)" ]; then
    unknown="(the whole tree -- there is no .manifest, so nothing here can be vouched for)"
  fi
fi

if [ -n "$modified" ] || [ -n "$unknown" ]; then
  if [ "$FORCE" -eq 0 ]; then
    say "expo-gl.sh: refusing to overwrite local changes in $OUT_DIR"
    if [ -n "$modified" ]; then printf '%s' "$modified" | grep . | sed 's/^/    modified:   /' >&2; fi
    if [ -n "$unknown" ];  then printf '%s\n' "$unknown" | grep . | sed 's/^/    unrecorded: /' >&2; fi
    say ""
    say "  This tree is generated. Move the edit into tools/vendor/expo-gl.rules"
    say "  (or tools/vendor/seam/) and re-run, or pass --force to discard it."
    exit 1
  fi
  say ">>> --force: discarding local changes in $OUT_DIR"
  DISCARDED=1
fi

# ---- report, then swap ------------------------------------------------------

if [ -f "$OUT_DIR/.manifest" ]; then
  grep -v '^#' "$OUT_DIR/.manifest" | LC_ALL=C sort > "$STAGE/was" || true
else
  : > "$STAGE/was"
fi
grep -v '^#' "$STAGE/manifest" | LC_ALL=C sort > "$STAGE/is"

sed 's/^[0-9a-f]*  //' "$STAGE/was" | LC_ALL=C sort > "$STAGE/was-names"
sed 's/^[0-9a-f]*  //' "$STAGE/is"  | LC_ALL=C sort > "$STAGE/is-names"

added="$(comm -13 "$STAGE/was-names" "$STAGE/is-names")"
removed="$(comm -23 "$STAGE/was-names" "$STAGE/is-names")"
written="$(comm -13 "$STAGE/was" "$STAGE/is" | sed 's/^[0-9a-f]*  //')"

count() { [ -z "$1" ] && printf '0' || printf '%s' "$(printf '%s\n' "$1" | grep -c .)"; }

if [ "$CHECK_ONLY" -eq 1 ]; then
  if [ -n "$written" ] || [ -n "$removed" ]; then
    say "expo-gl.sh: $OUT_DIR is not what $UPSTREAM_REF @ ${UPSTREAM_COMMIT:0:12} produces:"
    if [ -n "$written" ]; then printf '%s\n' "$written" | sed 's/^/    would write:  /' >&2; fi
    if [ -n "$removed" ]; then printf '%s\n' "$removed" | sed 's/^/    would remove: /' >&2; fi
    say ""
    say "  Run: tools/vendor/expo-gl.sh"
    exit 1
  fi
  say ">>> $OUT_DIR is current ($UPSTREAM_REF @ ${UPSTREAM_COMMIT:0:12}, $emitted files)"
  exit 0
fi

# The swap is last, and is one move: everything above can fail without the
# output tree ever being half-written.
mkdir -p "$(dirname "$OUT_DIR")"
rm -rf "$OUT_DIR.new" "$OUT_DIR.old"
cp -R "$NEW" "$OUT_DIR.new"
if [ -d "$OUT_DIR" ]; then
  mv "$OUT_DIR" "$OUT_DIR.old"
fi
mv "$OUT_DIR.new" "$OUT_DIR"
rm -rf "$OUT_DIR.old"

say ">>> $OUT_DIR: $emitted files from $UPSTREAM_REF @ ${UPSTREAM_COMMIT:0:12}"
if [ "$DISCARDED" -eq 1 ]; then
  # The manifests match because the *recorded* checksums did; the files on disk
  # did not, which is why --force was needed. Say that, rather than "unchanged".
  say "    local changes discarded; the tree is again what this import produces"
elif [ -z "$written" ] && [ -z "$removed" ]; then
  say "    unchanged -- byte-identical to what was already there"
else
  say "    $(count "$added") added, $(count "$removed") removed, $(count "$written") written in total"
  if [ -n "$written" ]; then printf '%s\n' "$written" | sed 's/^/      /' >&2; fi
  if [ -n "$removed" ]; then printf '%s\n' "$removed" | sed 's/^/      (gone) /' >&2; fi
fi
say ""
say "    now run: tools/vendor/verify.sh"

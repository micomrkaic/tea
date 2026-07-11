#!/bin/sh
# update.sh — deploy a tea release tarball, end to end:
#
#   ./tools/update.sh ~/Downloads/tea-vX.Y.Z.tar.gz
#
#   1. extracts over this repo (layout auto-detected; never tea/tea/)
#   2. make clean && make && make test   — your machine re-verifies the
#      42/42 byte-identical promise before anything is committed
#   3. shows git status, asks once for confirmation
#   4. commits, tags vVERSION (from the VERSION file), pushes both
#
# The tarball already contains the freshly built wasm and PDF docs in
# docs/, so this machine only builds and verifies the native binary —
# no emcc or LaTeX needed here.
#
# Notes: extraction OVERLAYS files (never deletes removed ones — do
# those by hand when a release says so).  If the tag already exists
# (a re-ship without a version bump), the script commits and pushes
# without re-tagging and tells you so.
set -eu

[ $# -eq 1 ] || { echo "usage: $0 path/to/tea-vX.Y.Z.tar.gz" >&2; exit 1; }
TARBALL=$1
[ -f "$TARBALL" ] || { echo "no such file: $TARBALL" >&2; exit 1; }

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -f "$ROOT/Makefile" ] && [ -d "$ROOT/src" ] || {
  echo "refusing: $ROOT does not look like the tea repo" >&2; exit 1; }
cd "$ROOT"

TOP=$(tar tzf "$TARBALL" | head -1 | cut -d/ -f1)
case "$TOP" in
  tea)          STRIP=1 ;;
  src|Makefile) STRIP=0 ;;
  *) echo "refusing: unexpected tarball top-level '$TOP'" >&2; exit 1 ;;
esac
echo "== extracting $(basename "$TARBALL") (strip=$STRIP)"
tar xzf "$TARBALL" --strip-components=$STRIP

VER=$(cat VERSION)
echo "== building and verifying v$VER natively"
make clean >/dev/null
make
make test

echo
echo "== git status (review before shipping)"
git status --short | head -30
echo
printf 'commit, tag v%s, and push? [y/N] ' "$VER"; read -r a
[ "$a" = y ] || { echo "stopped: extracted+verified, nothing committed"; exit 0; }

git add -A
if [ -n "$(git status --porcelain)" ]; then
  git commit -m "release v$VER"
else
  echo "(no changes to commit — repo already at this state)"
fi
if git rev-parse "v$VER" >/dev/null 2>&1; then
  echo "(tag v$VER already exists — pushing without re-tagging)"
  git push origin master
else
  git tag "v$VER"
  git push origin master "v$VER"
fi
echo "== shipped v$VER"

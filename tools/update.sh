#!/bin/sh
# update.sh — extract a tea release tarball over this repo, safely.
#
#   ./tools/update.sh ~/Downloads/tea-vX.Y.Z.tar.gz
#
# Works no matter where you run it from and no matter whether the
# tarball has a top-level tea/ directory (all official ones do): it
# detects the layout, extracts into the repo root exactly once (never
# tea/tea/), and finishes by showing git status so you can see what
# actually changed before committing.
#
# Note: extraction OVERLAYS files.  It never deletes files that a newer
# release removed; if a release notes removals, delete those by hand.
set -eu

[ $# -eq 1 ] || { echo "usage: $0 path/to/tea-vX.Y.Z.tar.gz" >&2; exit 1; }
TARBALL=$1
[ -f "$TARBALL" ] || { echo "no such file: $TARBALL" >&2; exit 1; }

# repo root = parent of the directory this script lives in
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -f "$ROOT/Makefile" ] && [ -d "$ROOT/src" ] || {
  echo "refusing: $ROOT does not look like the tea repo" >&2; exit 1; }

# inspect the tarball's top level
TOP=$(tar tzf "$TARBALL" | head -1 | cut -d/ -f1)
case "$TOP" in
  tea)  STRIP=1 ;;                       # official layout: tea/...
  src|Makefile) STRIP=0 ;;               # bare layout, just in case
  *) echo "refusing: unexpected tarball top-level '$TOP'" >&2; exit 1 ;;
esac

echo "extracting $(basename "$TARBALL") into $ROOT (strip=$STRIP)"
tar xzf "$TARBALL" -C "$ROOT" --strip-components=$STRIP

echo
echo "== git status (read this before committing) =="
git -C "$ROOT" status --short | head -25
echo
echo "next:  cd $ROOT && make clean && make && make test"

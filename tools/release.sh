#!/bin/sh
# release.sh — one-command release, everything versioned from ./VERSION.
#
#   1. edit VERSION (e.g. 1.4.0)
#   2. ./tools/release.sh
#
# Runs the test suite, rebuilds wasm + all docs, stamps the splash,
# syncs docs/, commits, tags vVERSION, and pushes.  Refuses to reuse an
# existing tag.  (Replaces the old deploy_v1.1.0.sh.)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
VER=$(cat VERSION)
git rev-parse "v$VER" >/dev/null 2>&1 && {
  echo "tag v$VER already exists — bump VERSION first" >&2; exit 1; }

make clean >/dev/null
make
make test
make wasm
make sync-web-version
make manual
make docs-pdf
make quickstart

cp web/index.html web/tea.js web/tea.wasm web/xterm.min.js \
   web/xterm.min.css web/tea_logo.jpg web/lineeditor.js docs/
cp tea-manual.pdf tea-manual.md STATA-QUICKSTART.pdf STATA-QUICKSTART.md \
   README.pdf COMPATIBILITY.pdf KNOWN_BUGS.pdf docs/

git add -A
git status --short | head -25
printf 'commit and tag v%s? [y/N] ' "$VER"; read -r a
[ "$a" = y ] || { echo "aborted (nothing committed)"; exit 1; }
git commit -m "release v$VER"
git tag "v$VER"
git push origin master "v$VER"
make dist
echo "released v$VER — tarball: tea-v$VER.tar.gz"

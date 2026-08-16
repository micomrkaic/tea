#!/bin/sh
# update.sh — deploy a tea release tarball, end to end:
#
#   ./tools/update.sh ~/Downloads/tea-vX.Y.Z.tar.gz
#
#   1. re-execs itself from /tmp (the tarball overwrites this very
#      file during extraction; a shell reads scripts incrementally,
#      so running the repo copy while replacing it is a live hazard)
#   2. syncs with origin: fast-forwards a merely-behind master, and
#      refuses a diverged one with recovery advice
#   3. extracts over this repo (layout auto-detected; never tea/tea/)
#   4. make clean && make && make test — your machine re-verifies the
#      byte-identical promise before anything is committed
#   5. shows git status, asks once for confirmation
#   6. commits, tags, and pushes master FIRST and the tag second —
#      chained, so a rejected branch push can never leave a published
#      tag pointing at unpublished history
#
# The tarball already contains the freshly built wasm and PDF docs in
# docs/, so this machine only builds and verifies the native binary —
# no emcc or LaTeX needed here.
#
# Notes: extraction OVERLAYS files (never deletes removed ones — do
# those by hand when a release says so).  If the tag already exists
# (a re-ship without a version bump), the script pushes without
# re-tagging and tells you so.
set -eu

# ---- 1. self-update hazard: run from a copy outside the repo -------
if [ -z "${TEA_UPDATE_REEXEC:-}" ]; then
    cp "$0" "/tmp/tea_update_reexec_$$.sh"
    TEA_UPDATE_REEXEC="/tmp/tea_update_reexec_$$.sh" \
        exec sh "/tmp/tea_update_reexec_$$.sh" "$@"
fi
trap 'rm -f "$TEA_UPDATE_REEXEC"' EXIT

[ $# -eq 1 ] || { echo "usage: $0 path/to/tea-vX.Y.Z.tar.gz" >&2; exit 1; }
TARBALL=$1
[ -f "$TARBALL" ] || { echo "no such file: $TARBALL" >&2; exit 1; }

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# when re-exec'd from /tmp, $0 no longer locates the repo — recover it
[ -f "$ROOT/Makefile" ] && [ -d "$ROOT/src" ] || ROOT=$PWD
[ -f "$ROOT/Makefile" ] && [ -d "$ROOT/src" ] || {
  echo "refusing: cannot locate the tea repo (run from inside it)" >&2; exit 1; }
cd "$ROOT"

# ---- 2. two-machine guard ------------------------------------------
# Releases are whole-tree imports, so the base commit barely matters
# to CONTENT — but it decides whether the push is a fast-forward.  A
# machine that has not pulled since the last release must sync first.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1 && \
   git remote get-url origin >/dev/null 2>&1; then
  echo "== syncing with origin"
  git fetch origin
  if git merge-base --is-ancestor origin/master master; then
      : # up to date (or ahead): proceed
  elif git merge-base --is-ancestor master origin/master; then
      echo "== local master is behind origin — fast-forwarding"
      git merge --ff-only origin/master
  else
      echo "!! local master and origin/master have DIVERGED." >&2
      echo "   releases are whole-tree imports, so recover with:" >&2
      echo "     git reset --hard origin/master   # then re-run update.sh" >&2
      echo "   (any local commit's content is re-imported by the tarball)" >&2
      exit 1
  fi
fi

# ---- 3. extract ------------------------------------------------------
TOP=$(tar tzf "$TARBALL" | head -1 | cut -d/ -f1)
case "$TOP" in
  tea)          STRIP=1 ;;
  src|Makefile) STRIP=0 ;;
  *) echo "refusing: unexpected tarball top-level '$TOP'" >&2; exit 1 ;;
esac
echo "== extracting $(basename "$TARBALL") (strip=$STRIP)"
tar xzf "$TARBALL" --strip-components=$STRIP

# ---- 4. verify -------------------------------------------------------
VER=$(cat VERSION)
echo "== building and verifying v$VER natively"
make clean >/dev/null
make
make test

# ---- 5. review + confirm --------------------------------------------
echo
echo "== git status (review before shipping)"
git status --short | head -30
echo
printf 'commit, tag v%s, and push? [y/N] ' "$VER"
# read from the TERMINAL: stdin may have been consumed upstream, which
# silently turned every 'y' into a decline
read -r a < /dev/tty || a=n
[ "$a" = y ] || { echo "stopped: extracted+verified, nothing committed"; exit 0; }

# ---- 6. commit, tag, push (master first, tag second) -----------------
git add -A
if [ -n "$(git status --porcelain)" ]; then
  git commit -m "release v$VER"
else
  echo "(no changes to commit — repo already at this state)"
fi
if git rev-parse "v$VER" >/dev/null 2>&1; then
  echo "(tag v$VER already exists — pushing without re-tagging)"
else
  git tag "v$VER"
fi
git push origin master && git push origin "v$VER"
echo "== shipped v$VER"

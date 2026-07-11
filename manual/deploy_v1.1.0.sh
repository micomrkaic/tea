#!/bin/sh
# deploy_v1.1.0.sh — publish tea v1.1.0 to GitHub from the release tarball.
#
# Usage:
#   ./deploy_v1.1.0.sh /path/to/tea-v1.1-plots-wasm.tar.gz [/path/to/your/tea/clone]
#
# What it does, in order (aborting at the first failure):
#   1. sanity-checks the clone and warns about uncommitted changes
#   2. unpacks the tarball over the working tree (git history untouched)
#   3. builds and runs the regression suite — refuses to publish on any failure
#   4. commits, tags v1.1.0, pushes branch + tag
#   5. creates a GitHub release (via `gh` if installed, else prints the manual step)
#   6. optionally publishes web/ to a gh-pages branch for the browser demo
#
# Requirements: git with push access to the repo; optionally the GitHub CLI
# (`gh auth login` done) for automatic release creation.

set -eu

VERSION="v1.1.0"
TARBALL="${1:?usage: $0 tarball.tar.gz [repo-dir]}"
REPO="${2:-.}"

TARBALL=$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")
[ -f "$TARBALL" ] || { echo "error: tarball not found: $TARBALL" >&2; exit 1; }

cd "$REPO"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || { echo "error: $REPO is not a git repository" >&2; exit 1; }

if git rev-parse "$VERSION" >/dev/null 2>&1; then
    echo "error: tag $VERSION already exists — delete it first if you mean to re-release:" >&2
    echo "       git tag -d $VERSION && git push origin :refs/tags/$VERSION" >&2
    exit 1
fi

if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    echo "warning: working tree has uncommitted changes; they will be mixed"
    echo "         into the release commit.  Ctrl-C now to abort, Enter to continue."
    read -r _
fi

echo "==> unpacking $TARBALL over $(pwd)"
tar xzf "$TARBALL" --strip-components=1

echo "==> building"
make clean >/dev/null 2>&1 || true
make

echo "==> regression suite (native)"
make test         # exits non-zero (and so do we) unless 40/40

echo "==> regression suite (ASan + UBSan)"
make debug >/dev/null
TEA=./tea-debug ./tests/regression/run_tests.sh

echo "==> committing and tagging"
git add -A
git commit -m "tea v1.1.0: SVG plotting, WebAssembly build, backend-independent output

- scatter/line/histogram with a dependency-free SVG renderer
- push-mode session API (TeaSession); REPL is a thin driver over it
- WebAssembly build (make wasm): full tea in the browser via xterm.js,
  reference CLAPACK/BLAS backend, MEMFS file upload/download, inline plots
- backend-independent output: mathematical zeros in degenerate fits are
  snapped so the same do-file prints byte-identical results on every
  machine and BLAS (see README, Semantics decisions)
- regression suite: 40 tests, passing byte-identically under native
  gcc+OpenBLAS (release and sanitizer builds) and WASM clang+reference-BLAS"
git tag -a "$VERSION" -m "tea $VERSION"

BRANCH=$(git rev-parse --abbrev-ref HEAD)
echo "==> pushing $BRANCH and $VERSION"
git push origin "$BRANCH"
git push origin "$VERSION"

echo "==> GitHub release"
if command -v gh >/dev/null 2>&1; then
    gh release create "$VERSION" "$TARBALL" \
        --title "tea $VERSION" \
        --notes "SVG plotting (scatter/line/histogram, zero dependencies), a full WebAssembly build that runs tea in the browser, and backend-independent output: the same do-file now prints byte-identical results on every machine and BLAS library (see README, Semantics decisions).  40/40 regression tests pass identically under native OpenBLAS and WASM reference BLAS."
    echo "release created: $(gh release view "$VERSION" --json url -q .url)"
else
    echo "gh CLI not found — create the release manually:"
    echo "  https://github.com/micomrkaic/tea/releases/new?tag=$VERSION"
    echo "  and attach: $TARBALL"
fi

printf "\nPublish the browser demo to GitHub Pages (gh-pages branch)? [y/N] "
read -r ans
if [ "$ans" = "y" ] || [ "$ans" = "Y" ]; then
    [ -f web/tea.wasm ] || { echo "error: web/tea.wasm missing from tarball" >&2; exit 1; }
    TMP=$(mktemp -d)
    cp web/index.html web/tea.js web/tea.wasm "$TMP"/
    git worktree add --detach "$TMP/gp" >/dev/null 2>&1 || true
    (
      cd "$TMP/gp"
      git checkout --orphan gh-pages 2>/dev/null || git checkout gh-pages
      git rm -rf . >/dev/null 2>&1 || true
      cp "$TMP"/index.html "$TMP"/tea.js "$TMP"/tea.wasm .
      git add index.html tea.js tea.wasm
      git commit -m "tea $VERSION browser demo" >/dev/null
      git push -f origin gh-pages
    )
    git worktree remove --force "$TMP/gp" >/dev/null 2>&1 || true
    rm -rf "$TMP"
    echo "demo pushed.  Enable Pages once (Settings → Pages → branch: gh-pages, / root)"
    echo "then it will be live at:  https://micomrkaic.github.io/tea/"
fi

echo
echo "done — tea $VERSION is published."

#!/bin/sh
# tea regression test harness.
#
# Each test consists of:
#   <name>.do       — the test do-file (run with ./tea)
#   <name>.expected — the expected stdout (verbatim)
#   <name>.flags    — (optional) extra CLI args passed to tea (e.g.
#                     '--tea-extensions') — read whole-file, split on
#                     whitespace
#
# This script runs every <name>.do, captures stdout, and diffs against
# <name>.expected.  Failures print the unified diff and the overall pass/
# fail summary at the end.  Exit code is 0 on all-pass, 1 otherwise.

set -e

TEA="${TEA:-./tea}"
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$TEA" ]; then
    echo "tea binary not found at $TEA — run 'make' first or set TEA=path/to/tea" >&2
    exit 2
fi

pass=0
fail=0
fail_names=""

for test_do in "$DIR"/*.do; do
    [ -e "$test_do" ] || continue
    name=$(basename "$test_do" .do)
    expected="$DIR/$name.expected"
    flags_file="$DIR/$name.flags"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected)"
        continue
    fi
    flags=""
    if [ -f "$flags_file" ]; then
        flags=$(cat "$flags_file")
    fi
    # shellcheck disable=SC2086
    actual=$(cd "$(dirname "$TEA")" && "$TEA" $flags "$test_do" 2>&1 || true)
    expected_content=$(cat "$expected")
    if [ "$actual" = "$expected_content" ]; then
        echo "PASS $name"
        pass=$((pass+1))
    else
        echo "FAIL $name"
        tmp=$(mktemp)
        printf '%s\n' "$actual" > "$tmp"
        diff -u "$expected" "$tmp" | sed 's/^/    /'
        rm -f "$tmp"
        fail=$((fail+1))
        fail_names="$fail_names $name"
    fi
done

total=$((pass+fail))
echo ""
echo "tea regression: $pass/$total passed"
if [ "$fail" -gt 0 ]; then
    echo "  failed:$fail_names"
    exit 1
fi
exit 0

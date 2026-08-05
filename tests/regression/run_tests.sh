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
skip=0
fail_names=""

# TEA_TIER=decaf: skip any test whose do-file invokes a command the
# decaf binary doesn't have.  The available-command list comes from the
# binary itself (help _list), so the filter can never drift from the
# build: adding a command to decaf automatically admits its tests.
TIER_AVAIL=""
if [ "${TEA_TIER:-}" = "decaf" ]; then
    _hl=$(mktemp); printf 'help _names\n' > "$_hl"
    TIER_AVAIL="$("$TEA" "$_hl" 2>/dev/null)"
    rm -f "$_hl"
fi

tier_skip() {
    [ -n "$TIER_AVAIL" ] || return 1
    awk '
        { line=$0
          sub(/^[ \t]+/,"",line)
          if (line=="" || line ~ /^\*/ || line ~ /^\/\//) next
          while (1) {
              if (line ~ /^(capture|quietly|qui|noisily)[ \t]+/) {
                  sub(/^[a-z]+[ \t]+/,"",line); continue }
              if (line ~ /^by(sort)?[ \t][^:]*:[ \t]*/) {
                  sub(/^by(sort)?[ \t][^:]*:[ \t]*/,"",line); continue }
              break
          }
          n=split(line, w, /[ \t,]/)
          if (n>0 && w[1] != "") print w[1]
        }' "$1" | sort -u > /tmp/tea_tier_cmds.$$
    missing=0
    while read -r cmd; do
        case "$cmd" in
            assert|shell|display|di|local|global|foreach|forvalues|while|if|else|set|end|program) continue ;;
            \}*|\{*|\!*|\#*) continue ;;
        esac
        if ! printf '%s\n' "$TIER_AVAIL" | grep -qx "$cmd"; then
            missing=1; break
        fi
    done < /tmp/tea_tier_cmds.$$
    rm -f /tmp/tea_tier_cmds.$$
    [ "$missing" = 1 ]
}

for test_do in "$DIR"/*.do; do
    [ -e "$test_do" ] || continue
    name_only=$(basename "$test_do" .do)
    if [ "${TEA_TIER:-}" = "decaf" ] && grep -qx "$name_only" "$DIR/decaf_skip.txt" 2>/dev/null; then
        skip=$((skip+1)); continue
    fi
    if [ "${TEA_TIER:-}" = "decaf" ] && tier_skip "$test_do"; then
        skip=$((skip+1)); continue
    fi
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
[ "$skip" -gt 0 ] && echo "  ($skip skipped: commands not in this tier)"
if [ "$fail" -gt 0 ]; then
    echo "  failed:$fail_names"
    exit 1
fi
exit 0

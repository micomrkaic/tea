#!/bin/sh
# compare.sh — diff the numeric content of tea's and Stata's outputs.
#
# Usage:
#   sh compare.sh tea_output.txt stata_output.log
#
# Strategy: both tea and Stata print regression tables, summarize tables,
# and "display" lines.  Whitespace and column alignment differ.  This
# script normalizes whitespace and shows a side-by-side diff of the lines
# that should match.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 tea_output.txt stata_output.log" >&2
    exit 2
fi
tea=$1
stata=$2

if [ ! -r "$tea" ];   then echo "cannot read $tea"   >&2; exit 1; fi
if [ ! -r "$stata" ]; then echo "cannot read $stata" >&2; exit 1; fi

# Normalize: collapse runs of whitespace to single spaces, strip leading/trailing
# whitespace, drop completely blank lines.  Helps focus on substance.
normalize() {
    sed -e 's/[[:space:]]\{1,\}/ /g' -e 's/^ //' -e 's/ $//' "$1" | grep -v '^$'
}

t_norm=$(mktemp)
s_norm=$(mktemp)
trap 'rm -f "$t_norm" "$s_norm"' EXIT

normalize "$tea"   > "$t_norm"
normalize "$stata" > "$s_norm"

echo "==> normalized line counts:  tea=$(wc -l < "$t_norm")  stata=$(wc -l < "$s_norm")"
echo

# Side-by-side diff so it's easy to eyeball coefficient differences.
# Use diff -y if available; fall back to a plain diff.
if diff --version >/dev/null 2>&1; then
    diff -y --suppress-common-lines -W 200 "$t_norm" "$s_norm" || true
else
    diff "$t_norm" "$s_norm" || true
fi

echo
echo "==> tip: focus on lines containing numeric values for coefficients,"
echo "    SEs, F-stats, R-squared, and 'Number of obs'.  Formatting"
echo "    differences (column widths, log timestamps) are expected."

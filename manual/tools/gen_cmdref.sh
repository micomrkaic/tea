#!/bin/sh
# gen_cmdref.sh — generate the manual's command-reference appendix by
# interrogating the tea binary itself, so the reference cannot drift from
# the implementation.  Output: manual/reference.md (included by the manual
# build).  Run from the repo root after any command change:
#
#     ./tools/gen_cmdref.sh
#
set -eu
TEA=${TEA:-./tea}
OUT=manual/reference.md
mkdir -p manual

[ -x "$TEA" ] || { echo "build tea first (make)" >&2; exit 1; }

# command list: the two-column index from `help`, flattened
CMDS=$(printf 'help\n' | "$TEA" -q - 2>/dev/null \
       | sed -n '/available commands:/,/native statements/p' \
       | grep -v 'available commands\|native statements' \
       | tr -s ' ' '\n' | grep -v '^$')

{
  echo '# Command reference'
  echo
  echo '*Generated from the tea binary by `tools/gen_cmdref.sh` — this text is'
  echo 'exactly what `help CMD` prints at the prompt, and regenerating it after'
  echo 'any change keeps the manual and the implementation in agreement.*'
  echo
  for c in $CMDS; do
    txt=$(printf 'help %s\n' "$c" | "$TEA" -q - 2>/dev/null | sed '/^$/d')
    [ -n "$txt" ] || continue
    echo "## \`$c\`"
    echo
    echo '```'
    echo "$txt"
    echo '```'
    echo
  done
  echo '## Native statements'
  echo
  echo 'Handled by the interpreter before command dispatch:'
  echo '`display`, `assert`, `shell` (and the `!cmd` escape), `#delimit`,'
  echo '`local`, `global`, `foreach`, `forvalues`, `while`, `if`/`else`,'
  echo '`capture`, `quietly`, `program define`, and comments (`*`, `//`, `///`).'
} > "$OUT"

n=$(grep -c '^## `' "$OUT")
echo "wrote $OUT: $n commands documented"

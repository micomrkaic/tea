* 44_else_confirm — else branches across lines, confirm, rc propagation.
capture confirm file /tmp/tea_no_such_file_44
display "confirm missing file rc: " _rc
capture confirm new file /tmp/tea_no_such_file_44
display "confirm new missing rc: " _rc

set obs 3
gen x = _n
capture confirm variable x
display "confirm var rc: " _rc
capture confirm variable ghost
display "confirm ghost rc: " _rc
capture confirm numeric variable x
display "confirm numeric rc: " _rc

* the if / } / else { layout from real do-files
local flag = 1
if `flag' {
  display "then-branch"
}
else {
  display "else-branch (must not print)"
}

local flag = 0
if `flag' {
  display "then-branch (must not print)"
}
else if `flag' == 99 {
  display "else-if branch (must not print)"
}
else {
  display "final else-branch"
}

* an error inside a block aborts the do-file (was silently swallowed)
if 1 {
  sysuse nosuchdata
}
display "unreachable"

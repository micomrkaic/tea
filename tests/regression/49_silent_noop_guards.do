* Regression: unknown variables in grouping/format varlists must error (111),
* never degrade into silent ungrouped/no-op behavior.
* Bugs guarded here (all shared the varlist_expand -1 == "empty list" flaw):
*   by badvar: cmd      -> ran UNGROUPED over the full data
*   egen ..., by(badvar) -> computed ungrouped statistics
*   collapse, by(badvar) -> would collapse the whole dataset to one row
*   format %fmt badvar   -> silently did nothing
clear
set obs 4
gen g = 1 + (_n > 2)
gen x = _n
capture by nosuchvar: gen n1 = _N
display _rc
capture egen m = mean(x), by(nosuchvar)
display _rc
capture collapse (mean) x, by(nosuchvar)
display _rc
* data must be intact after the failed collapse
display _N
capture format %9.3f nosuchvar
display _rc
* the valid paths still work
bysort g: gen n2 = _N
egen m2 = mean(x), by(g)
format %9.3f m2
list g x n2 m2

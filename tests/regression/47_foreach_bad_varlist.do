* Regression: `foreach v of varlist ...` with an unknown variable must abort
* with rc=111, not silently execute the loop body zero times.
* Bug: varlist_expand returns -1 for an unknown var; foreach treated -1 as an
* empty list, so rename loops over misnamed columns were silent no-ops and
* scripts failed much later with misleading errors (found replicating the WB
* fertility import: `foreach v of varlist F-BQ` after `import excel, firstrow`).
clear
set obs 3
gen a = _n
gen b = _n*2
foreach v of varlist a-nosuchvar {
    rename `v' x_`v'
}
display "unreachable"

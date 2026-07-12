* Regression: preserve / restore (disk snapshot, single depth) and the
* strtoname() function — the pieces the WB fertility lut block needs:
*   preserve / keep / duplicates drop / gen code_safe=strtoname(...) / restore
clear
set obs 3
gen x = _n
gen str16 nm = strtoname("CC.EST")
display nm[1]
display strtoname("1960")
display strtoname("per allsp.adq_pop tot")
preserve
* double preserve is an error (Stata r(621))
capture preserve
display _rc
drop if x > 1
display _N
restore
display _N
* restore with nothing preserved is an error
capture restore
display _rc
* restore, preserve keeps the snapshot for a second restore
preserve
drop if x > 1
restore, preserve
display _N
drop if x > 2
restore
display _N
* restore, not discards the snapshot without reloading
preserve
drop x
restore, not
display _N
capture confirm variable x
display _rc
* ---- auto-restore: a preserve pending when a do-file concludes (here via
* abort) must be restored automatically, like Stata ----
clear
set obs 5
gen x = _n
tempfile inner
file open fh using `inner', write
file write fh "preserve" _n
file write fh "drop if x > 1" _n
file write fh "nosuchcommand" _n
file close fh
capture do `inner'
display _rc
display _N

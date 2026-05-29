* Regression: time-series operators work in summarize and list (not
* just regress).  Before the fix, "summarize L.x" gave "variable not
* found" because summarize used varlist_expand directly without
* TS-op preprocessing.
clear
set obs 8
gen str1 panel = "A"
gen year = _n
gen x = _n*1.5
xtset panel year

display "--- summarize with TS ops ---"
summarize L.x
display "Expected: 7 obs, mean 6.0 (lag drops first row)"

summarize D.x
display "Expected: 7 obs of 1.5 each (constant first diff)"

summarize L(1/2).x
display "Expected: two rows (L.x with 7 obs, L2.x with 6 obs)"

display "--- list with TS ops ---"
list year x L.x D.x F.x

display "--- list with operator-list ---"
list year x L(1/3).x

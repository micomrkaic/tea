* Regression: try_tsop bug in src/parse.c — the lag-magnitude accumulator
* was initialized to 1 instead of 0, causing L2 to read as L12, L3 as L13,
* F2 as F12, etc. across ALL contexts (expressions, regress, etc.).
* This test pins down correct multi-digit operator behavior in expressions.
clear
set obs 10
gen str1 panel = "A"
gen year = _n
gen y = _n * 10.0
xtset panel year
display "--- L2.y at row 3 should be y[1] = 10, NOT y[-9] = . ---"
gen lag2 = L2.y
list year y lag2
display "Expected: lag2 missing in rows 1-2, lag2=10 at row 3, lag2=20 at row 4, etc."
display ""
display "--- F2.y at row 1 should be y[3] = 30 ---"
gen lead2 = F2.y
list year y lead2
display "Expected: lead2=30 at row 1, lead2=40 at row 2, ..., lead2 missing in rows 9-10."
display ""
display "--- S3.y (seasonal diff, 3 periods) at row 4: y[4]-y[1] = 40-10 = 30 ---"
gen sdiff = S3.y
list year y sdiff
display "Expected: sdiff missing in rows 1-3, sdiff=30 thereafter."

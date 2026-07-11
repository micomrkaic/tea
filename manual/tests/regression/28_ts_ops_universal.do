* Regression: time-series operators work in tabulate, tabstat, and
* collapse — all routed through the shared tsop_expand_varlist helper.
* This locks in the "universal and centralized" behavior: every command
* with a varlist accepts TS ops uniformly.

clear
set obs 12
gen str1 panel = "A"
gen year = _n
gen x = _n * 1.5
gen cat = mod(_n, 3)
xtset panel year

display "=== tabulate L.x ==="
tabulate L.x

display ""
display "=== tabstat with TS-op varlist: x L.x D.x ==="
tabstat x L.x D.x, statistics(mean sd min max)

display ""
display "=== tabstat L(1/3).x — cross product, expect 3 cols with decreasing N ==="
tabstat L(1/3).x, statistics(mean sd n)

display ""
display "=== collapse (mean) L.x by panel ==="
clear
set obs 30
gen str3 cid = "AAA"
replace cid = "BBB" in 11/20
replace cid = "CCC" in 21/30
gen year = mod(_n-1, 10) + 2015
gen x = _n * 1.0
xtset cid year
collapse (mean) L.x, by(cid)
list

display ""
display "=== collapse with cross-product L(1/2).x ==="
clear
set obs 30
gen str3 cid = "AAA"
replace cid = "BBB" in 11/20
replace cid = "CCC" in 21/30
gen year = mod(_n-1, 10) + 2015
gen x = _n * 1.0
xtset cid year
collapse (mean) L(1/2).x, by(cid)
list

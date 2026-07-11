* Regression: xtdescribe (with xtdes abbreviation) summarizes panel
* structure declared by xtset.

clear
set obs 12
gen str1 panel = "A"
replace panel = "B" in 5/8
replace panel = "C" in 9/12
gen year = mod(_n-1, 4) + 2020
gen x = _n
xtset panel year
display "--- balanced panel: 3 panels x 4 periods ---"
xtdescribe

display ""
display "--- xtdes abbreviation should give identical output ---"
xtdes

display ""
display "--- unbalanced panel ---"
clear
set obs 10
gen str3 cid = "USA"
replace cid = "FRA" in 4/7
replace cid = "DEU" in 8/10
gen year = mod(_n-1, 10) + 2020
replace year = 2020 + _n - 1 in 1/3
replace year = 2020 + _n - 4 in 4/7
replace year = 2020 + _n - 8 in 8/10
gen y = _n*1.0
xtset cid year
xtdescribe

display ""
display "--- xtdescribe without xtset should error ---"
clear
set obs 4
gen z = _n
capture xtdescribe
display "rc=" _rc

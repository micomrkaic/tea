* Regression: reshape error messages must diagnose specific mistakes.
clear
set obs 2
gen x = _n
capture reshape long
display "case 1 done"
capture reshape long i(x) j(year)
display "case 2 done"
clear
set obs 2
gen v1980 = 1
gen v1981 = 2
gen id = _n
capture reshape long v
display "case 3 done"

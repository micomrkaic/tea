* Regression: Stata's operator-list time-series syntax in regress varlists.
*   L(1/3).x        -> L1.x L2.x L3.x   (consecutive range)
*   L(1 3).x        -> L1.x L3.x        (explicit list with spaces)
*   L(0/2).x        -> x L.x L2.x       (L0.x means the variable itself)
*   F(1/2).x        -> F1.x F2.x        (leads)
clear
set obs 40
* one panel, 40 consecutive periods
gen str1 panel = "A"
gen year = _n
gen y = 1 + 0.4*_n + sin(_n*0.3)
xtset panel year

display "--- L(1/3).y ---"
regress y L(1/3).y

display ""
display "--- L(1 3).y (skip lag 2) ---"
regress y L(1 3).y

display ""
display "--- L(0/2).y (includes y itself via L0) ---"
* Note: regressing y on itself produces a perfect fit by construction
regress y L(0/2).y

display ""
display "--- F(1/2).y ---"
regress y F(1/2).y

display ""
display "--- mixed: plain var + operator-list ---"
gen yr = year - 20
regress y L(1/2).y yr

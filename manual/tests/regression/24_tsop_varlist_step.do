* Regression: extended TS-op forms.
*   L.(x y)        single op, var-list
*   L2.(x y)       op + explicit lag, var-list
*   L(1/2).(x y)   cross product (op-list × var-list)
*   L(1(2)9).x     step form numlist
*   F(1/2).(x y)   leads cross product
clear
set obs 30
gen str1 panel = "A"
gen year = _n
gen y = 1 + 0.3*_n + sin(_n*0.4)
gen x = 0.5 + 0.2*_n + cos(_n*0.4)
gen z = 0.8 + 0.1*_n + sin(_n*0.6)
xtset panel year

display "=== L.(x z) — bare op, two vars ==="
regress y L.(x z)
display ""
display "=== L2.(x z) — op + lag, two vars ==="
regress y L2.(x z)
display ""
display "=== L(1/2).(x z) — cross product, 4 regressors ==="
regress y L(1/2).(x z)
display ""
display "=== L(1(2)5).x — step form (lags 1, 3, 5) ==="
regress y L(1(2)5).x
display ""
display "=== F(1/2).(x z) — leads cross product ==="
regress y F(1/2).(x z)

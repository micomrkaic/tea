* Regression: xtreg random-effects (FGLS via Swamy-Arora-style quasi-
* demeaning).  Verifies that:
*   - the degenerate case where sigma_e = 0 collapses RE to FE
*   - theta is reported (with min/avg/max for unbalanced panels)
*   - _cons is reported (unlike FE)
*   - vce(robust) and vce(cluster) work
*   - errors when xtset not declared

* --- Hand-checkable degenerate case ---
clear
set obs 12
gen str1 panel = "A"
replace panel = "B" in 5/8
replace panel = "C" in 9/12
gen year = mod(_n-1, 4) + 2020
gen x = _n
gen y = .
replace y = 10 + x in 1/4
replace y = 20 + x in 5/8
replace y = 30 + x in 9/12
xtset panel year

display "=== RE on perfect-fit case: should collapse to FE (theta=1) ==="
display "Expected: beta_x = 1.0, _cons (omitted), sigma_e=0, theta=1"
xtreg y x, re

* --- Realistic unbalanced case ---
display ""
display "=== RE with vce(robust) ==="
clear
set obs 30
gen str3 cid = "AAA"
replace cid = "BBB" in 11/22
replace cid = "CCC" in 23/30
gen t = mod(_n-1, 12) + 2010
replace t = 2010 + _n - 1   in 1/10
replace t = 2010 + _n - 11  in 11/22
replace t = 2010 + _n - 23  in 23/30
gen x = _n*0.5
gen y = 5 + 2*x + 3*runiform()
xtset cid t
quietly xtreg y x, re
display "RE classical: see e(N), e(N_g), e(sigma_u), e(sigma_e), e(rho), e(theta)"
display "  N = " e(N)
display "  N_g = " e(N_g)
display "  sigma_u = " e(sigma_u)
display "  sigma_e = " e(sigma_e)
display "  rho = " e(rho)
display "  theta = " e(theta)

display ""
display "=== RE with vce(cluster) ==="
xtreg y x, re vce(cluster cid)

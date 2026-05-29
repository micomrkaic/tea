* Regression: xtreg fixed-effects (within) estimator.
*
* Hand-verifiable case: 3 panels (A,B,C) × 4 obs each, y = alpha_i + x
* with alpha_A=10, alpha_B=20, alpha_C=30.  Expected exact results:
*   beta_FE   = 1.0
*   R_within  = 1.0
*   R_between = 1.0     (panel means are co-linear)
*   sigma_e   = 0       (perfect within fit)
*   sigma_u   = 10      (sd of alphas = sqrt(((10-20)^2+(20-20)^2+(30-20)^2)/2))
*   rho       = 1       (all variance is from u_i)

clear
set obs 12
gen str1 panel = "A"
replace panel = "B" in 5/8
replace panel = "C" in 9/12
gen year = mod(_n-1, 4) + 2020
gen x = .
replace x = 1  in 1
replace x = 2  in 2
replace x = 3  in 3
replace x = 4  in 4
replace x = 5  in 5
replace x = 6  in 6
replace x = 7  in 7
replace x = 8  in 8
replace x = 9  in 9
replace x = 10 in 10
replace x = 11 in 11
replace x = 12 in 12
gen y = .
replace y = 10 + x in 1/4
replace y = 20 + x in 5/8
replace y = 30 + x in 9/12
xtset panel year

display "=== xtreg y x, fe ==="
xtreg y x, fe

display ""
display "=== xtreg with vce(robust) — same coefficient, different SE ==="
xtreg y x, fe vce(robust)

display ""
display "=== xtreg with vce(cluster panel) — clustered SE ==="
xtreg y x, fe vce(cluster panel)

display ""
display "=== xtreg fails cleanly when xtset not declared ==="
clear
set obs 6
gen z = _n
gen w = _n*2
capture xtreg z w, fe
display "rc=" _rc

display ""
display "=== xtreg rejects ,re and ,be (not yet implemented) ==="
clear
set obs 12
gen str1 p = "A"
replace p = "B" in 7/12
gen t = mod(_n-1,6)+2020
gen a = _n
gen b = _n*2
xtset p t
capture xtreg a b, re
display "rc=" _rc
capture xtreg a b, be
display "rc=" _rc

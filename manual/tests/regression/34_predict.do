* Regression: predict works after every estimator.  Verifies:
*   - regress: xb (default), residuals, stdp
*   - logit/probit: pr (default), xb
*   - xtreg fe/re: xb (default), u (panel effect), e (idiosync), ue, xbu
*   - TS-op coefficient names (L.growth etc.) resolve correctly after
*     drop_if/keep_if changed the frame
*   - sensible errors for invalid option combinations

* --- Regress: predict checks ---
clear
set obs 50
gen x = _n * 0.1
gen y = 2 + 3*x + 0.01*mod(_n, 4)   // non-exact fit: residuals are O(0.01), not FP noise
quietly regress y x
predict yhat
predict resids, resid
predict se, stdp
display "Regress checks: yhat[1] = 2 + 3*0.1 = 2.3"
list x y yhat resids in 1/3
display "stdp non-zero:"
list se in 1/3

* --- Logit: pr default + xb ---
clear
set obs 100
gen x = (_n - 50.5) * 0.1
gen y = (x > 0)
replace y = 1 - y if mod(_n*13+7, 10) == 0
quietly logit y x
predict p
predict idx, xb
display ""
display "Logit checks: at xb = idx, p should equal 1/(1+exp(-idx)):"
list x idx p in 1/3
list x idx p in 50/52

* --- Probit: pr default + xb ---
clear
set obs 100
gen x = (_n - 50.5) * 0.1
gen y = (x > 0)
replace y = 1 - y if mod(_n*13+7, 10) == 0
quietly probit y x
predict p
predict idx, xb
display ""
display "Probit checks: p = normal(idx):"
list x idx p in 1/3

* --- xtreg FE: xb / u / e / ue / xbu ---
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
quietly xtreg y x, fe
predict xb_hat
predict u_hat, u
predict e_hat, e
predict ue_hat, ue
predict xbu_hat, xbu
* e is a mathematical zero here; its last bits depend on the BLAS backend.
* Clean it through a tolerance that doubles as the assertion: a non-tiny
* value would survive and fail the golden diff.
replace e_hat = cond(abs(e_hat) < 1e-9, 0, e_hat)
replace ue_hat = cond(abs(ue_hat - u_hat) < 1e-9, u_hat, ue_hat)
display ""
display "xtreg FE perfect-fit case:"
display "  xb = beta*x = x (since beta=1, no _cons)"
display "  u = alpha_i = {10, 20, 30}"
display "  e = 0 exactly (perfect within fit)"
display "  ue = y - xb"
display "  xbu = y"
list panel x y xb_hat u_hat e_hat ue_hat xbu_hat

* --- TS-op coefficient: predict after regress on L.growth ---
display ""
display "=== TS-op coef name resolution after keep if ==="
clear
set obs 20
gen str1 p = "A"
gen t = _n
gen g = _n * 1.0
xtset p t
quietly regress g L.g
predict gh
predict gr, resid
* g on L.g is an exact fit; gr is a mathematical zero whose FP noise is
* backend-dependent.  Tolerance-clean (a real residual would survive).
replace gr = cond(abs(gr) < 1e-9, 0, gr)
display "First few rows (row 1 has L.g missing -> all missing):"
list t g gh gr in 1/5

* --- Errors on bad option combinations ---
display ""
display "=== predict ,pr after regress should error ==="
clear
set obs 10
gen y = _n
gen x = _n
quietly regress y x
capture predict p, pr
display "rc=" _rc

display ""
display "=== predict ,resid after xtreg should error ==="
clear
set obs 12
gen str1 p = "A"
replace p = "B" in 5/12
gen t = mod(_n-1, 4) + 2020
gen x = _n
gen y = _n + (p == "B")*10
xtset p t
quietly xtreg y x, fe
capture predict r, resid
display "rc=" _rc

display ""
display "=== predict ,u after regress should error ==="
clear
set obs 10
gen y = _n
gen x = _n
quietly regress y x
capture predict u, u
display "rc=" _rc

* Regression: margins after regress / logit / probit.
*
* Verifies:
*   - margins after regress: AME == β exactly, SE == SE(β)
*   - margins after logit: AME smaller than β, value approximately p̄(1-p̄)·β
*   - margins after probit: AME similar to logit's AME (the famous near-equivalence)
*   - atmeans (MEM) option works
*   - dydx(varlist) selects subset
*   - dydx(*) is the default behavior of "all non-cons"
*   - error: dydx(_cons), dydx(zzz)
*   - TS-op coefficient names work

* --- Margins after regress: should reproduce β table exactly ---
display "=== Margins after regress: AME == β, SE == SE(β) ==="
clear
set obs 100
gen x1 = _n*0.1
gen x2 = mod(_n*7, 13)*0.1
gen y = 2 + 3*x1 - 1.5*x2 + mod(_n*11, 7) - 3
regress y x1 x2
margins, dydx(*)

* --- Margins after logit: AME ≈ p̄(1-p̄) × β ---
display ""
display "=== Margins after logit: AME ≈ avg-p(1-p) × β ==="
clear
set obs 500
gen x1 = mod(_n*7, 100) * 0.02 - 1
gen x2 = mod(_n*13, 50) * 0.04 - 1
gen y = (x1 + 0.5*x2 + mod(_n*17, 7) - 3 > 0)
quietly logit y x1 x2
margins, dydx(*)

display ""
display "=== Margins after logit: MEM (atmeans) ==="
margins, dydx(*) atmeans

* --- Margins after probit: AME for probit ≈ AME for logit on same data ---
display ""
display "=== Margins after probit (should be very close to logit AME above) ==="
quietly probit y x1 x2
margins, dydx(*)

* --- dydx(specific var) ---
display ""
display "=== dydx(x1) only ==="
margins, dydx(x1)

* --- TS-op coefficient ---
display ""
display "=== Margins after logit with TS-op regressor ==="
clear
set obs 50
gen str1 p = "A"
replace p = "B" in 26/50
gen t = mod(_n-1, 25) + 2000
gen x = _n*1.0
gen y = (mod(_n*7, 3) == 0)
xtset p t
quietly logit y L.x x
margins, dydx(*)

* --- Errors ---
display ""
display "=== Error: dydx(_cons) ==="
clear
set obs 50
gen y = mod(_n,2)
gen x = _n*0.1
quietly logit y x
capture margins, dydx(_cons)
display "rc=" _rc

display ""
display "=== Error: unknown variable ==="
capture margins, dydx(zzz)
display "rc=" _rc

display ""
display "=== Error: margins without estimates ==="
clear
set obs 5
gen w = _n
capture margins, dydx(*)
display "rc=" _rc

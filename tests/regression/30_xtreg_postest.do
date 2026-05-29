* Regression: test and lincom work on xtreg estimates, and both accept
* TS-op coefficient names like "L.growth".  Also covers the extended
* test syntax: `= 0`, `= number`, `= other_coef`, and joint forms.

clear
set obs 18
gen str1 p = "A"
replace p = "B" in 7/12
replace p = "C" in 13/18
gen t = mod(_n-1, 6) + 2020
* Design: y = panel-FE + 0.5*L.x + 0.3*L2.x + noise (zero noise for test)
gen x = _n
xtset p t
gen y = .
* Compute y = alpha + 0.5*x[t-1] + 0.3*x[t-2] manually-ish; just use
* something that gives non-degenerate coefficients.
replace y = 10 + 0.5*x + 0.3*x^0.5  in 1/6
replace y = 20 + 0.5*x + 0.3*x^0.5  in 7/12
replace y = 30 + 0.5*x + 0.3*x^0.5  in 13/18

display "=== xtreg with TS ops ==="
xtreg y L.x L2.x, fe
display ""

display "=== test L.x (= 0 default) ==="
test L.x
display ""

display "=== test L.x = 0 (explicit) — should give same F ==="
test L.x = 0
display ""

display "=== test L.x = 0.5 (numeric RHS) ==="
test L.x = 0.5
display ""

display "=== test L.x = L2.x (equality of coefficients) ==="
test L.x = L2.x
display ""

display "=== test L.x L2.x (joint zero) ==="
test L.x L2.x
display ""

display "=== lincom with TS-op names ==="
lincom L.x + L2.x
display ""
lincom 0.5*L.x - 0.5*L2.x

* Regression: Tier 3 output and reproducibility.
*
* Verifies:
*   - estimates store / restore / dir / drop / table
*   - estout in LaTeX, markdown, plain
*   - log captures display output (after the tee fix)
*   - .dta round-trip of variable labels, value labels, formats

* --- estimates store + table ---
clear
set obs 100
gen x1 = _n*1.0
gen x2 = mod(_n, 5)*1.0
gen y = 2*x1 + 3*x2 + mod(_n*7, 11) - 5

quietly regress y x1
estimates store m1
quietly regress y x1 x2
estimates store m2

display ""
display "=== estimates dir ==="
estimates dir

display ""
display "=== estimates table m1 m2 ==="
estimates table m1 m2

display ""
display "=== estimates table m1 m2, se star stats(N r2 rmse) ==="
estimates table m1 m2, se star stats(N r2 rmse)

* --- estout: LaTeX format (default) ---
display ""
display "=== estout m1 m2 (LaTeX, default) ==="
estout m1 m2, stats(N r2)

* --- estout: markdown ---
display ""
display "=== estout m1 m2, format(markdown) ==="
estout m1 m2, format(markdown) stats(N r2)

* --- estout: plain ---
display ""
display "=== estout m1 m2, format(plain) ==="
estout m1 m2, format(plain) stats(N r2)

* --- estimates restore ---
display ""
display "=== estimates restore m1 ==="
estimates restore m1
display "after restore, _b[x1] = " _b[x1]
display "after restore, e(N)  = " e(N)

* --- estimates drop ---
display ""
display "=== estimates drop m1 ==="
estimates drop m1
estimates dir

* --- .dta round-trip with labels ---
display ""
display "=== .dta value-label round-trip ==="
clear
set obs 5
gen cat = mod(_n-1, 3) + 1
label define mycat 1 "First" 2 "Second" 3 "Third"
label values cat mycat
label variable cat "Categorical"
save /tmp/_test_labels.dta, replace
clear
use /tmp/_test_labels.dta
list cat
describe

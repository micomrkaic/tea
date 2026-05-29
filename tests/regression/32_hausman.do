* Regression: Hausman specification test (FE vs RE).
*
* Workflow: run xtreg ,fe then xtreg ,re (in either order), then
* `hausman` — tea remembers both estimates in dedicated workspace slots.
*
* Verifies:
*   - Hausman rejects when α_i and x are correlated (a constructed case)
*   - Hausman does NOT reject when α_i is independent of x
*   - errors cleanly when one or both estimates are missing

* --- Case 1: RE assumption holds (α_i independent of x) ---
clear
set obs 100
gen pid = ceil(_n / 10)
gen tid = mod(_n - 1, 10) + 1
* x depends only on tid (within-panel variation), not on pid
gen x = (tid - 5) * 1.0
* alpha varies by panel and is independent of x by construction
gen alpha = pid * 10
gen eps = mod(_n*7, 13) - 6
gen y = 2*x + alpha + eps
xtset pid tid

quietly xtreg y x, fe
quietly xtreg y x, re

display "=== Hausman when RE assumption holds: expect chi2 ~ 0, p ~ 1 ==="
hausman

* --- Case 2: RE assumption violated (α_i correlated with x) ---
display ""
display "=== Hausman when α_i correlated with x: expect rejection ==="
clear
set obs 100
gen pid = ceil(_n / 10)
gen tid = mod(_n - 1, 10) + 1
* x depends on pid -> correlated with alpha
gen x = pid * (tid - 5) * 0.1
gen alpha = pid * 10
gen eps = mod(_n*7, 13) - 6
gen y = 2*x + alpha + eps
xtset pid tid

quietly xtreg y x, fe
quietly xtreg y x, re
hausman

* --- Case 3: error handling ---
display ""
display "=== hausman errors with no estimates ==="
clear
set obs 4
gen z = _n
capture hausman
display "rc=" _rc

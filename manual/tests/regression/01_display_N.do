* Regression: _N and _n must reflect the active frame in display/macro contexts.
* Bug: previously _N hardcoded to 1 in eval_scalar, so display _N always showed 1.
clear
set obs 100
gen x = _n
display "N=" _N
display "x[1]=" x[1]
display "x[50]=" x[50]
display "x[100]=" x[100]
local n = _N
display "local n=" `n'

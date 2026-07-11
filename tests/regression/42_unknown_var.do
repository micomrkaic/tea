* 42_unknown_var — a typo'd variable name in an expression must error
* loudly (Stata r(111)) and leave the data untouched.  It must never
* silently evaluate to missing: `drop if typo` used to destroy all data.
set obs 10
gen x = _n

* each of these must error without touching data (capture: no abort)
capture keep if nosuchvar == 0
display "after keep-typo: " _N
capture drop if nosuchvar == 0
display "after drop-typo: " _N
capture summarize x if ghost > 5
capture count if ghost == 1
capture list x if ghost == 1
capture gen y = ghost + 1
capture replace x = ghost

* data fully intact
display "final _N: " _N
summarize x

* and the error path aborts a do-file when not captured
keep if nosuchvar == 0
display "must not print"

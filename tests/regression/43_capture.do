* 43_capture — Stata-faithful capture: total silence, _rc carries the code.
set obs 5
gen x = _n
capture sysuse nosuchdata
display "rc after bad sysuse: " _rc
capture keep if typovar == 1
display "rc after typo keep: " _rc
display "data intact: " _N
capture display "this must not print"
capture noisily display "capture-of-success leaves rc"
display "rc after ok display: " _rc
* uncaptured failure still aborts with its message
sysuse nosuchdata
display "unreachable"

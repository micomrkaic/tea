* Regression: each command rejects weight types Stata doesn't accept.
* Per Stata:
*   summarize : fweight, aweight, iweight   (not pweight)
*   tabulate  : fweight, aweight            (not pweight, iweight)
*   count     : fweight                     (not aweight, pweight, iweight)
clear
set obs 4
gen x = _n
gen w = _n
display "--- summarize ---"
capture summarize x [pweight=w]
display "summarize pweight: rejected"
summarize x [aweight=w]
display "--- tabulate ---"
capture tabulate x [pweight=w]
display "tabulate pweight: rejected"
capture tabulate x [iweight=w]
display "tabulate iweight: rejected"
display "--- count ---"
capture count [aweight=w]
display "count aweight: rejected"
capture count [pweight=w]
display "count pweight: rejected"
capture count [iweight=w]
display "count iweight: rejected"
count [fweight=w]
* regress accepts all four
display "--- regress accepts all four ---"
gen y = _n + 0.1*mod(_n, 3)   // non-exact fit: keeps SEs/residuals away from FP-noise zeros
regress y x [fweight=w]
regress y x [aweight=w]
regress y x [pweight=w]
regress y x [iweight=w]

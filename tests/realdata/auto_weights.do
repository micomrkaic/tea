* auto_weights.do — exercises weights on Stata's auto.dta.
*
* Runs in BOTH tea and Stata.  Print blocks are labelled (=== HEADER ===)
* so you can diff the two outputs and immediately see where (if anywhere)
* they diverge.
*
* Usage:
*   tea   auto_weights.do > out_tea.txt   2>&1
*   stata -b do auto_weights.do          # produces auto_weights.log
*
* Then:
*   bash compare.sh out_tea.txt auto_weights.log

clear all
use auto.dta

* Drop rep78 missing so the weight column is clean.  Keeps 69 of 74 obs.
drop if missing(rep78)

display "=========================================================="
display "1. UNWEIGHTED BASELINE"
display "=========================================================="
display "--- summarize price mpg weight ---"
summarize price mpg weight

display "--- regress price mpg weight foreign ---"
regress price mpg weight foreign


display "=========================================================="
display "2. FWEIGHT (rep78 as integer frequency weight)"
display "=========================================================="
display "--- regress price mpg weight foreign [fweight=rep78] ---"
regress price mpg weight foreign [fweight=rep78]
display "Expected: 'Number of obs' = sum(rep78) across the 69 rows"

display "--- summarize price [fweight=rep78] ---"
summarize price [fweight=rep78]


display "=========================================================="
display "3. AWEIGHT (rep78 as analytic weight)"
display "=========================================================="
display "--- regress price mpg weight foreign [aweight=rep78] ---"
regress price mpg weight foreign [aweight=rep78]
display "Expected: same betas as fweight; SE differ; Number of obs = 69"


display "=========================================================="
display "4. PWEIGHT (rep78 as sampling weight)"
display "=========================================================="
display "--- regress price mpg weight foreign [pweight=rep78] ---"
regress price mpg weight foreign [pweight=rep78]
display "Expected: same betas as a/fweight; robust (sandwich) SEs"


display "=========================================================="
display "5. STATA IDENTITY: aweight+robust == pweight"
display "=========================================================="
display "--- regress price mpg weight foreign [aweight=rep78], robust ---"
regress price mpg weight foreign [aweight=rep78], robust
display "Should be byte-identical to section 4 above (pweight)."


display "=========================================================="
display "6. IWEIGHT"
display "=========================================================="
display "--- regress price mpg weight foreign [iweight=rep78] ---"
regress price mpg weight foreign [iweight=rep78]
display "Expected: classical-style SE with sum(weights) df."


display "=========================================================="
display "7. CLUSTER + WEIGHT"
display "=========================================================="
display "--- regress price mpg weight [pweight=rep78], cluster(foreign) ---"
regress price mpg weight [pweight=rep78], cluster(foreign)


display "=========================================================="
display "8. VALIDATION: commands rejecting wrong weight types"
display "=========================================================="
capture summarize price [pweight=rep78]
display "summarize [pweight] -> tea will print 'pweight not allowed'"
display "Stata error: 'pweight not allowed r(101)'"
capture count [aweight=rep78]
display "count [aweight] -> tea will print 'aweight not allowed'"
display "Stata: 'aweight not allowed r(101)'"
capture tabulate foreign [iweight=rep78]
display "tabulate [iweight] -> tea will print 'iweight not allowed'"
display "Stata: 'iweight not allowed r(101)'"


display "=========================================================="
display "9. UNWEIGHTED tabulate + summarize (sanity)"
display "=========================================================="
tabulate foreign
summarize price, detail

display "=========================================================="
display "END OF TEST"
display "=========================================================="

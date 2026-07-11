* Regression: weighted regress, confirming two Stata identities:
*   1. [aweight=w] with ,robust produces identical SE to [pweight=w]
*   2. [fweight=w] is identical to expanding rows to match weights
clear
set obs 5
gen x = _n
gen y = _n
replace y = 10 in 1
gen w = 1
replace w = 100 in 5
display "--- [aweight=w], robust ---"
regress y x [aweight=w], robust
display "--- [pweight=w] (should be byte-identical to above) ---"
regress y x [pweight=w]
display "--- Number of obs comparison ---"
display "fweight should show sum of weights as obs:"
regress y x [fweight=w]
display "aweight, pweight, iweight should show unweighted N=5:"
regress y x [aweight=w]
* fweight equivalence: row replication
display "--- fweight row-replication identity ---"
clear
set obs 5
gen x = _n
gen y = _n + 0.5
gen w = 1
replace w = 2 in 1
replace w = 3 in 2
replace w = 2 in 4
replace w = 3 in 5
display "fweight on 5 rows:"
regress y x [fweight=w]
* expand manually
clear
set obs 11
gen x = .
gen y = .
* 2x obs 1 (x=1,y=1.5)
replace x = 1 in 1
replace y = 1.5 in 1
replace x = 1 in 2
replace y = 1.5 in 2
* 3x obs 2 (x=2,y=2.5)
replace x = 2 in 3
replace y = 2.5 in 3
replace x = 2 in 4
replace y = 2.5 in 4
replace x = 2 in 5
replace y = 2.5 in 5
* 1x obs 3 (x=3,y=3.5)
replace x = 3 in 6
replace y = 3.5 in 6
* 2x obs 4 (x=4,y=4.5)
replace x = 4 in 7
replace y = 4.5 in 7
replace x = 4 in 8
replace y = 4.5 in 8
* 3x obs 5 (x=5,y=5.5)
replace x = 5 in 9
replace y = 5.5 in 9
replace x = 5 in 10
replace y = 5.5 in 10
replace x = 5 in 11
replace y = 5.5 in 11
display "manually expanded to 11 rows:"
regress y x

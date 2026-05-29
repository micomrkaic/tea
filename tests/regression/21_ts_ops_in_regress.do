* Regression: time-series operators (L./F./D./S.) work in regress varlists,
* on the depvar and the regressors, with correct lag numbering and gap-aware
* per-panel lookups via xtset.
clear
set obs 24
* 3 countries, 8 years each
gen str3 cid = "AAA"
replace cid = "BBB" in 9/16
replace cid = "CCC" in 17/24
gen year = mod(_n-1, 8) + 2010
gen y = 1 + 0.3*_n + sin(_n*0.5)
xtset cid year
display "--- L.y (lag-1) ---"
regress y L.y if cid == "AAA"
display ""
display "--- L2.y (lag-2): coefficient label must be L2.y, NOT L12.y ---"
regress y L.y L2.y if cid == "AAA"
display ""
display "--- F.y (lead) ---"
regress y F.y if cid == "AAA"
display ""
display "--- D.y on its own RHS ---"
regress y D.y if cid == "AAA"
display ""
display "--- TS-op on depvar: regress D.y on lagged level ---"
regress D.y L.y if cid == "AAA"
display ""
display "--- mixed: TS-op + plain variable ---"
gen yr = year - 2014
regress y L.y yr if cid == "AAA"
display ""
display "--- case insensitive: lowercase l. should also work ---"
regress y l.y if cid == "AAA"

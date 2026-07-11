* Regression: tabstat columns(stats) vs columns(vars) must produce
* visibly different layouts in the by-mode.
clear
set obs 9
gen str8 region = "Asia"
replace region = "Europe" in 4/6
replace region = "Africa" in 7/9
gen gdp = _n * 100
display "--- columns(statistics) ---"
tabstat gdp, by(region) columns(stats) stats(mean sd n)
display "--- columns(variables) ---"
tabstat gdp, by(region) columns(vars) stats(mean sd n)

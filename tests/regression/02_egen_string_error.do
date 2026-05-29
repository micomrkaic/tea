* Regression: egen with a numeric aggregator on a string variable must error.
* Bug: previously silently produced all-missing output.
* Note: capture lets the do-file continue past the expected error.
clear
set obs 4
gen str10 country = "USA"
replace country = "FRA" in 2
replace country = "USA" in 3
replace country = "FRA" in 4
gen gdp = _n*1000
capture egen avg_country = mean(country)
* valid use of group() on a string is fine
egen cid = group(country)
list country gdp cid

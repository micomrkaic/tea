* Regression: commas inside quoted string literals must not be parsed as
* the options separator.
* Bug: `replace amount = "2,500.00" in 2` was splitting on the comma,
* leaving amount = "2" with options = '500.00" in 2'.
clear
set obs 3
gen amount = "1,234.56"
replace amount = "2,500.00" in 2
replace amount = "987.65" in 3
list

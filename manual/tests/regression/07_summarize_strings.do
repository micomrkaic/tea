* Regression: summarize on an explicit string varlist must error;
* under wildcard expansion it should silently skip strings.
clear
set obs 4
gen str10 name = "alice"
replace name = "bob" in 2
replace name = "carol" in 3
replace name = "dave" in 4
gen score = _n * 10
gen weight = _n
* wildcard — silently filters strings, summarizes the two numerics
display "--- wildcard ---"
summarize *
* explicit list with one string — must error and stop
display "--- explicit (will error) ---"
capture summarize name score

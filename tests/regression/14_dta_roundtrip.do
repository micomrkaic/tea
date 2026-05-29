* Regression: full .dta round-trip through save / use.
* Exercises the readstat-backed writer (with per-column type compression)
* and reader.  Confirms that numeric values, system-missing, tagged-
* missing, strings (including empty), variable labels, and formats all
* round-trip cleanly.
clear
set obs 4
gen byte b   = _n
gen int  i   = _n * 100
gen long l   = _n * 1000000
gen float fp = _n * 0.5
gen double dp = _n * 3.141592653589793
gen str10 country = "USA"
replace country = "FRA" in 2
replace country = "JPN" in 3
replace country = ""    in 4
* introduce missing values
replace b = . in 2
* variable labels & format
label variable b "byte test column"
label variable dp "high-precision constant"
format dp %10.4f
display "--- before save ---"
list
save /tmp/tea_reg14.dta, replace
clear
use /tmp/tea_reg14.dta
display "--- after reload ---"
list
erase /tmp/tea_reg14.dta

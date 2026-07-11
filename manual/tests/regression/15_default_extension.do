* Regression: default extension is .dta (matching Stata).
* use foo (no extension) should look for foo.dta.
* save foo (no extension) should write foo.dta.
* The .tea format is still available with explicit extension.
clear
set obs 3
gen x = _n
gen str3 tag = "AAA"
replace tag = "BBB" in 2
replace tag = "CCC" in 3
* save without extension -> writes .dta
save /tmp/tea_reg15, replace
clear
* use without extension -> reads .dta
use /tmp/tea_reg15
list
* explicit .tea extension still works
save /tmp/tea_reg15_native.tea, replace
clear
use /tmp/tea_reg15_native.tea
list
* cleanup
erase /tmp/tea_reg15.dta
erase /tmp/tea_reg15_native.tea

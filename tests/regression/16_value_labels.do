* Regression: value labels round-trip through .dta save/use.
* Tests that `label define` + `label values` -> save -> clear -> use
* preserves the value-to-text mappings.
clear
set obs 4
gen sex = mod(_n, 2)
gen region = _n
label define sexlbl 0 "Female" 1 "Male"
label define regionlbl 1 "North" 2 "South" 3 "East" 4 "West"
label values sex sexlbl
label values region regionlbl
display "--- before save ---"
list
label list
save /tmp/tea_reg16.dta, replace
clear
display "--- after clear, label list is empty ---"
label list
use /tmp/tea_reg16.dta
display "--- after reload ---"
list
label list
erase /tmp/tea_reg16.dta

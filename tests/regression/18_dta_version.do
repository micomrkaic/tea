* Regression: save's version() option picks the right DTA format version.
clear
set obs 2
gen x = _n
save /tmp/tea_reg18a.dta, replace
save /tmp/tea_reg18b.dta, replace version(117)
save /tmp/tea_reg18c.dta, replace version(119)
* invalid version errors out
capture save /tmp/tea_reg18bad.dta, replace version(99)
display "after bad version attempt (file not created)"
* sanity: all three good files re-read identically
clear
use /tmp/tea_reg18a.dta
list
clear
use /tmp/tea_reg18b.dta
list
clear
use /tmp/tea_reg18c.dta
list
erase /tmp/tea_reg18a.dta
erase /tmp/tea_reg18b.dta
erase /tmp/tea_reg18c.dta

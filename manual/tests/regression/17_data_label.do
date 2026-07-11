* Regression: `label data` dataset label round-trips through .dta.
clear
set obs 2
gen x = _n
label data "tea regression dataset (2 rows)"
display "--- before save ---"
describe
save /tmp/tea_reg17.dta, replace
clear
display "--- after clear, no data label ---"
describe
use /tmp/tea_reg17.dta
display "--- after reload, data label restored ---"
describe
erase /tmp/tea_reg17.dta

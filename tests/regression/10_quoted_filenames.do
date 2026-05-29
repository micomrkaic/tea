* Regression: filenames in single or double quotes must work for
* save/use/import/export.
clear
set obs 3
gen x = _n
gen y = _n * 10
save "/tmp/tea_regtest_quoted.tea", replace
clear
use "/tmp/tea_regtest_quoted.tea"
list
* also single-quoted form
clear
use '/tmp/tea_regtest_quoted.tea'
list
erase "/tmp/tea_regtest_quoted.tea"

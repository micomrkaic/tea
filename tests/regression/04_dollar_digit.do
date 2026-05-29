* Regression: $digit (e.g. $1, $2) must NOT be treated as a macro reference.
* Bug: "$1,234.56" was being parsed as global '$1' followed by ',234.56',
* turning amount values into ',234.56' etc.
* But $name (with letter/underscore first) must still expand.
clear
set obs 3
gen amount = "$1,234.56"
replace amount = "$2,500.00" in 2
replace amount = "$987.65" in 3
list
* and verify normal globals still work
global my_var "hello"
display "value: $my_var"
display "with-digit-in-name: $my_var = $my_var"
* a mix: $name and $digit in the same string
display "$my_var $42 $my_var"

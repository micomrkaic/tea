* Regression, WEO test_04 round:
* (1) compound quotes `"..."' — in display, in local values, through
*     macro expansion (macros expand INSIDE them), and in file write
* (2) extended macro function: local x : subinstr local y "a" "b" [, all]
*     (compound-quoted args are how you say a literal double quote)
* (3) forvalues range with inline-expression macros (`=_N') — used to run
*     the body ZERO times silently; unparseable ranges now fail loud
display `"hello compound"'
local x abc
display `"expanded: `x' end"'
local lab He said "hi" twice: "hi"
local esc : subinstr local lab `"""' `""""', all
display `"`esc'"'
local first : subinstr local lab "hi" "bye"
display `"`first'"'
* the label-generation pipeline from the WEO script, end to end
clear
set obs 2
gen str10 nm = cond(_n==1,"alpha","beta")
gen str20 lab = cond(_n==1,`"says "A""',"plain")
gen v1 = _n
gen v2 = _n*10
rename v1 alpha
rename v2 beta
tempfile labdo
file open fh using `labdo', write replace
quietly {
    forvalues i = 1/`=_N' {
        local v = nm[`i']
        local l = lab[`i']
        local l : subinstr local l `"""' `""""', all
        file write fh `"capture label variable `v' "`l'""' _n
    }
}
file close fh
do `labdo'
describe
* unparseable forvalues range: loud rc=198, never a silent zero-run
capture forvalues i = 1/banana {
    display "WRONG: body ran"
}
display _rc

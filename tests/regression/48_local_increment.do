* Regression: `local ++x` / `local --x` must increment/decrement macro x.
* Bug: "++yr" was parsed as the macro NAME, silently defining a macro
* literally called "++yr" while `yr' stayed constant — foreach rename loops
* produced "already exists" errors on the second iteration.
clear
local yr = 1960
local ++yr
local ++yr
display `yr'
local --yr
display `yr'
* counting inside a loop — the original failure shape
set obs 4
gen a = _n
gen b = _n*10
gen c = _n*100
local k = 0
foreach v of varlist a b c {
    local ++k
}
display `k'
* error path: ++ on an undefined macro must fail loud, not silently create one
capture local ++nosuchmacro
display _rc

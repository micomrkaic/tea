* ---- tea demo / semantics test -------------------------------------
import delimited using tests/panel.csv, clear
describe
list

* missing algebra: FR 2020 gdp is . ; arithmetic must propagate
generate lgdp = ln(gdp)
* the Stata footgun: missing counts as +infinity, so > catches it
count if gdp > 5
count if gdp < .
count if missing(gdp)

* panel setup + time-series operators (gap-aware within country)
xtset country year
generate lag_gdp  = L.gdp
generate g_growth = (gdp - L.gdp)/L.gdp * 100
generate d_pop    = D.pop
list country year gdp lag_gdp g_growth d_pop

* by-group _n/_N and running sum() that resets per group
bysort country (year): generate t = _n
bysort country (year): generate T = _N
bysort country (year): generate cumpop = sum(pop)
list country year t T cumpop

* replace with if ; should report real changes
replace pop = pop + 1 if country == "US"
count if country=="US"

* egen across the panel
egen mean_gdp = mean(gdp), by(country)
egen grp      = group(country)
list country year gdp mean_gdp grp

* calendar suite
clear
set obs 3
generate y = 2018 + _n - 1
generate mdate = ym(y,3)
format mdate %tm
generate qdate = yq(y,2)
format qdate %tq
generate ddate = mdy(1,15,y)
format ddate %td
generate wd    = dow(ddate)
list

* macros + loops
local base 100
global mult 2
generate v = `base' * $mult
forvalues k = 1/3 {
    display "k = `k'"
}
foreach c in alpha beta {
    display "tag=`c'"
}

* collapse
clear
import delimited using tests/panel.csv, clear
collapse (mean) gdp (sum) pop, by(country)
list

* scalar / display / r()
clear
import delimited using tests/panel.csv, clear
summarize gdp
display "mean gdp = " r(mean)
display "N = " r(N)

* ---- Milestone 2: merge + reshape -------------------------------------
clear
import delimited using tests/panel.csv, clear
merge m:1 country using tests/regions.csv, keep(master match)
sort country year
list country year gdp region _merge

* wide <-> long round trip (reshape is in-place)
clear
import delimited using tests/panel.csv, clear
keep country year gdp
reshape wide gdp, i(country) j(year)
list
reshape long gdp, i(country) j(year)
sort country year
list

* ---- Milestone 3: value labels, recode, weights, t-literals, frame copy --
clear
set obs 6
gen id  = _n
gen sex = mod(_n,2) + 1
label define sexlbl 1 "Male" 2 "Female"
label values sex sexlbl
gen wt  = _n
list id sex
summarize id [fw=wt]
recode id (1/3 = 1) (4/6 = 2), gen(half)
tabulate half [fw=wt]

* t*() literal date constructors
clear
set obs 1
gen d = td(15jan2020)
format d %td
gen m = tm(2020m3)
format m %tm
gen q = tq(2020q4)
format q %tq
list

* frame copy then in-place reshape on the copy (the safe pattern)
clear
import delimited using tests/panel.csv, clear
frame copy default backup
frame change backup
keep country year gdp
reshape wide gdp, i(country) j(year)
list
frame change default
codebook gdp
frame dir

* ---- Milestone 4: regress + postestimation -----------------------------
clear
set obs 100
gen x1 = mod(_n*13+5, 17) - 8
gen x2 = mod(_n*7+2, 11) - 5
gen u  = mod(_n*11+1, 7) - 3
gen y  = 2 + 0.5*x1 - 1.2*x2 + u

regress y x1 x2
predict yhat
predict resid, residuals
summarize yhat resid
test x1 x2
lincom x1 - x2
regress y x1 x2, robust

* Regression: reshape must carry non-stub variables (Stata semantics) and
* support a string j in both directions.
* Bugs: reshape long silently DROPPED carried variables (IndicatorName
* vanished after `reshape long y, i(CountryCode IndicatorCode) j(year)`);
* reshape wide read j as numeric unconditionally, so `j(code) string` was
* impossible; a fixed 512-level cap silently dropped level 513 onward.
clear
set obs 2
gen str3 id = "A"
replace id = "B" in 2
gen str5 name = "alpha"
replace name = "beta" in 2
gen y1990 = _n
gen y1991 = _n*10
reshape long y, i(id) j(year)
list
* the carried variable must survive the round trip back to wide, and be
* checked for constancy within i()
reshape wide y, i(id) j(year)
list
* ---- string j round trip ----
clear
set obs 4
gen str3 id = cond(_n<=2,"A","B")
gen str6 code = cond(mod(_n,2)==1,"X_ONE","Y_TWO")
gen v = _n
reshape wide v, i(id) j(code) string
list
reshape long v, i(id) j(code) string
list
* ---- error paths (all must fail loud, never silently mangle) ----
clear
set obs 4
gen str3 id = cond(_n<=2,"A","B")
gen year = cond(mod(_n,2)==1,1,2)
gen z = _n
gen w = _n
* w varies within id: carried var not constant -> rc 9
capture reshape wide z, i(id) j(year)
display _rc
* string j without the string option -> rc 109
clear
set obs 2
gen str3 id = cond(_n==1,"A","B")
gen str4 code = "a b"
gen v = _n
capture reshape wide v, i(id) j(code)
display _rc
* j values that make invalid column names -> rc 198 with strtoname hint
capture reshape wide v, i(id) j(code) string
display _rc
* duplicate j within an i() group -> rc 9, not last-write-wins
clear
set obs 2
gen str3 id = "A"
gen year = 1
gen v = _n
capture reshape wide v, i(id) j(year)
display _rc

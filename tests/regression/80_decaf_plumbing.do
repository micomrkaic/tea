* 80_decaf_plumbing — the decaf-tier integration test: the full data
* pipeline (generate frames, save, merge m:1, reshape both ways,
* collapse, dta round-trip) using only data-management commands, so it
* runs IDENTICALLY under full tea and decaf tea.  This is the workflow
* decaf exists for; the tier-aware harness guarantees it is in the
* decaf subset because every command here exists in that build.
clear
set obs 6
gen id = _n
gen grp = mod(_n-1, 3) + 1
gen v1990 = 10*_n
gen v2000 = 100*_n
tempfile side
preserve
clear
set obs 3
gen grp = _n
gen gname = "g" + string(grp)
quietly save `side'
restore
merge m:1 grp using `side'
count if _merge == 3
display "matched rows: " r(N)
drop _merge
reshape long v, i(id) j(year)
display "long obs: " _N
count if year == 1990
display "1990 rows: " r(N)
quietly collapse (mean) v, by(grp gname)
display "collapsed groups: " _N
list grp gname v
clear
sysuse airline
quietly gen lnp = ln(passengers)
tempfile rt
quietly save `rt'
clear
quietly use `rt'
display "dta round-trip vars: " (_N == 144)
summarize lnp
display "round-trip mean ok: " (abs(r(mean) - 5.542176) < 1e-5)

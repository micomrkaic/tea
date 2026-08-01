* stata_check_sspace.do — run in STATA to pin the state-space tier
* (DESIGN_SSPACE.md) against tea test 72.
* Convention notes (COMPATIBILITY.md): tea uses EXACT diffuse
* initialization (Koopman 1997); Stata's ucm uses a kappa
* approximation, so log-likelihoods can differ in the diffuse terms
* while variance estimates should agree to reported precision.
use nile, clear               // export from tea: sysuse nile + save
tsset year
ucm flow, model(llevel)
predict sl, smstate(level)    // Stata syntax for the smoothed level
summarize sl
replace flow = . if year>=1891 & year<=1900
ucm flow, model(llevel)
use airline, clear
gen lp = ln(passengers)
gen t = _n
tsset t
ucm lp, model(lltrend) seasonal(12)
ucm lp, model(rwdrift)
ucm lp, model(rwalk)

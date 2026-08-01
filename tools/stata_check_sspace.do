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

* -- sspace subset (tea v1.6.40, DESIGN_SSPACE.md Addendum A) --
* AR(1) as a state-space model: must reproduce arima exactly.
use ar1, clear                 // export from tea: the seed-123 AR(1) sim
tsset t
constraint 1 [y]u = 1
sspace (u L.u, state) (y u, noerror noconstant), constraints(1) covstate(diagonal)
arima y, arima(1,0,0) noconstant
* signal extraction (y observed with noise; z exported from tea)
use noisy, clear
tsset t
constraint 2 [z]u = 1
sspace (u L.u, state) (z u, noconstant), constraints(2) covstate(diagonal)

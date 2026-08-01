* stata_check_ts.do — run in STATA to pin the time-series inference
* tier (DESIGN_TSINFER.md) against tea tests 70 and 71.
* Known convention differences (COMPATIBILITY.md): tea's unit-root
* p-values are probit-interpolation approximations of MacKinnon;
* tea's vecrank prints Osterwald-Lenum CVs (as Stata does).
use airline, clear            // export from tea: sysuse airline + save
gen lp = ln(passengers)
gen time = _n
tsset time
newey lp time, lag(4)
dfuller lp
dfuller lp, trend lags(4)
dfuller D.lp, lags(4)
dfuller lp, drift lags(1)
pperron lp
pperron lp, trend
tsfilter hp lp_hp = lp, smooth(129600) trend(lp_tr)
summarize lp_hp lp_tr
use weo_usa, clear            // export from tea: the 71 sample
tsset year
var ngdp_rpch pcpipch lur, lags(1/2)
vargranger
irf create v, step(4) set(v, replace)
irf table oirf
lpirf ngdp_rpch, step(4) lags(2)
vecrank ngdp_rpch pcpipch lur, lags(2)

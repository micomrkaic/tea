* 75_ss_extensions — v1.6.41: sspace diffuse, dfactor idio ar(1),
* seasonal arima, ucm cycle.  Tolerance-first throughout.
* Verified: local level via sspace+diffuse == ucm llevel EXACTLY
* (-633.46456, the DK numbers); dfactor error_order matches
* statsmodels to 5 decimals (-969.47356); airline SARIMA(0,1,1)x
* (0,1,1,12) noconstant matches statsmodels/Stata's canonical fit
* (ll 244.6965); the ucm cycle model's loglik equals the large-kappa
* limit of the diffuse likelihood to 5 decimals (-505.59720) at
* kappa = 1e6, 1e8, 1e10 — where statsmodels' exact-diffuse disagrees
* with its own kappa-limit by 2.09 (see COMPATIBILITY.md).
* -- sspace diffuse == ucm llevel (internal exactness #3) --
sysuse nile, clear
quietly tsset year
constraint drop _all
constraint 1 [flow]u = 1
constraint 2 [u]L.u = 1
quietly sspace (u L.u, state) (flow u, noconstant), constraints(1 2) covstate(diagonal) diffuse
display "diffuse ll == ucm ok: "  (abs(e(ll) - (-633.46456)) < .0005)
display "diffuse var(u) ok: "     (abs(_b[var_u] - 1469.18) < .5)
* -- seasonal arima: the canonical airline model --
sysuse airline, clear
quietly gen lp = ln(passengers)
quietly gen t = _n
quietly tsset t
quietly arima lp, arima(0 1 1) sarima(0 1 1 12) noconstant
display "airline sarima ll ok: "  (abs(e(ll) - 244.6965) < .005)
display "airline ma1 ok: "        (abs(_b[ma1] - (-.4018)) < .002)
display "airline sma1 ok: "       (abs(_b[sma1] - (-.5569)) < .002)
* -- ucm cycle: DGP rho=.95 lambda=.5; kappa-limit-verified loglik --
clear
set seed 21
set obs 400
gen t = _n
quietly tsset t
gen ka = rnormal(0,0.4)
gen kb = rnormal(0,0.4)
gen c1 = 0
gen c2 = 0
quietly replace c1 = ka in 1
quietly replace c2 = kb in 1
forvalues i = 2/400 {
    quietly replace c1 = 0.95*(cos(0.5)*c1[`i'-1]+sin(0.5)*c2[`i'-1])+ka[`i'] in `i'
    quietly replace c2 = 0.95*(-sin(0.5)*c1[`i'-1]+cos(0.5)*c2[`i'-1])+kb[`i'] in `i'
}
gen lvl = 0
gen el = rnormal(0,0.15)
quietly replace lvl = el in 1
forvalues i = 2/400 {
    quietly replace lvl = lvl[`i'-1]+el[`i'] in `i'
}
gen y = lvl + c1 + rnormal(0,0.5)
quietly ucm y, model(llevel) cycle
display "cycle ll (kappa-limit) ok: " (abs(e(ll) - (-505.5972)) < .005)
display "cycle damping ok: "          (abs(_b[cycle_rho] - .9478) < .005)
display "cycle frequency ok: "        (abs(_b[cycle_freq] - .5259) < .005)
* -- dfactor idiosyncratic ar(1) vs statsmodels error_order=1 --
clear
set seed 11
set obs 250
gen t = _n
quietly tsset t
gen e0 = rnormal(0,1)
gen f = 0
replace f = e0 in 1
forvalues i = 2/250 {
    replace f = 0.7*f[`i'-1] + e0[`i'] in `i'
}
gen y1 = 1.0*f + rnormal(0,0.6)
gen y2 = 0.8*f + rnormal(0,0.8)
gen y3 = 0.5*f + rnormal(0,0.5)
gen u1 = 0
set seed 77
quietly replace u1 = rnormal(0,0.5) in 1
forvalues i = 2/250 {
    quietly replace u1 = 0.5*u1[`i'-1]+rnormal(0,0.5) in `i'
}
quietly replace y1 = y1 + u1
foreach v in y1 y2 y3 {
    quietly summarize `v'
    quietly replace `v' = `v' - r(mean)
}
quietly dfactor (y1 y2 y3 = , noconstant ar(1)) (f1 = , ar(1))
display "idio-ar ll vs sm ok: " (abs(e(ll) - (-969.47356)) < .005)
constraint drop _all

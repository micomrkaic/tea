* 74_sspace — the sspace subset (DESIGN_SSPACE.md Addendum A).
* Verified three ways: (1) INTERNAL EXACTNESS — the AR(1) written as a
* state-space model reproduces `arima, arima(1 0 0) noconstant` (same
* exact likelihood via a different front door): ll -218.04181, coef
* .673299, OIM se .0431407 identical to arima's; (2) a noisy-AR(1)
* signal-extraction model matches a hand-written statsmodels custom
* MLEModel: ll -310.16419 both; (3) covstate(identity) reaches the
* same ll with scale migrated into the loading (.506174 = sqrt of the
* diagonal-form var(u)).  Tolerance-first per the v1.6.38 doctrine.
clear
set seed 123
set obs 300
gen t = _n
quietly tsset t
gen eps = rnormal(0,0.5)
gen y = 0
replace y = eps in 1
forvalues i = 2/300 {
    replace y = 0.7*y[`i'-1] + eps[`i'] in `i'
}
constraint drop _all
constraint 1 [y]u = 1
quietly sspace (u L.u, state) (y u, noerror noconstant), constraints(1) covstate(diagonal)
display "ar1-equivalence coef ok: " (abs(_b[u_Lu] - .673299) < .0005)
display "ar1-equivalence var ok: "  (abs(_b[var_u] - .250004) < .0005)
display "ar1-equivalence ll ok: "   (abs(e(ll) - (-218.04181)) < .001)
* signal extraction: y observed with noise
set seed 5
gen z = y + rnormal(0,0.4)
constraint 2 [z]u = 1
quietly sspace (u L.u, state) (z u, noconstant), constraints(2) covstate(diagonal) smstates(sm)
display "noisy ll at sm-custom optimum ok: " (abs(e(ll) - (-310.16419)) < .001)
display "noisy var(e) ok: " (abs(_b[var_ez] - .1617) < .002)
quietly summarize sm_u
display "smoothed state sd ok: " (abs(r(sd) - .592) < .03)
* identity normalization: same ll, scale in the loading
quietly sspace (u L.u, state) (z u, noconstant)
display "identity ll invariant ok: " (abs(e(ll) - (-310.16419)) < .001)
display "identity loading = sqrt(var) ok: " (abs(_b[z_u] - .5062) < .005)
constraint drop _all

* 73_dfactor_constraint — dynamic-factor models + the constraints
* subsystem (DESIGN_DFACTOR.md, signed).  Verified against statsmodels
* DynamicFactor: k=1 demeaned/noconstant loglik matches to 5 decimals
* (-930.83738); k=2 with the triangular constraint reproduces the
* UNCONSTRAINED statsmodels optimum's loglik exactly (-1753.32571) —
* the normalization-invariance check.  Per the v1.6.38 lesson, this
* test is tolerance-first: dfactor likelihoods are flat-adjacent and
* optimizer landing points are backend-dependent below the asserted
* tolerances.
* -- constraint command mechanics (deterministic, exact golden) --
constraint 1 [y2]f2 = 0
constraint define 2 [y1]f1 = 1
constraint list
constraint drop 1
constraint list
constraint drop _all
* -- k=1 --
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
quietly dfactor (y1 y2 y3 = ) (f1 = , ar(1))
display "k1 load1 ok: " (abs(_b[y1_f1] - 1.053) < .01)
display "k1 load3 ok: " (abs(_b[y3_f1] - 0.5004) < .01)
display "k1 ar ok: "    (abs(_b[f1_L1f1] - 0.6807) < .01)
display "k1 var2 ok: "  (abs(_b[var_y2] - 0.6436) < .01)
* ragged edge: missing values are legal inside the range
quietly replace y2 = . if t > 245
quietly replace y3 = . if t > 247
quietly dfactor (y1 y2 y3 = ) (f1 = , ar(1)), smfactor(g)
quietly summarize g1
display "ragged smoothed-factor sd ok: " (abs(r(sd) - 1.31) < .05)
* -- k=2, identified by constraint; ll equals the unconstrained
*    statsmodels optimum (rotation invariance) --
clear
set seed 99
set obs 300
gen t = _n
quietly tsset t
gen ea = rnormal(0,1)
gen eb = rnormal(0,1)
gen fa = 0
gen fb = 0
replace fa = ea in 1
replace fb = eb in 1
forvalues i = 2/300 {
    replace fa = 0.6*fa[`i'-1] + ea[`i'] in `i'
    replace fb = -0.3*fb[`i'-1] + eb[`i'] in `i'
}
gen y1 = 1.0*fa + rnormal(0,0.7)
gen y2 = 0.7*fa + 0.4*fb + rnormal(0,0.7)
gen y3 = 0.3*fa + 1.0*fb + rnormal(0,0.7)
gen y4 = -0.5*fa + 0.8*fb + rnormal(0,0.7)
foreach v in y1 y2 y3 y4 {
    quietly summarize `v'
    quietly replace `v' = `v' - r(mean)
}
constraint 1 [y1]f2 = 0
quietly dfactor (y1 y2 y3 y4 = , noconstant) (f1 f2 = , ar(1)), constraints(1)
display "k2 constrained coef pinned: " (abs(_b[y1_f2]) < 1e-8)
display "k2 ll at sm unconstrained optimum: " (abs(e(ll) - (-1753.32571)) < .002)
constraint drop _all

* make_pin_data.do — run in TEA to export every dataset the Stata pin
* scripts (tools/stata_check_*.do) consume.  One run of this file,
* then the four check scripts run in Stata with no further setup.
* DGPs reproduce the regression-test simulations exactly (same seeds,
* same recursions), so the pins land on the same numbers the tea
* suite asserts.  Output: eight .dta files in the working directory.

* --- bundled datasets --------------------------------------------------
sysuse airline, clear
save airline.dta, replace
sysuse nile, clear
save nile.dta, replace
sysuse grunfeld, clear
save grunfeld.dta, replace

* --- weo_usa: the test-71 sample --------------------------------------
sysuse weo, clear
quietly keep if iso == "USA"
quietly keep year ngdp_rpch pcpipch lur
save weo_usa.dta, replace

* --- ar1 + noisy: the test-74 state-space simulations ------------------
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
quietly drop eps
save ar1.dta, replace
set seed 5
gen z = y + rnormal(0,0.4)
save noisy.dta, replace

* --- df1: the test-73 k=1 dynamic-factor simulation --------------------
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
quietly drop e0 f
save df1.dta, replace

* --- df3: the test-73 k=2 simulation, demeaned -------------------------
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
quietly drop ea eb fa fb
save df3.dta, replace

display "pin datasets written: airline nile grunfeld weo_usa ar1 noisy df1 df3"

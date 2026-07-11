* Regression: Tier 4 econometric completion
* (ivregress 2sls, poisson, xtreg be, arima, runiform/rnormal)

* ---- ivregress 2sls ----
clear
set seed 1
set obs 200
gen z = rnormal()
gen u = rnormal()
gen x = 0.8*z + 0.5*u + rnormal(0, 0.5)
gen y = 2.0*x + u + rnormal(0, 0.3)
quietly regress y x
display "OLS (biased): _b[x] = " _b[x]
ivregress 2sls y (x = z)
display "IV recovered: _b[x] = " _b[x]
display "Expected: ~2.0"

* ---- poisson ----
clear
set obs 200
gen y = mod(_n, 5)
quietly poisson y
display ""
display "Poisson intercept-only: _b[_cons] = " _b[_cons]
display "Expected: log(mean(y)) = log(2.0) = 0.6931"

* ---- xtreg, be ----
clear
set obs 12
gen country = ceil(_n/4)
gen year = mod(_n-1, 4) + 2020
gen x = _n*1.0
gen y = 5 + 2*x
xtset country year
xtreg y x, be
display ""
display "BE: _b[x] = " _b[x] "  _b[_cons] = " _b[_cons]
display "Expected: 2 and 5"

* ---- arima AR(1) ----
clear
set seed 123
set obs 300
gen t = _n
tsset t
gen eps = rnormal(0, 0.5)
gen y = 0
replace y = eps in 1
forvalues i = 2/300 {
    replace y = 0.7*y[`i'-1] + eps[`i'] in `i'
}
quietly arima y, arima(1 0 0) noconstant
display ""
* The optimizer's last digits depend on the libm/BLAS backend; assert the
* estimate with a tolerance instead of printing full precision.
display "ARIMA AR(1) within 0.05 of true 0.7: " (abs(_b[ar1] - 0.7) < 0.05)
display "ARIMA AR(1) rounded: " round(_b[ar1]*10000)/10000

* ---- PRNG reproducibility ----
display ""
set seed 42
display "First runiform with seed 42:"
clear
set obs 3
gen u = runiform()
list u
set seed 42
display "Re-seeded:"
clear
set obs 3
gen u = runiform()
list u

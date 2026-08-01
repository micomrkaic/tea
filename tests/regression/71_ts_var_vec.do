* 71_ts_var_vec — time-series inference tier stage 2: var, vargranger,
* irf create/table, lpirf, vecrank on the US series from the bundled
* WEO.  VAR coefficients and Johansen trace statistics verified
* EXACTLY against statsmodels; Sigma uses the ML divisor T (Stata's
* convention; statsmodels uses T-K), and vecrank prints
* Osterwald-Lenum critical values (Stata's tables).
sysuse weo, clear
quietly keep if iso == "USA"
quietly keep year ngdp_rpch pcpipch lur
quietly drop if ngdp_rpch==. | pcpipch==. | lur==.
quietly tsset year
var ngdp_rpch pcpipch lur, lags(1/2)
vargranger
irf create, step(4)
irf table oirf
irf table irf
lpirf ngdp_rpch, step(4) lags(2)
vecrank ngdp_rpch pcpipch lur, lags(2)

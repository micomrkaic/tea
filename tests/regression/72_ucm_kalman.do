* 72_ucm_kalman — state-space tier release one (DESIGN_SSPACE.md):
* the exact-diffuse univariate Kalman engine + ucm.
* Verified against the literature: Nile local level reproduces DK's
* canonical variances (15099, 1469.1) and statsmodels' exact-diffuse
* loglik to 5 decimals; airline lltrend+seasonal(12) matches
* statsmodels' optimum (ll 217.4201).
* Per Decision 6(b) of the design note, this golden prints estimates
* THROUGH round(): the ML optimum is identical across BLAS backends
* at rounded precision, but the numerical-Hessian SEs can differ in
* the 6th digit (observed: 1280.38 vs 1280.37 native/wasm), so raw
* tables are quietly suppressed and rounded scalars are displayed.
sysuse nile, clear
quietly tsset year
quietly ucm flow, model(llevel) smstate(sl)
display round(_b[var_level], .01)
display round(_b[var_e], .1)
display round(_se[var_level], 1)
display round(_se[var_e], 1)
quietly gen r_sl = round(sl, .1)
summarize r_sl
quietly replace flow = . if year>=1891 & year<=1900
quietly ucm flow, model(llevel)
display round(_b[var_level], .1)
display round(_b[var_e], 1)
sysuse airline, clear
quietly gen lp = ln(passengers)
quietly gen t = _n
quietly tsset t
quietly ucm lp, model(lltrend) seasonal(12)
* The airline UC likelihood is famously flat: optimizer landing points
* differ in the 4th significant digit across BLAS backends (observed:
* var(level) .0006994 container vs .0006993 ext4/OpenBLAS — the
* seventh documented substrate instance).  Assert with tolerances
* against the statsmodels optimum instead of printing digits.
display "airline var(level) ok: " (abs(_b[var_level] - .000699) < .00002)
display "airline var(seas) ok: "  (abs(_b[var_seas]  - .0000642) < .000002)
display "airline var(e) ok: "     (abs(_b[var_e]     - .00013)  < .00002)
quietly ucm lp, model(rwdrift)
display round(_b[var_level], 1e-5)
quietly ucm lp, model(rwalk)
display round(_b[var_level], 1e-5)

* 70_ts_unitroots_filters — time-series inference tier stage 1
* (DESIGN_TSINFER.md): newey, dfuller (incl. ts-operators and drift),
* pperron, tsfilter hp/bk/hamilton.  ADF t-stats, MacKinnon CVs, HP
* moments and the Newey-West SE verified EXACTLY against statsmodels;
* p-values are the documented probit-interpolation approximation.
sysuse airline, clear
quietly gen lp = ln(passengers)
quietly gen time = _n
quietly tsset time
newey lp time, lag(4)
dfuller lp
dfuller lp, trend lags(4)
dfuller D.lp, lags(4)
dfuller lp, drift lags(1)
pperron lp
pperron lp, trend
tsfilter hp lp_hp = lp, smooth(129600) trend(lp_tr)
tsfilter bk lp_bk = lp, minperiod(6) maxperiod(32) k(12)
tsfilter hamilton lp_ham = lp
* the hp and hamilton cycles are zero-mean by construction; their raw
* means are solver noise (~1e-12) that differs by BLAS — round before
* summarizing so the golden locks signal, not dust
quietly gen r_hp  = round(lp_hp,  .000001)
quietly gen r_bk  = round(lp_bk,  .000001)
quietly gen r_ham = round(lp_ham, .000001)
summarize r_hp lp_tr r_bk r_ham

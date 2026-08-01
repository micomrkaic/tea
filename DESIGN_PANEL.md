# Design note: the panel depth tier

Status: SIGNED (Mico, 2026-08-01, as part of the five-item agenda).
Three commands: areg, xtivreg (fe), xtabond.

## 1. areg

    areg depvar indepvars [if] [in], absorb(catvar) [robust cluster()]

Within-transform on absorb() groups (any variable, treated
categorically by value), OLS on demeaned data, coefficients identical
to including group dummies; dof: N - K - G (G = number of absorbed
groups), so classical/robust SEs use the dummy-equivalent df.  SEs
through the vce module (its fourth act).  VERIFICATION: internal
exactness against tea's own regress with the dummy set — coefficients
equal to machine precision, SEs equal after the df correction.

## 2. xtivreg, fe

    xtivreg depvar [exog] (endo = instruments), fe [robust cluster()]

Within transform (panel demeaning) of every variable, then 2SLS on
the demeaned system; df accounts for the N_g absorbed means (Stata's
small-sample convention: df = N - K - N_g).  fe only; re is staged.
VERIFICATION: internal exactness against manual demeaning +
ivregress 2sls in tea itself, plus a numpy referee.

## 3. xtabond

    xtabond depvar [indepvars], lags(1) [maxldep(#) robust twostep]

Arellano-Bond difference GMM, release scope: lags(1) (one lag of the
dependent variable), strictly exogenous indepvars instrumented by
themselves in differences, GMM-style (non-collapsed) instruments for
the lagged dependent variable: at time t, levels y_{i,1}..y_{i,t-2},
optionally capped by maxldep().  One-step estimator with the
Arellano-Bond H matrix (tridiagonal 2,-1) as the initial weight;
twostep option uses the one-step residual-based optimal weight.
vce: conventional one-step SEs, or robust = panel-clustered sandwich
(Stata's vce(robust)); Windmeijer correction for two-step is STAGED
(two-step robust SEs print a note).  Sargan statistic reported from
the two-step (or one-step homoskedastic) J.  AR(1)/AR(2) tests
staged.  VERIFICATION: an independent numpy GMM referee implementing
the same formulas on the same simulated panel (no reference
implementation is installed; the referee is hand-rolled and the
formulas are cross-checked against Arellano-Bond 1991), plus rho
recovery on a known DGP.

## Shared

Panel machinery uses xtset (ts_panel/ts_time).  All three post
Estimates with the standard machinery, honor if/in via the house Sel
idiom where applicable, and are exercised by tolerance-first tests
where estimators are iterative or sampling-based, exact goldens where
deterministic (areg/xtivreg are deterministic: exact goldens).

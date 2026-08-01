# Design note: the macro time-series inference tier

Status: approved design; implementation staged from v1.6.35.
Owner: Mico Mrkaic.  Companion: DESIGN_VCE.md (the scores/meat
interface this tier extends).

## Scope and sequencing

Eight command families, implemented in dependency order:

1. newey        — HAC (Bartlett/Newey-West) regression.  FIRST because
                  pperron and lpirf consume the long-run-variance
                  machinery, which lands in src/vce.c as the promised
                  "different M from the same s_i".
2. dfuller      — Augmented Dickey-Fuller.
3. pperron      — Phillips-Perron (reuses the HAC kernel).
4. tsfilter     — hp, bk, hamilton.
5. var          — reduced-form VAR by equation-wise OLS.
6. vargranger   — Granger Wald tests from the last var.
7. irf          — impulse responses from the last var (subset, below).
8. lpirf        — local projections with Newey SEs.
9. vecrank      — Johansen trace test.  vec estimation is STAGED to a
                  later release; vecrank answers the everyday question
                  ("how many cointegrating relations?") at a fraction
                  of the surface.

## Specification decisions

newey:  `newey y xs [if] [in], lag(#)`.  OLS point estimates; V from
the vce module with Bartlett meat  M = Omega_0 + sum_{l=1..L}
w_l (Omega_l + Omega_l') , w_l = 1 - l/(L+1), Omega_l = sum_t s_t
s_{t-l}'.  Small-sample factor N/(N-k) (Stata's).  t stats with N-k
df.  Requires tsset; gaps in time are an error (Stata behavior).

dfuller:  `dfuller y [if] [in] [, lags(#) trend drift noconstant
regress]`.  Test regression Delta y_t on y_{t-1} [, t] [, const]
[, lagged Delta terms].  Reported: Z(t) = tau on y_{t-1}, the 1/5/10%
critical values from the MacKinnon (2010) response surfaces (constant
/ trend / none variants), and the MacKinnon (1994) approximate
p-value via the published regression coefficients.  `regress` prints
the underlying regression table.  Critical values and p-values are
tabulated approximations by construction; the Stata check pins
agreement to displayed precision (4 decimals).

pperron:  `pperron y [, lags(#) trend noconstant regress]`.  DF
regression WITHOUT lag augmentation; Z(t) and Z(rho) corrected with
the Bartlett long-run variance of the residuals, default lag
floor(4 (T/100)^(2/9)) (Stata's).  Same MacKinnon furniture.

tsfilter:  `tsfilter hp NEW = VAR [, smooth(#)]` with lambda default
1600 quarterly, 100 yearly, 129600 monthly (Stata's defaults by tsset
frequency); solved exactly via the pentadiagonal system
(I + lambda D''D) tau = y with a banded LAPACK solve.
`tsfilter bk NEW = VAR [, minperiod(#) maxperiod(#) k(#)]` Baxter-King
symmetric truncated bandpass, defaults 6/32/12 quarterly scaled by
frequency; the first and last k observations are missing.
`tsfilter hamilton NEW = VAR [, h(#) p(#)]` Hamilton (2018)
regression filter, defaults h=8, p=4 at quarterly (scaled by
frequency); the residual is the cycle.  All three create the NEW
cycle variable; hp also honors `trend(NAME)`.

var:  `var y1 y2 ... [if] [in] [, lags(1/p)]` default lags(1/2).
Equation-wise OLS with constant.  Prints Stata's per-equation header
block (Sample, obs, log-lik, AIC/HQIC/SBIC, per-equation R2/RMSE)
and coefficient tables.  Estimates stored for vargranger/irf: A_1..
A_p, Sigma (ML, divisor T), variable names, T.

vargranger:  Wald chi2 per (equation, excluded variable) pair on that
variable's lags, plus the ALL row.  Stata's table layout.

irf (subset):  Stata's irf is a file-based subsystem (irf create /
irf set / active files).  tea keeps ONE active IRF set in memory,
from the last var:
    irf create [, step(#)]      default step(8); computes irf and oirf
    irf table [irf|oirf]        the response table, all pairs
    irf graph [irf|oirf]        one combined SVG via the graph engine
No .irf files, no multiple sets, no fevd this release; the point is
the daily loop (var -> irf table/graph) not the archival system.
Orthogonalized responses use the Cholesky factor of Sigma in the
variable order given to var (Stata's convention).

lpirf:  `lpirf y [if] [in] [, step(#) lags(#)]` Jorda local
projections of y_{t+h} on the shock (y_t) and lags(#) controls of y,
h = 1..step (default 8), each horizon's SE Newey-West with lag h
(Stata's default).  Output: the coefficient/SE table per horizon.
Multivariate lpirf (responses of other variables) is staged with vec.

vecrank:  `vecrank y1 y2 ... [, lags(#) trend(constant|none)]`
Johansen trace statistics from the canonical correlations of the
VECM's concentrated system; 5% critical values from Osterwald-Lenum
(1992) for the unrestricted-constant and no-constant cases.  Prints
the rank table with the selected rank starred, Stata-style.

## Numerical ground rules

Everything is LAPACK/GSL, deterministic, and inside the 6-significant-
digit display bound.  Tabulated constants (MacKinnon response
surfaces and p-value regressions, Osterwald-Lenum critical values)
are embedded as static tables with their citations; they are data,
not code, and COMPATIBILITY.md documents that p-values are
approximations that may differ from Stata in the last displayed
digit.  tools/stata_check_ts.do mirrors every regression test so the
IMF Stata run pins the whole tier.

## Staged out (explicitly)

vec estimation, fevd, irf files/sets, multivariate lpirf, svar,
xtunitroot, dfgls.  Each becomes cheap once this tier's machinery
exists; none blocks the daily loop this tier serves.

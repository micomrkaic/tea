/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * stats.h — distributional helpers used by estimation/postestimation,
 * routed through GSL so the rest of tea doesn't include GSL headers.
 */
#ifndef TEA_STATS_H
#define TEA_STATS_H

/* two-sided p-value for a t-statistic */
double tea_pval_t(double t, double df);

/* upper-tail p-value for an F statistic */
double tea_pval_f(double f, double df1, double df2);

/* upper-tail p-value for a chi-squared statistic */
double tea_pval_chi2(double x, double df);

/* standard normal CDF Φ(z) — for probit */
double tea_normal_cdf(double z);
/* standard normal PDF φ(z) = (2π)^(-1/2) exp(-z²/2) — for probit score */
double tea_normal_pdf(double z);
/* log Φ(z) computed stably for large negative z (avoid log(0)) */
double tea_log_normal_cdf(double z);

/* inverse t and inverse normal — for CIs */
double tea_invt(double p, double df);   /* P(T <= t) = p */
double tea_ttail(double df, double t);    /* Stata-named: upper-tail */
double tea_invttail(double df, double p); /* Stata-named: inverse upper-tail */
double tea_invnormal(double p);           /* standard-normal inverse CDF */

/* ---- backend-independent zero handling --------------------------------
 * A residual sum of squares that is mathematically zero (perfect fit)
 * comes out as O(1e-16..1e-29) floating-point noise whose exact value
 * depends on the BLAS backend, its version, and even the CPU's kernel
 * dispatch.  Snap it to exactly 0 so that sigma², SEs, t, p, RMSE, and
 * the F line print identically on every machine.  Threshold is relative
 * (1e-12 of TSS) with a tiny absolute floor for the TSS==0 edge case. */
static inline double tea_snap_rss(double rss, double tss){
    if (rss < 0) return 0;                    /* roundoff can go negative */
    if (rss < 1e-12 * tss + 1e-30) return 0;
    return rss;
}


#endif

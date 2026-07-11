/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Postestimation interface: every estimator (regress, xtreg, ...) writes
 * its results into a workspace-level Estimates struct, which test/predict/
 * lincom then consume uniformly.
 */
#ifndef TEA_ESTIMATES_H
#define TEA_ESTIMATES_H

#include <stddef.h>
#include <stdbool.h>

typedef enum { SE_CLASSICAL=0, SE_ROBUST=1, SE_CLUSTER=2 } SeKind;

typedef struct Estimates {
    char     cmd[16];                /* "regress", "xtreg", ... */
    char     depvar[33];
    int      K;                      /* coefficients incl _cons if present */
    char   (*xnames)[33];            /* K x 33 */
    int     *omitted;                /* K flags: 1 if dropped for collinearity */
    double  *b;                      /* K */
    double  *V;                      /* K*K row-major */
    long     N;
    int      df_r;                   /* residual df */
    int      df_m;                   /* model df = K - omitted - (has_cons?1:0) */
    int      has_cons;
    double   r2, r2_a, rmse, F, F_p;
    double   tss, rss, mss;
    double   sigma2;                 /* RSS / df_r — classical scale */
    SeKind   se_kind;
    char     cluster_var[33];
    long     n_clusters;
    /* row-aligned bookkeeping for predict (size = frame nobs at estimation) */
    size_t   nobs_at_fit;
    char    *used;                   /* nobs_at_fit bytes: 1 if row was in sample */
    char     fitted_frame[33];       /* name of frame at estimation time */
    /* ---- xtreg-specific fields (zero for non-panel commands) ---- */
    long     n_groups;               /* number of panels */
    long     T_min, T_max;           /* obs per panel: min/max */
    double   T_avg;                  /* obs per panel: average */
    double   r2_w, r2_b, r2_o;       /* within, between, overall R² */
    double   sigma_u, sigma_e, rho;  /* variance components for FE */
    double   F_u, F_u_p;             /* test of u_i = 0 */
    double   corr_u_Xb;              /* corr(u_i, Xβ) — Stata reports this */
} Estimates;

Estimates *est_new(void);
Estimates *est_clone(const Estimates *src);
void       est_free(Estimates *e);
int        est_idx_of(const Estimates *e, const char *name);  /* coefficient index by name, -1 if none */

#endif

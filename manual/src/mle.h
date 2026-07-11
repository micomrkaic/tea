/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mle.h — generic Newton-Raphson MLE driver for GLM-type estimators.
 *
 * Currently used by probit and logit; designed to extend to poisson and
 * cloglog with no changes to the driver — just new MleFamily structs.
 *
 * The iteration: β_{t+1} = β_t + (X' W X)^{-1} X' s,
 * where W and s are family-specific functions of the linear predictor
 * η = Xβ.  For canonical-link GLMs (logit, poisson):
 *   s_i = y_i - μ(η_i)         (raw residual at link scale)
 *   W_i = dμ/dη  evaluated at η_i  (= μ(1-μ) for logit; μ for poisson)
 * For non-canonical (probit), the formulae are different but the driver
 * doesn't care — it just gets s and W from the family.
 */
#ifndef TEA_MLE_H
#define TEA_MLE_H

#include <stddef.h>
#include <stdbool.h>

/* Family-specific callbacks.  All work row-by-row given the linear
 * predictor η.  N is the number of observations. */
typedef struct {
    const char *name;            /* "logit", "probit", "poisson", ... */
    const char *outcome_kind;    /* "binary", "count", etc. — used for y-validation */

    /* For each i, given η_i and y_i, compute:
     *   *score_i  = derivative of log-lik wrt η_i  (sets dL/dβ_j = X[j,i] * score_i)
     *   *weight_i = -d²(log-lik)/dη_i²            (always >= 0 for valid families)
     *   *loglik_i = the log-lik contribution for row i */
    void (*per_obs)(double y, double eta,
                    double *score, double *weight, double *loglik);

    /* Optional family-specific y validation.  Returns NULL if OK, error
     * string otherwise.  Called once on the full y vector before fitting. */
    const char *(*validate_y)(const double *y, long N);

    /* Starting-value strategy.  beta_out must be filled with K values.
     * Default if NULL: use OLS (X'X)^{-1} X' y. */
    void (*start_values)(const double *X, const double *y, long N, int K,
                         double *beta_out);
} MleFamily;

/* Result of an MLE fit. */
typedef struct {
    double *beta;           /* K — fitted coefficients */
    double *V_classical;    /* K*K row-major — (X'WX)^{-1} */
    double *eta;            /* N — final linear predictor (caller's data) */
    int    *omitted;        /* K — 1 if column was dropped (Stata-style) */
    double  loglik;         /* final log-likelihood */
    double  loglik_0;       /* null model (intercept-only) log-likelihood */
    int     iterations;     /* number of NR iterations taken */
    bool    converged;
    bool    perfect_pred;   /* set if separation/perfect-prediction detected */
} MleFit;

/* Fit by Newton-Raphson.  Returns 0 on success, non-zero on failure.
 *
 *   X       — N × K column-major (CblasColMajor convention)
 *   y       — N
 *   N, K    — dimensions
 *   fam     — family-specific callbacks
 *   max_iter — typically 16
 *   tol     — convergence tolerance on the relative change in log-lik;
 *             typically 1e-7
 *   err     — out: error message on failure (static or family-owned)
 *
 * Fills fit->beta, fit->V_classical, fit->eta, fit->omitted, fit->loglik,
 * fit->loglik_0, fit->iterations, fit->converged.  Caller frees the
 * heap-allocated members via mle_fit_free. */
int mle_newton(const double *X, const double *y, long N, int K,
               const MleFamily *fam,
               int max_iter, double tol,
               MleFit *fit, const char **err);

void mle_fit_free(MleFit *fit);

/* Pre-defined families. */
extern const MleFamily mle_family_logit;
extern const MleFamily mle_family_probit;
extern const MleFamily mle_family_poisson;

#endif

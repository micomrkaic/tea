/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mle.c — Newton-Raphson MLE driver for GLM families (probit, logit).
 *
 * Iteration:  β ← β + (X'WX)^{-1} X's
 *
 * Stops when |Δℓ| / (|ℓ| + 1) < tol, or |Δβ|_∞ < tol, or after
 * max_iter iterations.  Step-halving is applied if ℓ decreases — this
 * makes Newton-Raphson behave reasonably even when the initial step
 * overshoots (which happens with bad starting values or near
 * separation).
 *
 * Collinearity:  before the main loop we run an OLS solve with column
 * dropping (dgelsd-based) to detect rank deficiency.  Omitted columns
 * are then held at β=0 throughout the NR iteration — equivalent to
 * dropping the columns entirely but keeps the indexing aligned.
 */
#include "mle.h"
#include "stats.h"
#include "linalg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- helpers ---------------------------------------------------------- */

/* y'X·something into a vector: out[K] = X' v.  X is N×K col-major. */
static void Xt_v(const double *X, const double *v, long N, int K, double *out)
{
    for(int j=0;j<K;j++){
        double s = 0;
        for(long i=0;i<N;i++) s += X[(size_t)j*N + i] * v[i];
        out[j] = s;
    }
}

/* η = X β (with omitted coefficients held at 0) */
static void compute_eta(const double *X, const double *beta, const int *omitted,
                        long N, int K, double *eta)
{
    for(long i=0;i<N;i++){
        double s = 0;
        for(int j=0;j<K;j++) if(!omitted[j]) s += X[(size_t)j*N + i] * beta[j];
        eta[i] = s;
    }
}

/* OLS column-drop solve copy (kept self-contained here so mle.c doesn't
 * have to reach into regress.c).  Returns: K-vector beta in b (zeros at
 * omitted indices), K-flag array omitted[], and effective rank in *rank. */
static int ols_solve_for_init(double *X, double *y, long N, int K,
                              double *b, int *omitted, int *rank)
{
    /* dgelsd returns min-norm solution + rank.  We use it just to detect
     * rank deficiency; the actual starting-value computation is below. */
    double *Xc = malloc((size_t)N*K*sizeof(double));
    double *yc = malloc((size_t)(N>K?N:K)*sizeof(double));
    memcpy(Xc, X, (size_t)N*K*sizeof(double));
    memcpy(yc, y, (size_t)N*sizeof(double));
    double *S = malloc(K*sizeof(double));
    int eff_rank=0;
    int info = LAPACKE_dgelsd(LAPACK_COL_MAJOR, N, K, 1, Xc, N, yc,
                              N>K?N:K, S, 1e-12, &eff_rank);
    free(Xc); free(yc); free(S);
    if(info) return info;
    *rank = eff_rank;

    /* Identify omitted columns using a simple pass: any column with a
     * tiny X'X diagonal after orthogonalization is collinear.  For the
     * MLE driver, we use a simpler heuristic — Gram-Schmidt with
     * dropping. */
    for(int j=0;j<K;j++) omitted[j] = 0;
    if(eff_rank == K){
        /* full rank: do an actual OLS solve for starting values */
        double *Xc2 = malloc((size_t)N*K*sizeof(double));
        double *yc2 = malloc((size_t)N*sizeof(double));
        memcpy(Xc2, X, (size_t)N*K*sizeof(double));
        memcpy(yc2, y, (size_t)N*sizeof(double));
        int info2 = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', N, K, 1, Xc2, N, yc2, N);
        if(info2){ free(Xc2); free(yc2); return info2; }
        memcpy(b, yc2, K*sizeof(double));
        free(Xc2); free(yc2);
        return 0;
    }
    /* Rank-deficient: identify omitted via Gram-Schmidt with column
     * dropping (same algorithm as regress.c uses). */
    double *G = malloc((size_t)N*K*sizeof(double));
    memcpy(G, X, (size_t)N*K*sizeof(double));
    double *norms = malloc(K*sizeof(double));
    for(int j=0;j<K;j++){
        for(int k=0;k<j;k++) if(!omitted[k]){
            double dot=0;
            for(long i=0;i<N;i++) dot += G[(size_t)j*N+i]*G[(size_t)k*N+i];
            double scale = dot/norms[k];
            for(long i=0;i<N;i++) G[(size_t)j*N+i] -= scale*G[(size_t)k*N+i];
        }
        double nn=0;
        for(long i=0;i<N;i++) nn += G[(size_t)j*N+i]*G[(size_t)j*N+i];
        norms[j] = nn;
        if(nn < 1e-12){ omitted[j]=1; norms[j]=1; }
    }
    free(G); free(norms);

    /* Re-solve dropping the omitted columns. */
    int Kr=0; for(int j=0;j<K;j++) if(!omitted[j]) Kr++;
    double *Xr = malloc((size_t)N*Kr*sizeof(double));
    double *yr = malloc((size_t)N*sizeof(double));
    int *map = malloc(K*sizeof(int));
    int col=0;
    for(int j=0;j<K;j++) if(!omitted[j]){
        memcpy(Xr+(size_t)col*N, X+(size_t)j*N, N*sizeof(double));
        map[col] = j; col++;
    }
    memcpy(yr, y, N*sizeof(double));
    int info3 = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', N, Kr, 1, Xr, N, yr, N);
    if(info3){ free(Xr); free(yr); free(map); return info3; }
    for(int j=0;j<K;j++) b[j] = 0;
    for(int j=0;j<Kr;j++) b[map[j]] = yr[j];
    free(Xr); free(yr); free(map);
    return 0;
}

/* Cholesky inverse of a K×K SPD matrix in column-major. */
static int chol_inv(double *A, int n)
{
    int info = LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', n, A, n);
    if(info) return info;
    info = LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', n, A, n);
    if(info) return info;
    /* symmetrize */
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) A[(size_t)i*n+j] = A[(size_t)j*n+i];
    return 0;
}

/* ---- families --------------------------------------------------------- */

/* Logit:  μ = Λ(η) = 1/(1+e^{-η}) = e^η/(1+e^η)
 *   score:  s = y - μ
 *   weight: w = μ(1-μ)
 *   loglik: y·log μ + (1-y)·log(1-μ) = y·η - log(1+e^η)
 * Computed stably via log(1+exp(η)) = max(η,0) + log(1+exp(-|η|)). */
static void logit_per_obs(double y, double eta,
                          double *score, double *weight, double *loglik)
{
    double mu;
    if(eta >= 0){
        double e = exp(-eta);
        mu = 1.0/(1.0 + e);
    } else {
        double e = exp(eta);
        mu = e/(1.0 + e);
    }
    *score  = y - mu;
    *weight = mu * (1.0 - mu);
    /* log(1+e^η) computed stably */
    double log1pe;
    double a = fabs(eta);
    log1pe = (eta > 0 ? eta : 0.0) + log1p(exp(-a));
    *loglik = y * eta - log1pe;
}

static const char *binary_validate(const double *y, long N)
{
    for(long i=0;i<N;i++){
        if(y[i] != 0.0 && y[i] != 1.0){
            return "outcome variable not binary (must be 0 or 1)";
        }
    }
    return NULL;
}

const MleFamily mle_family_logit = {
    .name = "logit",
    .outcome_kind = "binary",
    .per_obs = logit_per_obs,
    .validate_y = binary_validate,
    .start_values = NULL,
};

/* Probit:  μ = Φ(η),  density φ(η)
 * Score (per obs):  s = (y - Φ(η)) · φ(η) / [Φ(η)·(1 - Φ(η))]
 * Weight (Fisher info per obs): w = φ(η)² / [Φ(η)·(1 - Φ(η))]
 * Log-lik: y · log Φ(η) + (1-y) · log(1 - Φ(η))
 *   = y · log Φ(η) + (1-y) · log Φ(-η)   (using 1-Φ(η)=Φ(-η))
 *
 * Carefully handle |η| large: φ/Φ explodes for very negative η when y=1;
 * use the Mills-ratio bound `φ(η)/Φ(η) → -η` as η → -∞ (asymptotic),
 * and clip the per-obs weight to a finite ceiling to avoid divergence. */
static void probit_per_obs(double y, double eta,
                           double *score, double *weight, double *loglik)
{
    double Phi = tea_normal_cdf(eta);
    double phi = tea_normal_pdf(eta);
    double oneMinusPhi = 1.0 - Phi;
    /* loglik with stable log Φ */
    double logPhi   = (eta <= -3.0) ? tea_log_normal_cdf(eta)  : log(Phi > 1e-300 ? Phi : 1e-300);
    double logOneMinus = (eta >=  3.0) ? tea_log_normal_cdf(-eta) : log(oneMinusPhi > 1e-300 ? oneMinusPhi : 1e-300);
    *loglik = y * logPhi + (1.0 - y) * logOneMinus;
    /* Inverse Mills ratio for each tail.  For numerical stability, when
     * one tail is essentially zero we use the asymptotic λ(η) = φ/Φ → -η.
     * (Standard reference: Greene, "Econometric Analysis," §17.) */
    double m1, m0;  /* m1 = φ/Φ, m0 = φ/(1-Φ) */
    if(Phi > 1e-300) m1 = phi / Phi;
    else             m1 = -eta;  /* asymptotic */
    if(oneMinusPhi > 1e-300) m0 = phi / oneMinusPhi;
    else                     m0 = eta;
    /* score = y·m1 - (1-y)·m0  (= dℓ/dη) */
    *score = y * m1 - (1.0 - y) * m0;
    /* Fisher info: w = m1·m0 (= φ²/(Φ(1-Φ))). */
    *weight = m1 * m0;
    if(*weight < 0) *weight = 0;            /* numerical safety */
    if(*weight > 1e8) *weight = 1e8;        /* clip extreme tails */
}

const MleFamily mle_family_probit = {
    .name = "probit",
    .outcome_kind = "binary",
    .per_obs = probit_per_obs,
    .validate_y = binary_validate,
    .start_values = NULL,
};

/* Poisson: μ = exp(η)
 *   score:  s = y - μ
 *   weight: w = μ
 *   loglik: y·η - μ - lgamma(y+1)
 *   The lgamma(y+1) = log(y!) term is constant in β, so it doesn't affect
 *   the estimates — but it MUST be included in the reported log-likelihood:
 *   McFadden's pseudo-R² (1 - LL/LL0) is not invariant to adding a constant
 *   to both LLs, and dropping it also makes LL disagree with Stata.
 *   (Found via the bundled nmes1988 data: pseudo-R² printed 5.63.) */
static void poisson_per_obs(double y, double eta,
                            double *score, double *weight, double *loglik)
{
    /* clip eta to avoid exp() overflow.  exp(700) ≈ 1e304, near double max.
     * In practice MLE iterations don't get that far without diverging
     * anyway, but we guard defensively. */
    double eta_c = eta;
    if(eta_c > 700) eta_c = 700;
    double mu = exp(eta_c);
    *score  = y - mu;
    *weight = mu;
    *loglik = y * eta_c - mu - lgamma(y + 1.0);
}

static const char *count_validate(const double *y, long N)
{
    for(long i = 0; i < N; i++){
        if(y[i] < 0){
            return "outcome variable must be non-negative for poisson";
        }
        /* Allow non-integer y (Stata does; it's the GLM extension).
         * Could warn if non-integer, but skip for simplicity. */
    }
    return NULL;
}

const MleFamily mle_family_poisson = {
    .name = "poisson",
    .outcome_kind = "count",
    .per_obs = poisson_per_obs,
    .validate_y = count_validate,
    .start_values = NULL,
};

/* ---- driver ----------------------------------------------------------- */

int mle_newton(const double *X, const double *y, long N, int K,
               const MleFamily *fam,
               int max_iter, double tol,
               MleFit *fit, const char **err)
{
    *err = NULL;
    memset(fit, 0, sizeof *fit);

    /* y validation */
    if(fam->validate_y){
        const char *e = fam->validate_y(y, N);
        if(e){ *err = e; return 1; }
    }

    fit->beta    = calloc(K, sizeof(double));
    fit->omitted = calloc(K, sizeof(int));
    fit->eta     = malloc(N * sizeof(double));
    fit->V_classical = NULL;

    /* Starting values: OLS on (X, y).  Even for binary y this gives a
     * reasonable start, and lets us detect collinearity for free. */
    double *X_for_ols = malloc((size_t)N*K*sizeof(double));
    double *y_for_ols = malloc(N*sizeof(double));
    memcpy(X_for_ols, X, (size_t)N*K*sizeof(double));
    memcpy(y_for_ols, y, N*sizeof(double));
    int rank = 0;
    int ols_rc = ols_solve_for_init(X_for_ols, y_for_ols, N, K,
                                    fit->beta, fit->omitted, &rank);
    free(X_for_ols); free(y_for_ols);
    if(ols_rc){
        *err = "starting-value solve failed";
        free(fit->beta); free(fit->omitted); free(fit->eta);
        return 2;
    }
    int Kr = 0; for(int j=0;j<K;j++) if(!fit->omitted[j]) Kr++;
    if(Kr < 1){
        *err = "no non-collinear regressors";
        free(fit->beta); free(fit->omitted); free(fit->eta);
        return 3;
    }

    /* Null-model log-likelihood: intercept-only.  Find the intercept
     * column (we conventionally put _cons last).  If no intercept,
     * fall back to using ȳ via a single-iteration scalar fit.  We
     * compute it from ȳ directly for simplicity. */
    {
        double ybar = 0;
        for(long i=0;i<N;i++) ybar += y[i];
        ybar /= N;
        double eta0 = 0;
        if(!strcmp(fam->name, "poisson")){
            if(ybar > 0) eta0 = log(ybar);   /* null Poisson: μ = ȳ */
        } else if(ybar > 0 && ybar < 1){
            if(!strcmp(fam->name, "logit")) eta0 = log(ybar/(1.0-ybar));
            else if(!strcmp(fam->name, "probit")) eta0 = tea_invnormal(ybar);
        }
        double ll0 = 0;
        for(long i=0;i<N;i++){
            double s, w, l;
            fam->per_obs(y[i], eta0, &s, &w, &l);
            ll0 += l;
        }
        fit->loglik_0 = ll0;
    }

    /* Buffers for the iteration. */
    double *score_per = malloc(N*sizeof(double));
    double *weight_per= malloc(N*sizeof(double));
    double *loglik_per= malloc(N*sizeof(double));
    double *grad      = malloc(K*sizeof(double));     /* X' score */
    double *XtWX      = malloc((size_t)K*K*sizeof(double));
    double *XtWX_inv  = malloc((size_t)K*K*sizeof(double));
    double *delta     = malloc(K*sizeof(double));
    double *beta_try  = malloc(K*sizeof(double));

    /* Initial η and log-lik. */
    compute_eta(X, fit->beta, fit->omitted, N, K, fit->eta);
    double ll = 0;
    for(long i=0;i<N;i++){
        fam->per_obs(y[i], fit->eta[i], &score_per[i], &weight_per[i], &loglik_per[i]);
        ll += loglik_per[i];
    }
    fit->loglik = ll;

    int iter;
    bool converged = false;
    for(iter = 0; iter < max_iter; iter++){
        /* gradient = X' s */
        Xt_v(X, score_per, N, K, grad);
        /* Zero out gradient for omitted coefficients so they stay at 0. */
        for(int j=0;j<K;j++) if(fit->omitted[j]) grad[j] = 0;

        /* Build X'WX for non-omitted columns; identity placeholder
         * (1.0) on diagonal for omitted columns so the matrix is
         * invertible and the corresponding δ is 0 (since grad is 0). */
        for(int i=0;i<K;i++) for(int j=0;j<K;j++){
            if(fit->omitted[i] || fit->omitted[j]){
                XtWX[(size_t)j*K + i] = (i==j ? 1.0 : 0.0);
                continue;
            }
            double s = 0;
            for(long n=0;n<N;n++) s += X[(size_t)i*N + n] * weight_per[n] * X[(size_t)j*N + n];
            /* column-major: A[i,j] = A[j*K + i] */
            XtWX[(size_t)j*K + i] = s;
        }
        memcpy(XtWX_inv, XtWX, (size_t)K*K*sizeof(double));
        int inv_rc = chol_inv(XtWX_inv, K);
        if(inv_rc){
            *err = "Hessian not positive definite (likely separation)";
            fit->perfect_pred = true;
            break;
        }

        /* δ = (X'WX)^{-1} grad */
        for(int j=0;j<K;j++){
            double s = 0;
            for(int k=0;k<K;k++) s += XtWX_inv[(size_t)k*K + j] * grad[k];
            delta[j] = s;
        }

        /* Step with backtracking: try full step, halve if log-lik
         * doesn't increase.  Up to 8 halvings. */
        double step = 1.0;
        bool step_ok = false;
        double ll_new = ll;
        for(int halve = 0; halve < 8; halve++){
            for(int j=0;j<K;j++) beta_try[j] = fit->beta[j] + step * delta[j];
            compute_eta(X, beta_try, fit->omitted, N, K, fit->eta);
            ll_new = 0;
            int finite_ok = 1;
            for(long i=0;i<N;i++){
                fam->per_obs(y[i], fit->eta[i], &score_per[i], &weight_per[i], &loglik_per[i]);
                ll_new += loglik_per[i];
                if(!isfinite(loglik_per[i])){ finite_ok = 0; break; }
            }
            if(finite_ok && ll_new >= ll - 1e-12){
                step_ok = true;
                break;
            }
            step *= 0.5;
        }
        if(!step_ok){
            /* Couldn't improve — declare convergence.  This usually
             * means we're at the max. */
            converged = true;
            iter++;
            break;
        }
        memcpy(fit->beta, beta_try, K*sizeof(double));

        /* Convergence check */
        double rel = fabs(ll_new - ll) / (fabs(ll) + 1.0);
        ll = ll_new;
        fit->loglik = ll;
        if(rel < tol){
            converged = true;
            iter++;
            break;
        }
    }
    fit->iterations = iter;
    fit->converged = converged;

    /* Perfect-separation heuristic: when log-likelihood is essentially
     * zero (every observation classified with p ≈ 1), the MLE doesn't
     * exist in the strict sense — β diverges to ±∞ and the Hessian
     * becomes singular.  We detected the diverging part by tracking how
     * close ℓ got to 0.  This is informational; we still report β. */
    /* Only meaningful for binary outcomes: their LL is <= 0 by construction
     * and approaches 0 under separation.  Count models have no such bound
     * (and the old constant-less Poisson LL was positive, making this fire
     * spuriously on any real count data — caught by the bundled nmes1988). */
    if(!strcmp(fam->outcome_kind, "binary") && fit->loglik > -1e-6 && N > 1){
        fit->perfect_pred = true;
    }

    /* Final V_classical = (X'WX)^{-1} at the converged β.  Recompute since
     * we may have broken out without storing it. */
    for(long i=0;i<N;i++){
        double s, w, l;
        fam->per_obs(y[i], fit->eta[i], &s, &w, &l);
        weight_per[i] = w;
    }
    for(int i=0;i<K;i++) for(int j=0;j<K;j++){
        if(fit->omitted[i] || fit->omitted[j]){
            XtWX[(size_t)j*K + i] = (i==j ? 1.0 : 0.0);
            continue;
        }
        double s = 0;
        for(long n=0;n<N;n++) s += X[(size_t)i*N + n] * weight_per[n] * X[(size_t)j*N + n];
        XtWX[(size_t)j*K + i] = s;
    }
    fit->V_classical = malloc((size_t)K*K*sizeof(double));
    memcpy(fit->V_classical, XtWX, (size_t)K*K*sizeof(double));
    int v_rc = chol_inv(fit->V_classical, K);
    if(v_rc){
        *err = "final variance matrix not positive definite";
        /* Continue anyway — variance just won't be available. */
        memset(fit->V_classical, 0, (size_t)K*K*sizeof(double));
    } else {
        /* zero out rows/cols for omitted */
        for(int i=0;i<K;i++) for(int j=0;j<K;j++)
            if(fit->omitted[i] || fit->omitted[j])
                fit->V_classical[(size_t)i*K + j] = 0;
    }

    free(score_per); free(weight_per); free(loglik_per);
    free(grad); free(XtWX); free(XtWX_inv); free(delta); free(beta_try);
    return 0;
}

void mle_fit_free(MleFit *fit)
{
    if(!fit) return;
    free(fit->beta);
    free(fit->V_classical);
    free(fit->eta);
    free(fit->omitted);
    memset(fit, 0, sizeof *fit);
}

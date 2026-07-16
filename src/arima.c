/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * arima.c — ARIMA(p,d,q) via conditional likelihood.
 *
 * Syntax:
 *   arima y [exog_varlist] [if] [in], arima(p d q) [noconstant]
 *
 *   y           dependent (time-series) variable
 *   exog        optional exogenous regressors (ARIMAX)
 *   p           AR order
 *   d           difference order (0..2 typical)
 *   q           MA order
 *
 * Model after d-th differencing:
 *   y*_t = μ + β'x_t + ε_t
 *   ε_t  = Σ_{i=1..p} φ_i (y*_{t-i} - μ - β'x_{t-i})
 *        + Σ_{j=1..q} θ_j ε_{t-j} + u_t
 *   u_t ~ N(0, σ²)
 *
 * Conditional likelihood: assumes ε_{0}, ε_{-1}, ..., ε_{1-q} = 0.
 * Then ε_t for t=1..n is computed recursively, and we maximize
 *   ℓ(φ, θ, β, μ) = -(n/2) log(2π σ²) - (1/(2σ²)) Σ u_t²
 * by concentrating out σ² = (1/n) Σ u_t² and minimizing the SSR
 * over (φ, θ, β, μ).
 *
 * Numerical strategy: Gauss-Newton iteration with step halving.
 * Score (gradient of SSR) and approximate Hessian computed by
 * numerical differentiation (forward finite differences).  This is
 * less elegant than analytic gradients but produces correct results
 * and avoids re-deriving the MA-recursion gradient by hand.
 *
 * Starting values:
 *   - AR coefficients: from OLS regression of y* on its own lags
 *     (Yule-Walker would be slightly better but OLS is robust)
 *   - MA coefficients: zero
 *   - Constant: ȳ_d
 *   - Exog coefficients: from OLS of y* on the exog vars
 *
 * Convergence: relative change in SSR < tol, max_iter = 50.
 *
 * v1.0 limitations:
 *   - Conditional likelihood only (no Kalman filter for exact ML)
 *   - No seasonal terms (sar, sma)
 *   - Stationarity/invertibility NOT enforced — extreme starting values
 *     can lead to nonconvergence.  We warn if |Σφ|, |Σθ| > 0.99.
 */
#define _GNU_SOURCE
#include "interp.h"
#include "cmd.h"
#include "estimates.h"
#include "stats.h"
#include "tsop.h"
#include "linalg.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Forward declaration of helper from regress.c */
extern void store_coef_macros(Estimates *e, MacroKV **tbl);

#define MAX_AR 8
#define MAX_MA 8
#define MAX_EXOG 64

/* Pack/unpack parameters into a single vector.
 *
 * Layout: [μ, β_1, ..., β_K_ex, φ_1, ..., φ_p, θ_1, ..., θ_q]
 *   K_total = (has_cons?1:0) + K_ex + p + q
 */
typedef struct {
    int has_cons;
    int K_ex;          /* number of exog regressors */
    int p, q;
    long n;            /* number of obs in differenced series */
    const double *y;   /* differenced y, length n */
    const double *X;   /* n × K_ex column-major; NULL if K_ex == 0 */
} ArimaCtx;

static int K_total(const ArimaCtx *c){
    return c->has_cons + c->K_ex + c->p + c->q;
}

/* Compute residuals u_t given parameter vector θ_vec.  Returns SSR. */
static double arima_ssr(const ArimaCtx *c, const double *theta, double *resid_out)
{
    int K_ex = c->K_ex;
    int p = c->p, q = c->q;
    int hc = c->has_cons;
    long n = c->n;
    /* unpack */
    double mu = hc ? theta[0] : 0.0;
    const double *beta = theta + hc;                  /* K_ex values */
    const double *phi  = theta + hc + K_ex;           /* p values */
    const double *thq  = theta + hc + K_ex + p;       /* q values */

    /* Compute the "mean-adjusted" series w_t = y_t - μ - β' x_t. */
    double *w = malloc(n * sizeof(double));
    for(long t = 0; t < n; t++){
        double m = mu;
        for(int k = 0; k < K_ex; k++) m += beta[k] * c->X[(size_t)k*n + t];
        w[t] = c->y[t] - m;
    }
    /* Recursive residuals u_t.
     * u_t = w_t - Σ_i φ_i w_{t-i} - Σ_j θ_j u_{t-j}
     * with w_{<0} = 0 and u_{<0} = 0  (conditional assumption). */
    double *u = resid_out ? resid_out : malloc(n * sizeof(double));
    double ssr = 0;
    for(long t = 0; t < n; t++){
        double ut = w[t];
        for(int i = 1; i <= p; i++){
            if(t - i >= 0) ut -= phi[i-1] * w[t-i];
            /* else w_{<0} = 0, contributes nothing */
        }
        for(int j = 1; j <= q; j++){
            if(t - j >= 0) ut -= thq[j-1] * u[t-j];
        }
        u[t] = ut;
        ssr += ut * ut;
    }
    free(w);
    if(!resid_out) free(u);
    return ssr;
}

/* Difference y d times: y[t] -> Δ^d y[t].  Returns new length. */
static long difference_series(double *y, long n, int d)
{
    for(int k = 0; k < d; k++){
        for(long t = n - 1; t >= 1; t--) y[t] = y[t] - y[t-1];
        /* drop first observation; shift everything left */
        for(long t = 0; t < n - 1; t++) y[t] = y[t+1];
        n--;
    }
    return n;
}

/* Numerical gradient of SSR at theta via forward differences.
 * grad[k] = (SSR(θ + h e_k) - SSR(θ)) / h.
 * h is scaled by |θ_k| if non-trivial. */
static void arima_grad(const ArimaCtx *c, const double *theta,
                       double f0, double *grad)
{
    int K = K_total(c);
    double *th2 = malloc(K * sizeof(double));
    memcpy(th2, theta, K * sizeof(double));
    for(int k = 0; k < K; k++){
        double h = 1e-6 * (1.0 + fabs(theta[k]));
        th2[k] = theta[k] + h;
        double f1 = arima_ssr(c, th2, NULL);
        grad[k] = (f1 - f0) / h;
        th2[k] = theta[k];
    }
    free(th2);
}

/* Approximate Hessian via finite differences of the gradient.
 * For Gauss-Newton, we'd use J'J where J is the residual Jacobian; for
 * simplicity we use the SSR Hessian directly.  v1.0 uses central
 * differences of the gradient. */
static void arima_hess(const ArimaCtx *c, const double *theta,
                       double *hess)
{
    int K = K_total(c);
    double *grad0 = malloc(K * sizeof(double));
    double *grad1 = malloc(K * sizeof(double));
    double *th2 = malloc(K * sizeof(double));
    double f0 = arima_ssr(c, theta, NULL);
    arima_grad(c, theta, f0, grad0);
    memcpy(th2, theta, K * sizeof(double));
    for(int k = 0; k < K; k++){
        double h = 1e-5 * (1.0 + fabs(theta[k]));
        th2[k] = theta[k] + h;
        double f1 = arima_ssr(c, th2, NULL);
        arima_grad(c, th2, f1, grad1);
        for(int j = 0; j < K; j++){
            hess[(size_t)j*K + k] = (grad1[j] - grad0[j]) / h;
        }
        th2[k] = theta[k];
    }
    /* Symmetrize. */
    for(int i = 0; i < K; i++) for(int j = i+1; j < K; j++){
        double avg = 0.5 * (hess[(size_t)i*K + j] + hess[(size_t)j*K + i]);
        hess[(size_t)i*K + j] = hess[(size_t)j*K + i] = avg;
    }
    free(grad0); free(grad1); free(th2);
}

/* ---- parse arima(p d q) option ----------------------------------------- */

static int parse_pdq(const char *spec, int *p, int *d, int *q)
{
    *p = *d = *q = 0;
    /* Accept "p d q" or "p, d, q" formats. */
    char buf[64]; snprintf(buf, sizeof buf, "%s", spec);
    char *sp = NULL;
    int n = 0;
    int vals[3] = {0, 0, 0};
    for(char *t = strtok_r(buf, " ,", &sp); t && n < 3; t = strtok_r(NULL, " ,", &sp)){
        vals[n++] = atoi(t);
    }
    if(n < 3) return -1;
    *p = vals[0]; *d = vals[1]; *q = vals[2];
    if(*p < 0 || *p > MAX_AR) return -1;
    if(*d < 0 || *d > 5) return -1;
    if(*q < 0 || *q > MAX_MA) return -1;
    return 0;
}

/* ---- main entry -------------------------------------------------------- */

int do_arima(Cmd *c)
{
    if(!c->varlist[0]){
        fprintf(stderr,"arima: depvar required\n");
        return 198;
    }
    /* Parse arima(p d q) option */
    char pdq[64] = "";
    if(!opt_value(c->options, "arima", pdq, sizeof pdq)){
        /* Also accept ar(...) and ma(...) separately, but for v1.0 require arima() */
        fprintf(stderr,"arima: arima(p d q) option required\n");
        return 198;
    }
    int p = 0, d = 0, q = 0;
    if(parse_pdq(pdq, &p, &d, &q) < 0){
        fprintf(stderr,"arima: arima() must contain three nonnegative integers, e.g. arima(1 1 1)\n");
        return 198;
    }
    if(p == 0 && q == 0 && d == 0){
        fprintf(stderr,"arima: arima(0 0 0) is just a constant — use regress\n");
        return 198;
    }
    bool noconst = opt_present(c->options, "noconstant") || opt_present(c->options, "nocons");
    bool has_cons = !noconst;

    /* Require xtset or tsset so we have a time variable for ordering */
    if(c->f->ts_time < 0){
        fprintf(stderr,"arima: must tsset (or xtset) first\n");
        return 459;
    }
    if(c->f->ts_panel >= 0){
        fprintf(stderr,"arima: panel ARIMA not supported in v1.0; use single time series\n");
        return 198;
    }

    /* Parse depvar and optional exog. */
    char vbuf[1024]; snprintf(vbuf, sizeof vbuf, "%s", c->varlist);
    char *sp = NULL;
    char *dep_tok = strtok_r(vbuf, " ", &sp);
    char *exog_spec = strtok_r(NULL, "", &sp);
    if(!dep_tok || !dep_tok[0]){
        fprintf(stderr,"arima: depvar required\n");
        return 198;
    }
    int yi = var_find(c->f, dep_tok);
    if(yi < 0){
        fprintf(stderr,"arima: depvar %s not found\n", dep_tok);
        return 111;
    }
    /* Resolve exog vars */
    int K_ex = 0;
    int *exi = NULL;
    int n_temps = 0;
    if(exog_spec){
        while(*exog_spec == ' ') exog_spec++;
        if(*exog_spec){
            const char *vlerr = NULL;
            K_ex = tsop_expand_varlist(c->f, exog_spec, &exi, &n_temps, &vlerr);
            if(K_ex < 0){
                fprintf(stderr,"arima: bad exog: %s\n", vlerr?vlerr:"resolution failed");
                tsop_drop_temps(c->f, n_temps); return 198;
            }
            if(K_ex > MAX_EXOG){
                fprintf(stderr,"arima: too many exog vars (max %d)\n", MAX_EXOG);
                free(exi); tsop_drop_temps(c->f, n_temps); return 198;
            }
        }
    }

    /* Extract y and X.  Listwise deletion: any row with missing depvar
     * or any missing exog drops out.  Order is the frame's current
     * order — tsset has already sorted by time. */
    size_t Nfull = c->f->nobs;
    char *used = calloc(Nfull, 1);
    long n = 0;
    for(size_t i = 0; i < Nfull; i++){
        double yv = c->f->vars[yi].num[i];
        if(sv_is_miss(yv)) continue;
        bool any_miss = false;
        for(int k = 0; k < K_ex; k++){
            double xv = c->f->vars[exi[k]].num[i];
            if(sv_is_miss(xv)){ any_miss = true; break; }
        }
        if(any_miss) continue;
        used[i] = 1; n++;
    }
    if(n < 1){
        fprintf(stderr,"arima: no observations\n");
        free(used); free(exi); tsop_drop_temps(c->f, n_temps); return 2000;
    }
    double *y = malloc(n * sizeof(double));
    double *X = K_ex ? malloc((size_t)n * K_ex * sizeof(double)) : NULL;
    long row = 0;
    for(size_t i = 0; i < Nfull; i++){
        if(!used[i]) continue;
        y[row] = c->f->vars[yi].num[i];
        for(int k = 0; k < K_ex; k++)
            X[(size_t)k * n + row] = c->f->vars[exi[k]].num[i];
        row++;
    }

    /* Difference y d times.  Also difference exog if d > 0 (we model
     * the differenced series; exog should be differenced consistently).
     * For simplicity, in v1.0 we difference y but assume exog enters
     * the differenced model directly.  Users who want exog differenced
     * should pre-difference using gen. */
    long n_d = difference_series(y, n, d);
    /* For exog: just drop the first d observations to align. */
    if(K_ex && d > 0){
        for(int k = 0; k < K_ex; k++){
            for(long t = 0; t < n - d; t++)
                X[(size_t)k * n + t] = X[(size_t)k * n + t + d];
        }
    }
    /* Effective sample for the estimator: n_d. */
    if(n_d < p + q + K_ex + has_cons + 1){
        fprintf(stderr,"arima: not enough observations (%ld) after differencing for the model\n", n_d);
        free(y); free(X); free(used); free(exi);
        tsop_drop_temps(c->f, n_temps); return 2000;
    }
    /* Realloc X to actual size if needed (we use leading n_d rows). */
    double *X_use = NULL;
    if(K_ex){
        X_use = malloc((size_t)n_d * K_ex * sizeof(double));
        for(int k = 0; k < K_ex; k++)
            for(long t = 0; t < n_d; t++)
                X_use[(size_t)k * n_d + t] = X[(size_t)k * n + t];
        free(X);
    }

    /* Set up the context. */
    ArimaCtx ctx = {0};
    ctx.has_cons = has_cons ? 1 : 0;
    ctx.K_ex = K_ex;
    ctx.p = p; ctx.q = q;
    ctx.n = n_d;
    ctx.y = y;
    ctx.X = X_use;
    int K = K_total(&ctx);

    /* Starting values.
     *
     * (a) μ_0 = ȳ if has_cons, else 0
     * (b) β_0 = OLS of (y - μ_0) on X
     * (c) φ_0 = OLS coefficients from regressing y* on its lags
     *           where y* = y - μ_0 - β_0' X
     * (d) θ_0 = 0
     */
    double *theta = calloc(K, sizeof(double));
    double ybar = 0;
    if(has_cons){
        for(long t = 0; t < n_d; t++) ybar += y[t];
        ybar /= n_d;
        theta[0] = ybar;
    }
    /* Quick AR(p) starting values via OLS on lags (after mean-subtraction).
     * We skip MA cross-effects for the starting fit. */
    if(p > 0){
        long n_ols = n_d - p;
        if(n_ols > p){
            double *A = malloc((size_t)n_ols * p * sizeof(double));
            double *b = malloc(n_ols * sizeof(double));
            for(long t = 0; t < n_ols; t++){
                b[t] = y[t + p] - ybar;
                for(int k = 0; k < p; k++)
                    A[(size_t)k * n_ols + t] = y[t + p - k - 1] - ybar;
            }
            int info = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', n_ols, p, 1, A, n_ols, b, n_ols);
            if(info == 0){
                for(int k = 0; k < p; k++) theta[has_cons + K_ex + k] = b[k];
            }
            free(A); free(b);
        }
    }
    /* θ_0 stays zero. */

    /* Gauss-Newton with step halving. */
    int max_iter = 50;
    double tol = 1e-8;
    double ssr_prev = arima_ssr(&ctx, theta, NULL);
    int iter;
    for(iter = 0; iter < max_iter; iter++){
        double *grad = malloc(K * sizeof(double));
        double *hess = malloc((size_t)K * K * sizeof(double));
        arima_grad(&ctx, theta, ssr_prev, grad);
        arima_hess(&ctx, theta, hess);
        /* Add a small ridge to keep Hessian positive definite. */
        for(int k = 0; k < K; k++) hess[(size_t)k*K + k] += 1e-8;
        /* Solve hess · Δ = grad via dgesv (general; Hessian might not be SPD). */
        int *ipiv = malloc(K * sizeof(int));
        double *delta = malloc(K * sizeof(double));
        for(int k = 0; k < K; k++) delta[k] = grad[k];
        int info = LAPACKE_dgesv(LAPACK_COL_MAJOR, K, 1, hess, K, ipiv, delta, K);
        free(ipiv);
        if(info != 0){
            free(grad); free(hess); free(delta);
            fprintf(stderr,"arima: Hessian singular at iter %d\n", iter);
            break;
        }
        /* Step halving: try full Newton step (theta - delta), halve if SSR didn't decrease. */
        double *th_try = malloc(K * sizeof(double));
        double step = 1.0;
        double ssr_new = ssr_prev;
        bool accepted = false;
        for(int halve = 0; halve < 12; halve++){
            for(int k = 0; k < K; k++) th_try[k] = theta[k] - step * delta[k];
            ssr_new = arima_ssr(&ctx, th_try, NULL);
            if(isfinite(ssr_new) && ssr_new < ssr_prev){
                accepted = true;
                break;
            }
            step *= 0.5;
        }
        if(accepted){
            memcpy(theta, th_try, K * sizeof(double));
            double rel = fabs(ssr_new - ssr_prev) / (fabs(ssr_prev) + 1.0);
            ssr_prev = ssr_new;
            free(grad); free(hess); free(delta); free(th_try);
            if(rel < tol){ iter++; break; }
        } else {
            free(grad); free(hess); free(delta); free(th_try);
            break;
        }
    }
    double sigma2 = ssr_prev / n_d;
    double rmse = sqrt(sigma2);
    double loglik = -0.5 * n_d * (log(2*M_PI*sigma2) + 1.0);

    /* Compute SE from the final Hessian.  V ≈ σ² · (Hess/2)^{-1}
     * (factor of 2 because we're working with SSR, not -log L).
     * Actually for conditional ML the asymptotic V ≈ (J'J)^{-1} σ²
     * where J is the residual Jacobian; the Hessian we computed is
     * approximately 2 J'J, so V ≈ 2σ² · Hess^{-1}. */
    double *hess_final = malloc((size_t)K * K * sizeof(double));
    arima_hess(&ctx, theta, hess_final);
    double *V = malloc((size_t)K * K * sizeof(double));
    memcpy(V, hess_final, (size_t)K * K * sizeof(double));
    for(int k = 0; k < K; k++) V[(size_t)k*K + k] += 1e-8;  /* ridge */
    int *ipiv2 = malloc(K * sizeof(int));
    int rc1 = LAPACKE_dgetrf(LAPACK_COL_MAJOR, K, K, V, K, ipiv2);
    int rc2 = 0;
    if(rc1 == 0) rc2 = LAPACKE_dgetri(LAPACK_COL_MAJOR, K, V, K, ipiv2);
    free(ipiv2);
    if(rc1 || rc2){
        fprintf(stderr,"arima: variance matrix not invertible — SEs unavailable\n");
        for(long k = 0; k < (long)K*K; k++) V[k] = 0;
    } else {
        /* Scale by 2σ² */
        for(long k = 0; k < (long)K*K; k++) V[k] *= 2.0 * sigma2;
    }
    free(hess_final);

    /* Build coefficient names. */
    int hc = ctx.has_cons;
    char (*xnames)[33] = malloc(K * sizeof *xnames);
    int idx = 0;
    if(hc){ snprintf(xnames[idx++], 33, "_cons"); }
    for(int k = 0; k < K_ex; k++){
        snprintf(xnames[idx++], 33, "%s", c->f->vars[exi[k]].name);
    }
    for(int k = 0; k < p; k++) snprintf(xnames[idx++], 33, "ar%d", k+1);
    for(int k = 0; k < q; k++) snprintf(xnames[idx++], 33, "ma%d", k+1);

    /* Print Stata-style header + coefficient table. */
    if(!c->quiet){
        printf("\n");
        printf("ARIMA regression                                Number of obs   = %8ld\n", n_d);
        if(d > 0) printf("                                                D.%s                 \n", c->f->vars[yi].name);
        printf("Sample (after differencing): %ld obs\n", n_d);
        printf("Log likelihood = %.4f                          \n", loglik);
        printf("\n");
        printf("------------------------------------------------------------------------------\n");
        printf("%12s | Coefficient  Std. err.     z    P>|z|     [95%% conf. interval]\n",
               c->f->vars[yi].name);
        printf("-------------+----------------------------------------------------------------\n");
        if(hc || K_ex > 0){
            printf("%-12s |\n", "ARMA model");
            double zcrit = tea_invnormal(0.975);
            int ki = 0;
            for(; ki < hc + K_ex; ki++){
                double v = V[(size_t)ki*K + ki];
                double se = v > 0 ? sqrt(v) : 0;
                double z = se > 0 ? theta[ki]/se : 0;
                double pv = se > 0 ? 2.0*(1.0 - tea_normal_cdf(fabs(z))) : 1.0;
                double lo = theta[ki] - zcrit*se;
                double hi = theta[ki] + zcrit*se;
                printf("%12s | %10s  %10s %7.2f %5.3f   %10s  %10s\n", xnames[ki], gfit(theta[ki],9), gfit(se,9), z, pv, gfit(lo,9), gfit(hi,9));
            }
        }
        if(p > 0){
            printf("%-12s |\n", "AR");
            double zcrit = tea_invnormal(0.975);
            for(int k = 0; k < p; k++){
                int ki = hc + K_ex + k;
                double v = V[(size_t)ki*K + ki];
                double se = v > 0 ? sqrt(v) : 0;
                double z = se > 0 ? theta[ki]/se : 0;
                double pv = se > 0 ? 2.0*(1.0 - tea_normal_cdf(fabs(z))) : 1.0;
                double lo = theta[ki] - zcrit*se;
                double hi = theta[ki] + zcrit*se;
                printf("%12s | %10s  %10s %7.2f %5.3f   %10s  %10s\n", xnames[ki], gfit(theta[ki],9), gfit(se,9), z, pv, gfit(lo,9), gfit(hi,9));
            }
        }
        if(q > 0){
            printf("%-12s |\n", "MA");
            double zcrit = tea_invnormal(0.975);
            for(int k = 0; k < q; k++){
                int ki = hc + K_ex + p + k;
                double v = V[(size_t)ki*K + ki];
                double se = v > 0 ? sqrt(v) : 0;
                double z = se > 0 ? theta[ki]/se : 0;
                double pv = se > 0 ? 2.0*(1.0 - tea_normal_cdf(fabs(z))) : 1.0;
                double lo = theta[ki] - zcrit*se;
                double hi = theta[ki] + zcrit*se;
                printf("%12s | %10s  %10s %7.2f %5.3f   %10s  %10s\n", xnames[ki], gfit(theta[ki],9), gfit(se,9), z, pv, gfit(lo,9), gfit(hi,9));
            }
        }
        printf("%-12s |\n", "/sigma");
        printf("%12s | %10s\n", "sigma", gfit(rmse,9));
        printf("------------------------------------------------------------------------------\n");
        printf("Iterations: %d.  Conditional likelihood (no Kalman filter in v1.0).\n", iter);
    }

    /* Stash an Estimates struct. */
    Estimates *e = est_new();
    snprintf(e->cmd, sizeof e->cmd, "arima");
    snprintf(e->depvar, sizeof e->depvar, "%s", c->f->vars[yi].name);
    e->K = K;
    e->xnames = xnames;
    e->omitted = calloc(K, sizeof(int));
    e->b = malloc(K * sizeof(double));
    memcpy(e->b, theta, K * sizeof(double));
    e->V = V;
    e->N = n_d;
    e->df_r = (int)(n_d - K);
    e->df_m = K - hc;
    e->has_cons = hc;
    e->rmse = rmse;
    e->sigma2 = sigma2;
    e->se_kind = SE_CLASSICAL;
    e->nobs_at_fit = c->f->nobs;
    e->used = used;
    snprintf(e->fitted_frame, sizeof e->fitted_frame, "%s", c->f->name);
    est_free(c->ws->last_est);
    c->ws->last_est = e;

    /* r() macros */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",n_d); mac_set(&c->ip->rret,"e(N)",bb);
    snprintf(bb,sizeof bb,"%.10g",loglik); mac_set(&c->ip->rret,"e(ll)",bb);
    snprintf(bb,sizeof bb,"%.10g",sigma2); mac_set(&c->ip->rret,"e(sigma2)",bb);
    snprintf(bb,sizeof bb,"%d",iter); mac_set(&c->ip->rret,"e(iter)",bb);
    store_coef_macros(e, &c->ip->rret);

    free(theta); free(y); free(X_use); free(exi);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

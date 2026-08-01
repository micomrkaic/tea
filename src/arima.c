/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * arima.c — ARIMA(p,d,q) via EXACT maximum likelihood on the
 * state-space engine (kalman.c), per DESIGN_SSPACE.md §8.2.
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
 * Method: the ARMA(p,q) process on the d-times-differenced,
 * mean/regression-adjusted series is cast in Harvey companion form
 * (m = max(p, q+1) states) with stationary Lyapunov initialization,
 * and the exact Gaussian likelihood is evaluated by the univariate
 * Kalman filter.  Optimization is BFGS2 with central-difference
 * gradients from three deterministic starting points, over
 * transformed parameters: Monahan's partial-autocorrelation transform
 * for the AR and MA polynomials (stationarity and invertibility
 * enforced), log-sigma for the scale.  Standard errors are the
 * observed information matrix (numerical Hessian of the exact
 * likelihood in the transformed space, delta method back to the
 * reported scale).  COMPATIBILITY.md records the conventions:
 * Stata differences first and reports the regression-intercept
 * (mean-form) constant — both matched here — but defaults to
 * OPG/BHHH standard errors, which differ from OIM in finite samples.
 *
 * The conditional-SSR recursion (arima_ssr) is retained only to seed
 * the variance starting value.
 *
 * Remaining limitations:
 *   - No seasonal terms (sarima) yet; staged per the design note.
 */
#define _GNU_SOURCE
#include "interp.h"
#include "cmd.h"
#include "estimates.h"
#include "stats.h"
#include "tsop.h"
#include <lapacke.h>
#include <gsl/gsl_multimin.h>
#include "kalman.h"
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

/* Approximate Hessian via finite differences of the gradient.
 * For Gauss-Newton, we'd use J'J where J is the residual Jacobian; for
 * simplicity we use the SSR Hessian directly.  v1.0 uses central
 * differences of the gradient. */

/* ---- parse arima(p d q) option ----------------------------------------- */

/* ---- exact ML via the state-space engine (DESIGN_SSPACE.md §8.2) ----
 *
 * Parameterization (Decision 7): AR and MA polynomials through
 * Monahan's partial-autocorrelation transform (stationarity and
 * invertibility enforced during optimization), variance as log-sigma.
 * Unconstrained z maps to partials r = z/sqrt(1+z^2) in (-1,1), then
 * the Durbin-Levinson recursion maps partials to coefficients. */
static void pacf_to_coef(const double *r, int k, double *out)
{
    double work[16];
    for(int i = 0; i < k; i++){
        out[i] = r[i];
        for(int j = 0; j < i; j++) work[j] = out[j] - r[i]*out[i-1-j];
        for(int j = 0; j < i; j++) out[j] = work[j];
    }
}
static void z_to_coef(const double *z, int k, double *out)
{
    double r[16];
    for(int i = 0; i < k; i++) r[i] = z[i]/sqrt(1.0 + z[i]*z[i]);
    pacf_to_coef(r, k, out);
}
/* inverse Durbin-Levinson: coefficients -> partials -> z (for starting
 * values); clips partials into the open interval */
static void coef_to_z(const double *coef, int k, double *z)
{
    double a[16], b[16];
    memcpy(a, coef, (size_t)k*sizeof(double));
    for(int i = k-1; i >= 0; i--){
        double r = a[i];
        if(r >  0.95) r =  0.95;
        if(r < -0.95) r = -0.95;
        z[i] = r/sqrt(1.0 - r*r);
        if(i > 0){
            double den = 1.0 - r*r;
            for(int j = 0; j < i; j++)
                b[j] = (a[j] + r*a[i-1-j])/den;
            memcpy(a, b, (size_t)i*sizeof(double));
        }
    }
}

typedef struct {
    const double *y;      /* differenced series */
    const double *X;      /* exog, col-major n x K_ex */
    long n;
    int K_ex, hc, p, q;
} ExactCtx;

/* psi layout: [mu (if hc), beta (K_ex), zAR (p), zMA (q), log sigma] */
static double exact_negll(const gsl_vector *x, void *params)
{
    ExactCtx *e = params;
    int p = e->p, q = e->q, m = (p > q+1) ? p : q+1;
    double psi[40];
    for(size_t i = 0; i < x->size; i++) psi[i] = gsl_vector_get(x, i);
    double mu = e->hc ? psi[0] : 0.0;
    const double *beta = psi + e->hc;
    double phi[16] = {0}, thq[16] = {0};
    if(p) z_to_coef(psi + e->hc + e->K_ex, p, phi);
    if(q) z_to_coef(psi + e->hc + e->K_ex + p, q, thq);
    double s2 = exp(2.0*psi[e->hc + e->K_ex + p + q]);
    /* Harvey-form ARMA state space */
    double Z[16] = {0}, T[256] = {0}, R[256] = {0}, H[1] = {0};
    double a1[16] = {0}, Pst[256], Pinf[256] = {0}, RQR[256] = {0};
    Z[0] = 1.0;
    for(int j = 0; j < p; j++) T[j] = phi[j];               /* first column */
    for(int j = 0; j < m-1; j++) T[(size_t)(j+1)*m + j] = 1.0;
    /* R: m x 1 disturbance loading [1, th1, ..., th_{m-1}]' */
    R[0] = 1.0;
    for(int j = 0; j < q; j++) R[j+1] = thq[j];
    double Q1[1] = { s2 };
    for(int r2 = 0; r2 < m; r2++) for(int c2 = 0; c2 < m; c2++)
        RQR[(size_t)c2*m + r2] = R[r2]*R[c2]*s2;
    if(ss_lyapunov(T, RQR, m, Pst)) return 1e30;
    for(int i = 0; i < m*m; i++) if(!isfinite(Pst[i])) return 1e30;
    SSModel M = { m, 1, 1, Z, H, T, R, Q1, a1, Pst, Pinf };
    double w_stack[4096];
    double *w = e->n <= 4096 ? w_stack : malloc((size_t)e->n*sizeof(double));
    for(long t = 0; t < e->n; t++){
        double mm = mu;
        for(int k = 0; k < e->K_ex; k++) mm += beta[k]*e->X[(size_t)k*e->n + t];
        w[t] = e->y[t] - mm;
    }
    double ll = ss_loglik(&M, w, e->n, NULL, NULL);
    if(w != w_stack) free(w);
    if(!isfinite(ll)) return 1e30;
    return -ll;
}
static void exact_negll_df(const gsl_vector *x, void *params, gsl_vector *g)
{
    double h = 1e-5;
    gsl_vector *xp = gsl_vector_alloc(x->size);
    for(size_t i = 0; i < x->size; i++){
        gsl_vector_memcpy(xp, x);
        gsl_vector_set(xp, i, gsl_vector_get(x,i)+h);
        double fp = exact_negll(xp, params);
        gsl_vector_set(xp, i, gsl_vector_get(x,i)-h);
        double fm = exact_negll(xp, params);
        gsl_vector_set(g, i, (fp-fm)/(2*h));
    }
    gsl_vector_free(xp);
}
static void exact_negll_fdf(const gsl_vector *x, void *params, double *f, gsl_vector *g)
{
    *f = exact_negll(x, params); exact_negll_df(x, params, g);
}

/* untransformed coefficient vector [mu, beta, phi, thq] from psi */
static void psi_to_theta(const ExactCtx *e, const double *psi, double *theta)
{
    int idx = 0;
    if(e->hc) theta[idx++] = psi[0];
    for(int k = 0; k < e->K_ex; k++) theta[idx++] = psi[e->hc + k];
    double tmp[16];
    if(e->p){ z_to_coef(psi + e->hc + e->K_ex, e->p, tmp);
              for(int k = 0; k < e->p; k++) theta[idx++] = tmp[k]; }
    if(e->q){ z_to_coef(psi + e->hc + e->K_ex + e->p, e->q, tmp);
              for(int k = 0; k < e->q; k++) theta[idx++] = tmp[k]; }
}

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

    /* Exact ML via the state-space engine (BFGS2 on transformed
     * parameters, central-difference gradients, deterministic
     * multistart — the ucm pattern; DESIGN_SSPACE.md Decisions 6/7). */
    ExactCtx ectx = { y, ctx.X, n_d, K_ex, has_cons, p, q };
    int Kp = K + 1;                       /* + log sigma */
    double psi0[40] = {0};
    {
        int idx = 0;
        if(has_cons) psi0[idx++] = theta[0];
        for(int k = 0; k < K_ex; k++) psi0[idx++] = theta[has_cons + k];
        if(p){
            double zar[16];
            coef_to_z(theta + has_cons + K_ex, p, zar);
            for(int k = 0; k < p; k++) psi0[idx++] = zar[k];
        }
        for(int k = 0; k < q; k++) psi0[idx++] = 0.0;
        /* sigma start from the conditional-SSR at the starting values */
        double ssr0 = arima_ssr(&ctx, theta, NULL);
        psi0[idx] = 0.5*log((isfinite(ssr0) && ssr0 > 0 ? ssr0 : 1.0)/n_d);
    }
    double psi[40]; double ll = -1e300; int iter = 0;
    {
        double starts[3][40];
        int nstarts = (p + q) > 0 ? 3 : 1;
        memcpy(starts[0], psi0, sizeof psi0);
        memcpy(starts[1], psi0, sizeof psi0);
        memcpy(starts[2], psi0, sizeof psi0);
        for(int k = 0; k < p + q; k++){
            starts[1][has_cons + K_ex + k] =  0.75;   /* partials ~ +0.6 */
            starts[2][has_cons + K_ex + k] = -0.75;
        }
        gsl_multimin_function_fdf F =
            { exact_negll, exact_negll_df, exact_negll_fdf, (size_t)Kp, &ectx };
        for(int s0 = 0; s0 < nstarts; s0++){
            gsl_multimin_fdfminimizer *s =
                gsl_multimin_fdfminimizer_alloc(
                    gsl_multimin_fdfminimizer_vector_bfgs2, Kp);
            gsl_vector *x0 = gsl_vector_alloc(Kp);
            for(int i = 0; i < Kp; i++) gsl_vector_set(x0, i, starts[s0][i]);
            gsl_multimin_fdfminimizer_set(s, &F, x0, 0.1, 1e-4);
            int status = GSL_CONTINUE, it = 0;
            double f_prev = 1e300;
            while(status == GSL_CONTINUE && it < 300){
                it++;
                status = gsl_multimin_fdfminimizer_iterate(s);
                if(status) break;
                if(fabs(f_prev - s->f) < 1e-10*(1.0 + fabs(s->f))) break;
                f_prev = s->f;
                status = gsl_multimin_test_gradient(s->gradient, 1e-7);
            }
            iter += it;
            if(-s->f > ll){
                ll = -s->f;
                for(int i = 0; i < Kp; i++) psi[i] = gsl_vector_get(s->x, i);
            }
            gsl_multimin_fdfminimizer_free(s);
            gsl_vector_free(x0);
        }
    }
    psi_to_theta(&ectx, psi, theta);
    double sigma2 = exp(2.0*psi[K]);
    double rmse = sqrt(sigma2);
    double loglik = ll;

    /* OIM standard errors: numerical Hessian of -ll in psi-space
     * (always inside the stationary region), inverted, then mapped to
     * the reported [mu, beta, phi, thq] scale by the delta method with
     * a finite-difference Jacobian.  (COMPATIBILITY.md: Stata's arima
     * default is OPG/BHHH, so SEs differ slightly in finite samples;
     * point estimates and the log likelihood are the comparable
     * quantities.) */
    double *V = malloc((size_t)K * K * sizeof(double));
    {
        double Hp[1600];
        double h = 1e-4;
        for(int i = 0; i < Kp; i++) for(int j = 0; j <= i; j++){
            double tpp[40], tpm[40], tmp2[40], tmm[40];
            memcpy(tpp, psi, sizeof psi); memcpy(tpm, psi, sizeof psi);
            memcpy(tmp2, psi, sizeof psi); memcpy(tmm, psi, sizeof psi);
            tpp[i] += h; tpp[j] += h;  tpm[i] += h; tpm[j] -= h;
            tmp2[i] -= h; tmp2[j] += h; tmm[i] -= h; tmm[j] -= h;
            gsl_vector_view vv2;
            double fpp, fpm, fmp, fmm;
            vv2 = gsl_vector_view_array(tpp, Kp); fpp = exact_negll(&vv2.vector, &ectx);
            vv2 = gsl_vector_view_array(tpm, Kp); fpm = exact_negll(&vv2.vector, &ectx);
            vv2 = gsl_vector_view_array(tmp2, Kp); fmp = exact_negll(&vv2.vector, &ectx);
            vv2 = gsl_vector_view_array(tmm, Kp); fmm = exact_negll(&vv2.vector, &ectx);
            double hij = (fpp - fpm - fmp + fmm)/(4*h*h);
            Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i] = hij;
        }
        int okH = (LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0)
               && (LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0);
        if(!okH){
            fprintf(stderr, "arima: information matrix not positive definite — SEs unavailable\n");
            memset(V, 0, (size_t)K*K*sizeof(double));
        } else {
            for(int i = 0; i < Kp; i++) for(int j = i+1; j < Kp; j++)
                Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i];
            /* J: K x Kp Jacobian of theta(psi) */
            double J[640];
            double hj = 1e-6;
            double thp[40], thm[40];
            for(int j = 0; j < Kp; j++){
                double pp[40], pm[40];
                memcpy(pp, psi, sizeof psi); memcpy(pm, psi, sizeof psi);
                pp[j] += hj; pm[j] -= hj;
                psi_to_theta(&ectx, pp, thp);
                psi_to_theta(&ectx, pm, thm);
                for(int i = 0; i < K; i++)
                    J[(size_t)j*K + i] = (thp[i] - thm[i])/(2*hj);
            }
            /* V = J Hinv J' */
            double JH[640];
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, Kp, Kp,
                        1.0, J, K, Hp, Kp, 0.0, JH, K);
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, K, K, Kp,
                        1.0, JH, K, J, K, 0.0, V, K);
        }
    }

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
        printf("Iterations: %d.  Exact ML via the Kalman filter (state-space engine).\n", iter);
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

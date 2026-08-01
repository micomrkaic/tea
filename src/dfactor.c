/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dfactor.c — dynamic-factor models (DESIGN_DFACTOR.md, signed).
 *
 *   y_t = mu + B x_t + Lambda f_t + u_t,   u_t ~ N(0, diag(s2_i))
 *   f_t = A_1 f_{t-1} + ... + A_p f_{t-p} + v_t,  v_t ~ N(0, I_k)
 *
 * Stata surface: dfactor (y1 y2 ... [= exog] [, noconstant])
 *                        (f1 [f2 ...] = , ar(1[/p]))
 *                [, constraints(numlist) smfactor(stub)]
 *
 * k <= 4 factors, p <= 4 lags, full A_j matrices (factors interact).
 * Normalization Var(v)=I_k (D2).  Identification for k >= 2 through
 * the constraints subsystem (constraint.c): the parameter map is
 * reparameterized as theta = theta_p + N psi and the ucm/arima
 * BFGS2-multistart-OIM machinery runs unchanged on psi.
 * Deterministic sign convention (first nonzero loading of each factor
 * made positive by a joint flip) applies only when unconstrained.
 * Starting values from principal components (dsyev).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_cdf.h>
#include "cmd.h"
#include "dataset.h"
#include "value.h"
#include "kalman.h"
#include "estimates.h"
#include "interp.h"
#include <lapacke.h>

extern void store_coef_macros(Estimates *e, MacroKV **tbl);
extern int cns_build(Workspace *ws, const char *numlist, const char (*names)[33],
                     int K, double **Rout, double **rout, int *nc);
extern int cns_nullspace(const double *R, const double *r, int nc, int K,
                         double *theta_p, double **Nout, int *nfree);

static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

#define DF_MAXN   12
#define DF_MAXK    4
#define DF_MAXP    4
#define DF_MAXX    4
#define DF_MAXPAR 160

typedef struct {
    int n, k, p, Kx, hc;             /* observables, factors, lags, exog, cons */
    int yi[DF_MAXN], xi[DF_MAXX];
    char fnames[DF_MAXK][33];
    /* mean-parameter layout (constrainable):
     * [mu_i] (n if hc) | [b_ij] (n*Kx) | [lam_ig] (n*k) | [A_l[g,h]] (p*k*k) */
    int Km;                          /* # mean params */
    int Kall;                        /* Km + n (log-sigmas) */
    /* constraint reparameterization: theta_mean = tp + N psi_mean */
    double *tp;  double *Nb;  int nfree;  int ncns;
    /* data */
    const double *y;                 /* n x Tn col-major, NaN missing */
    const double *X;                 /* Kx x Tn col-major */
    long Tn;
} DfSpec;

static int df_im(const DfSpec *d, int i){ return i; }                    /* mu   */
static int df_ib(const DfSpec *d, int i, int j){ return d->hc*d->n + j*d->n + i; }
static int df_il(const DfSpec *d, int i, int g){
    return d->hc*d->n + d->Kx*d->n + g*d->n + i; }
static int df_ia(const DfSpec *d, int l, int g, int h){
    return d->hc*d->n + d->Kx*d->n + d->k*d->n + ((l*d->k + h)*d->k + g); }

/* full-psi -> negative loglik.  psi = [psi_mean (nfree) | lsig (n)] */
static double df_negll(const gsl_vector *x, void *params)
{
    DfSpec *d = params;
    int n = d->n, k = d->k, p = d->p, m = k*p;
    double th[DF_MAXPAR];
    /* mean block */
    for(int i = 0; i < d->Km; i++) th[i] = d->tp[i];
    for(int j = 0; j < d->nfree; j++){
        double pj = gsl_vector_get(x, j);
        for(int i = 0; i < d->Km; i++) th[i] += d->Nb[(size_t)j*d->Km + i]*pj;
    }
    double Hd[DF_MAXN];
    for(int i = 0; i < n; i++)
        Hd[i] = exp(2.0*gsl_vector_get(x, d->nfree + i));
    /* state space */
    double Z[DF_MAXN*DF_MAXK*DF_MAXP];  memset(Z, 0, sizeof Z);
    double T[16*16] = {0}, R[16*DF_MAXK] = {0}, Q[DF_MAXK*DF_MAXK] = {0};
    double a1[16] = {0}, Pst[16*16], Pinf[16*16] = {0}, RQR[16*16] = {0};
    for(int i = 0; i < n; i++) for(int g = 0; g < k; g++)
        Z[(size_t)g*n + i] = th[df_il(d, i, g)];
    for(int l = 0; l < p; l++)
        for(int g = 0; g < k; g++) for(int h = 0; h < k; h++)
            T[(size_t)(l*k + h)*m + g] = th[df_ia(d, l, g, h)];
    for(int j = 0; j < m - k; j++) T[(size_t)j*m + (k + j)] = 1.0;
    for(int g = 0; g < k; g++){ R[(size_t)g*m + g] = 1.0; Q[(size_t)g*k + g] = 1.0; }
    for(int g = 0; g < k; g++) RQR[(size_t)g*m + g] = 1.0;
    if(ss_lyapunov(T, RQR, m, Pst)) return 1e30;
    for(int i = 0; i < m*m; i++) if(!isfinite(Pst[i]) || fabs(Pst[i]) > 1e12) return 1e30;
    /* A nonstationary T can still yield an algebraic Lyapunov "solution"
     * — but not a positive-definite one.  Reject non-PD P (evaluation
     * error per the design ruling, not a crash): dpotrf on a copy. */
    {
        double Pchk[16*16];
        memcpy(Pchk, Pst, (size_t)m*m*sizeof(double));
        for(int i = 0; i < m; i++) Pchk[(size_t)i*m + i] += 1e-10;
        if(LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', m, Pchk, m) != 0) return 1e30;
    }
    SSModel M = { m, n, k, Z, Hd, T, R, Q, a1, Pst, Pinf };
    /* mean-adjusted data */
    long Tn = d->Tn;
    double *w = malloc((size_t)n*Tn*sizeof(double));
    for(int i = 0; i < n; i++){
        double mu = d->hc ? th[df_im(d, i)] : 0.0;
        for(long t = 0; t < Tn; t++){
            double v = d->y[(size_t)i*Tn + t];
            if(isnan(v)){ w[(size_t)i*Tn + t] = NAN; continue; }
            double mm = mu;
            for(int j = 0; j < d->Kx; j++)
                mm += th[df_ib(d, i, j)]*d->X[(size_t)j*Tn + t];
            w[(size_t)i*Tn + t] = v - mm;
        }
    }
    double ll = ss_loglik(&M, w, Tn, NULL, NULL);
    free(w);
    if(!isfinite(ll)) return 1e30;
    return -ll;
}
static void df_negll_df(const gsl_vector *x, void *params, gsl_vector *g)
{
    double h = 1e-5;
    gsl_vector *xp = gsl_vector_alloc(x->size);
    for(size_t i = 0; i < x->size; i++){
        gsl_vector_memcpy(xp, x);
        gsl_vector_set(xp, i, gsl_vector_get(x, i)+h);
        double fp = df_negll(xp, params);
        gsl_vector_set(xp, i, gsl_vector_get(x, i)-h);
        double fm = df_negll(xp, params);
        gsl_vector_set(g, i, (fp-fm)/(2*h));
    }
    gsl_vector_free(xp);
}
static void df_negll_fdf(const gsl_vector *x, void *params, double *f, gsl_vector *g)
{ *f = df_negll(x, params); df_negll_df(x, params, g); }

/* project a full theta_mean vector onto psi_mean (N' (theta - tp);
 * exact when theta satisfies the constraints, best-approx otherwise) */
static void df_theta_to_psi(const DfSpec *d, const double *th, double *psi)
{
    for(int j = 0; j < d->nfree; j++){
        double s = 0;
        for(int i = 0; i < d->Km; i++)
            s += d->Nb[(size_t)j*d->Km + i]*(th[i] - d->tp[i]);
        psi[j] = s;
    }
}

int do_dfactor(Cmd *c);
int do_dfactor(Cmd *c)
{
    DfSpec d; memset(&d, 0, sizeof d);
    d.hc = 1;
    /* ---- parse the two parenthesized equations ---- */
    const char *s = c->varlist;
    char eq1[512] = "", eq2[512] = "";
    {
        int depth = 0; const char *p = s; const char *st = NULL; int which = 0;
        for(; *p; p++){
            if(*p == '('){ if(!depth) st = p+1; depth++; }
            else if(*p == ')'){
                depth--;
                if(!depth && st){
                    if(which == 0) snprintf(eq1, sizeof eq1, "%.*s", (int)(p-st), st);
                    else if(which == 1) snprintf(eq2, sizeof eq2, "%.*s", (int)(p-st), st);
                    which++;
                }
            }
        }
        if(which != 2){
            tea_err("dfactor: syntax is dfactor (y1 y2 ... [= exog][, noconstant]) (f1 [f2 ...] = , ar(#))\n");
            return 198;
        }
    }
    /* eq1: y-list [= exog-list] [, noconstant] */
    {
        char *comma = strchr(eq1, ',');
        if(comma){
            if(strstr(comma+1, "nocons")) d.hc = 0;
            *comma = 0;
        }
        char *eqs = strchr(eq1, '=');
        char ylist[512];
        if(eqs){ snprintf(ylist, sizeof ylist, "%.*s", (int)(eqs-eq1), eq1); }
        else snprintf(ylist, sizeof ylist, "%s", eq1);
        int *vv = NULL;
        int nv = varlist_expand(c->f, ylist, &vv);
        if(nv < 2 || nv > DF_MAXN){
            tea_err("dfactor: need 2..%d observed variables (got %d)\n", DF_MAXN, nv);
            free(vv); return 198;
        }
        d.n = nv;
        for(int i = 0; i < nv; i++) d.yi[i] = vv[i];
        free(vv);
        if(eqs){
            int *xv = NULL;
            int nx = varlist_expand(c->f, eqs+1, &xv);
            if(nx > DF_MAXX){ tea_err("dfactor: at most %d exog vars\n", DF_MAXX);
                              free(xv); return 198; }
            d.Kx = nx > 0 ? nx : 0;
            for(int j = 0; j < d.Kx; j++) d.xi[j] = xv[j];
            free(xv);
        }
    }
    /* eq2: f-names = , ar(spec) */
    {
        char *eqs = strchr(eq2, '=');
        if(!eqs){ tea_err("dfactor: factor equation needs `= , ar(#)'\n"); return 198; }
        *eqs = 0;
        char *tok = strtok(eq2, " \t");
        while(tok && d.k < DF_MAXK + 1){
            if(d.k >= DF_MAXK){ tea_err("dfactor: at most %d factors\n", DF_MAXK); return 198; }
            snprintf(d.fnames[d.k++], 33, "%s", tok);
            tok = strtok(NULL, " \t");
        }
        if(!d.k){ tea_err("dfactor: no factor names\n"); return 198; }
        char *ar = strstr(eqs+1, "ar(");
        if(!ar){ tea_err("dfactor: factor equation needs ar(#) or ar(1/#)\n"); return 198; }
        int a = 0, b = 0;
        if(sscanf(ar, "ar(%d/%d)", &a, &b) == 2) d.p = b;
        else if(sscanf(ar, "ar(%d)", &a) == 1) d.p = a;
        if(d.p < 1 || d.p > DF_MAXP){ tea_err("dfactor: ar order must be 1..%d\n", DF_MAXP); return 198; }
    }
    /* ---- sample: tsset, gap-free time, missing values legal ---- */
    if(c->f->ts_time < 0){ tea_err("dfactor: data not tsset\n"); return 111; }
    if(c->f->ts_panel >= 0){ tea_err("dfactor: panel data not supported\n"); return 111; }
    Variable *tv = &c->f->vars[c->f->ts_time];
    long first = -1, last = -1;
    for(size_t r = 0; r < c->f->nobs; r++){
        if(sv_is_miss(tv->num[r])) continue;
        if(first < 0) first = (long)r;
        last = (long)r;
    }
    if(first < 0){ tea_err("dfactor: no observations\n"); return 2000; }
    for(long r = first; r < last; r++)
        if(tv->num[r+1] != tv->num[r] + c->f->ts_delta){
            tea_err("dfactor: time variable has gaps\n"); return 111; }
    long Tn = last - first + 1;
    double *y = malloc((size_t)d.n*Tn*sizeof(double));
    double *X = d.Kx ? malloc((size_t)d.Kx*Tn*sizeof(double)) : NULL;
    long nobs_scalar = 0; long tobs = 0;
    for(long t = 0; t < Tn; t++){
        int any = 0;
        for(int i = 0; i < d.n; i++){
            double v = c->f->vars[d.yi[i]].num[first+t];
            if(sv_is_miss(v)) y[(size_t)i*Tn + t] = NAN;
            else { y[(size_t)i*Tn + t] = v; nobs_scalar++; any = 1; }
        }
        if(any) tobs++;
        for(int j = 0; j < d.Kx; j++){
            double v = c->f->vars[d.xi[j]].num[first+t];
            if(sv_is_miss(v)){ tea_err("dfactor: missing exog values not supported\n");
                free(y); free(X); return 2000; }
            X[(size_t)j*Tn + t] = v;
        }
    }
    d.y = y; d.X = X; d.Tn = Tn;
    /* ---- parameter names ---- */
    d.Km = d.hc*d.n + d.Kx*d.n + d.k*d.n + d.p*d.k*d.k;
    d.Kall = d.Km + d.n;
    if(d.Km > DF_MAXPAR - DF_MAXN){ tea_err("dfactor: model too large\n");
        free(y); free(X); return 198; }
    char (*mnames)[33] = calloc(d.Km, 33);
    for(int i = 0; i < d.n; i++){
        const char *yn = c->f->vars[d.yi[i]].name;
        if(d.hc) snprintf(mnames[df_im(&d, i)], 33, "[%s]_cons", yn);
        for(int j = 0; j < d.Kx; j++)
            snprintf(mnames[df_ib(&d, i, j)], 33, "[%s]%s", yn, c->f->vars[d.xi[j]].name);
        for(int g = 0; g < d.k; g++)
            snprintf(mnames[df_il(&d, i, g)], 33, "[%s]%s", yn, d.fnames[g]);
    }
    for(int l = 0; l < d.p; l++)
        for(int g = 0; g < d.k; g++) for(int h = 0; h < d.k; h++){
            if(l == 0) snprintf(mnames[df_ia(&d, l, g, h)], 33, "[%s]L.%s",
                                d.fnames[g], d.fnames[h]);
            else snprintf(mnames[df_ia(&d, l, g, h)], 33, "[%s]L%d.%s",
                          d.fnames[g], l+1, d.fnames[h]);
        }
    /* ---- constraints ---- */
    char cnl[256] = "";
    d.ncns = 0;
    if(opt_value(c->options, "constraints", cnl, sizeof cnl) && cnl[0]){
        double *R = NULL, *r = NULL; int nc = 0;
        if(cns_build(c->ws, cnl, (const char (*)[33])mnames, d.Km, &R, &r, &nc)){
            free(mnames); free(y); free(X); return 198;
        }
        d.tp = calloc(d.Km, sizeof(double));
        int rk = cns_nullspace(R, r, nc, d.Km, d.tp, &d.Nb, &d.nfree);
        free(R); free(r);
        if(rk < 0){ tea_err("dfactor: constraint matrix decomposition failed\n");
            free(d.tp); free(mnames); free(y); free(X); return 198; }
        d.ncns = rk;
    } else {
        d.tp = calloc(d.Km, sizeof(double));
        d.Nb = calloc((size_t)d.Km*d.Km, sizeof(double));
        for(int i = 0; i < d.Km; i++) d.Nb[(size_t)i*d.Km + i] = 1.0;
        d.nfree = d.Km;
    }
    int Kp = d.nfree + d.n;
    /* ---- starting values: principal components ---- */
    double th0[DF_MAXPAR] = {0};
    double lsig0[DF_MAXN];
    {
        /* covariance of pairwise-complete demeaned data */
        double mean[DF_MAXN] = {0}; long cnt[DF_MAXN] = {0};
        for(int i = 0; i < d.n; i++){
            for(long t = 0; t < Tn; t++){
                double v = y[(size_t)i*Tn + t];
                if(!isnan(v)){ mean[i] += v; cnt[i]++; }
            }
            mean[i] /= cnt[i] > 0 ? cnt[i] : 1;
        }
        double C[DF_MAXN*DF_MAXN] = {0};
        for(int i = 0; i < d.n; i++) for(int j = 0; j <= i; j++){
            double acc = 0; long nn = 0;
            for(long t = 0; t < Tn; t++){
                double a = y[(size_t)i*Tn + t], b = y[(size_t)j*Tn + t];
                if(!isnan(a) && !isnan(b)){ acc += (a-mean[i])*(b-mean[j]); nn++; }
            }
            C[(size_t)j*d.n + i] = C[(size_t)i*d.n + j] = nn > 1 ? acc/(nn-1) : 0;
        }
        double ev[DF_MAXN];
        double Cw[DF_MAXN*DF_MAXN];
        memcpy(Cw, C, sizeof C);
        LAPACKE_dsyev(LAPACK_COL_MAJOR, 'V', 'U', d.n, Cw, d.n, ev); /* asc */
        for(int i = 0; i < d.n; i++){
            if(d.hc) th0[df_im(&d, i)] = mean[i];
            double resid = C[(size_t)i*d.n + i];
            for(int g = 0; g < d.k; g++){
                int col = d.n - 1 - g;                      /* largest first */
                double lam = Cw[(size_t)col*d.n + i]*sqrt(ev[col] > 0 ? ev[col] : 1e-4);
                th0[df_il(&d, i, g)] = lam;
                resid -= lam*lam;
            }
            lsig0[i] = 0.5*log(resid > 1e-8 ? resid : 1e-8);
        }
        for(int g = 0; g < d.k; g++) th0[df_ia(&d, 0, g, g)] = 0.5;
    }
    /* ---- BFGS2 multistart on psi ---- */
    double psi[DF_MAXPAR]; double ll = -1e300;
    {
        double s0[DF_MAXPAR], s1[DF_MAXPAR], s2[DF_MAXPAR];
        df_theta_to_psi(&d, th0, s0);
        memcpy(s1, s0, sizeof s0);
        memcpy(s2, s0, sizeof s0);
        for(int j = 0; j < d.nfree; j++){ s1[j] *= 0.5; s2[j] *= 1.5; }
        double *starts[3] = { s0, s1, s2 };
        gsl_multimin_function_fdf F =
            { df_negll, df_negll_df, df_negll_fdf, (size_t)Kp, &d };
        for(int si = 0; si < 3; si++){
            gsl_multimin_fdfminimizer *mz =
                gsl_multimin_fdfminimizer_alloc(
                    gsl_multimin_fdfminimizer_vector_bfgs2, Kp);
            gsl_vector *x0 = gsl_vector_alloc(Kp);
            for(int j = 0; j < d.nfree; j++) gsl_vector_set(x0, j, starts[si][j]);
            for(int i = 0; i < d.n; i++)
                gsl_vector_set(x0, d.nfree + i, lsig0[i] + (si == 2 ? -0.5 : 0.0));
            gsl_multimin_fdfminimizer_set(mz, &F, x0, 0.1, 1e-4);
            int status = GSL_CONTINUE, it = 0;
            double f_prev = 1e300;
            while(status == GSL_CONTINUE && it < 400){
                it++;
                status = gsl_multimin_fdfminimizer_iterate(mz);
                if(status) break;
                if(fabs(f_prev - mz->f) < 1e-10*(1.0 + fabs(mz->f))) break;
                f_prev = mz->f;
                status = gsl_multimin_test_gradient(mz->gradient, 1e-6);
            }
            if(-mz->f > ll){
                ll = -mz->f;
                for(int j = 0; j < Kp; j++) psi[j] = gsl_vector_get(mz->x, j);
            }
            gsl_multimin_fdfminimizer_free(mz);
            gsl_vector_free(x0);
        }
    }
    /* theta at the optimum */
    double th[DF_MAXPAR];
    for(int i = 0; i < d.Km; i++){
        th[i] = d.tp[i];
        for(int j = 0; j < d.nfree; j++) th[i] += d.Nb[(size_t)j*d.Km + i]*psi[j];
    }
    double sig2[DF_MAXN];
    for(int i = 0; i < d.n; i++) sig2[i] = exp(2.0*psi[d.nfree + i]);
    /* ---- deterministic sign convention (unconstrained only, D2) ---- */
    if(!d.ncns){
        for(int g = 0; g < d.k; g++){
            int i0 = -1;
            for(int i = 0; i < d.n; i++)
                if(fabs(th[df_il(&d, i, g)]) > 1e-10){ i0 = i; break; }
            if(i0 >= 0 && th[df_il(&d, i0, g)] < 0){
                for(int i = 0; i < d.n; i++) th[df_il(&d, i, g)] = -th[df_il(&d, i, g)];
                for(int l = 0; l < d.p; l++) for(int h = 0; h < d.k; h++){
                    if(h != g){
                        th[df_ia(&d, l, g, h)] = -th[df_ia(&d, l, g, h)];
                        th[df_ia(&d, l, h, g)] = -th[df_ia(&d, l, h, g)];
                    }
                }
                df_theta_to_psi(&d, th, psi);   /* keep psi consistent */
            }
        }
    }
    /* ---- OIM: Hessian in psi-space, delta to [theta_mean; sig2] ---- */
    int Krep = d.Km + d.n;
    double *V = calloc((size_t)Krep*Krep, sizeof(double));
    {
        double *Hp = malloc((size_t)Kp*Kp*sizeof(double));
        double h = 1e-4;
        for(int i = 0; i < Kp; i++) for(int j = 0; j <= i; j++){
            double tpp[DF_MAXPAR], tpm[DF_MAXPAR], tmp2[DF_MAXPAR], tmm[DF_MAXPAR];
            memcpy(tpp, psi, sizeof psi); memcpy(tpm, psi, sizeof psi);
            memcpy(tmp2, psi, sizeof psi); memcpy(tmm, psi, sizeof psi);
            tpp[i] += h; tpp[j] += h;  tpm[i] += h; tpm[j] -= h;
            tmp2[i] -= h; tmp2[j] += h; tmm[i] -= h; tmm[j] -= h;
            gsl_vector_view vv2;
            double fpp, fpm, fmp, fmm;
            vv2 = gsl_vector_view_array(tpp, Kp); fpp = df_negll(&vv2.vector, &d);
            vv2 = gsl_vector_view_array(tpm, Kp); fpm = df_negll(&vv2.vector, &d);
            vv2 = gsl_vector_view_array(tmp2, Kp); fmp = df_negll(&vv2.vector, &d);
            vv2 = gsl_vector_view_array(tmm, Kp); fmm = df_negll(&vv2.vector, &d);
            double hij = (fpp - fpm - fmp + fmm)/(4*h*h);
            Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i] = hij;
        }
        int okH = (LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0)
               && (LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0);
        if(!okH){
            tea_err("dfactor: information matrix not positive definite — SEs unavailable%s\n",
                    d.k >= 2 && !d.ncns ?
                    " (an unconstrained multi-factor model is not identified;"
                    " see constraints())" : "");
        } else {
            for(int i = 0; i < Kp; i++) for(int j = i+1; j < Kp; j++)
                Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i];
            /* J: Krep x Kp.  theta_mean rows: N on psi_mean cols.
             * sig2 rows: 2*sig2_i on its own lsig col. */
            double *J = calloc((size_t)Krep*Kp, sizeof(double));
            for(int i = 0; i < d.Km; i++)
                for(int j = 0; j < d.nfree; j++)
                    J[(size_t)j*Krep + i] = d.Nb[(size_t)j*d.Km + i];
            for(int i = 0; i < d.n; i++)
                J[(size_t)(d.nfree + i)*Krep + (d.Km + i)] = 2.0*sig2[i];
            double *JH = malloc((size_t)Krep*Kp*sizeof(double));
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, Krep, Kp, Kp,
                        1.0, J, Krep, Hp, Kp, 0.0, JH, Krep);
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, Krep, Krep, Kp,
                        1.0, JH, Krep, J, Krep, 0.0, V, Krep);
            free(J); free(JH);
        }
        free(Hp);
    }
    /* ---- output ---- */
    long dsteps = 0, nsteps = 0;
    (void)nsteps; (void)dsteps;
    if(!c->quiet){
        printf("Dynamic-factor model\n");
        printf("Factors: %d, ar(%d)%s\n", d.k, d.p,
               d.ncns ? "" : (d.k >= 2 ?
               "\nNote: k >= 2 without constraints() is identified only up to"
               " rotation;\n      the log likelihood and the factor space are"
               " reproducible, the\n      particular rotation is not." : ""));
        printf("\nSample: %ld periods (%ld scalar obs)          Log likelihood = %.5f\n",
               Tn, nobs_scalar, ll);
        if(d.ncns) printf("Constraints imposed: %d (rank)\n", d.ncns);
        printf("------------------------------------------------------------------------------\n");
        printf("             | Coefficient  Std. err.      z    P>|z|     [95%% conf. interval]\n");
        double zc = gsl_cdf_ugaussian_Pinv(0.975);
        /* Stata order: one block per observation equation (loadings,
         * exog, _cons), then one block per factor equation. */
        int order[DF_MAXPAR]; int no = 0;
        for(int i = 0; i < d.n; i++){
            for(int g = 0; g < d.k; g++) order[no++] = df_il(&d, i, g);
            for(int j = 0; j < d.Kx; j++) order[no++] = df_ib(&d, i, j);
            if(d.hc) order[no++] = df_im(&d, i);
        }
        for(int g = 0; g < d.k; g++)
            for(int l = 0; l < d.p; l++)
                for(int h = 0; h < d.k; h++) order[no++] = df_ia(&d, l, g, h);
        char cureq[34] = "";
        for(int oi = 0; oi < no; oi++){
            int i = order[oi];
            char eqn[34], par[34];
            const char *br = strchr(mnames[i], ']');
            snprintf(eqn, sizeof eqn, "%.*s", (int)(br - mnames[i] - 1), mnames[i]+1);
            snprintf(par, sizeof par, "%s", br+1);
            if(strcmp(cureq, eqn)){
                printf("-------------+----------------------------------------------------------------\n");
                printf("%-12.12s |\n", eqn);
                snprintf(cureq, sizeof cureq, "%s", eqn);
            }
            double se = V[(size_t)i*Krep + i] > 0 ? sqrt(V[(size_t)i*Krep + i]) : 0;
            int constrained = 0;
            if(d.ncns && se < 1e-12){
                /* is this coefficient pinned?  (row of N all ~0) */
                double nn = 0;
                for(int j = 0; j < d.nfree; j++)
                    nn += fabs(d.Nb[(size_t)j*d.Km + i]);
                if(nn < 1e-10) constrained = 1;
            }
            if(constrained)
                printf("%12.12s | %10.6g  (constrained)\n", par, th[i]);
            else {
                double z = se > 0 ? th[i]/se : 0;
                printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
                       par, th[i], se, z,
                       se > 0 ? 2.0*(1.0 - gsl_cdf_ugaussian_P(fabs(z))) : 1.0,
                       th[i] - zc*se, th[i] + zc*se);
            }
        }
        printf("-------------+----------------------------------------------------------------\n");
        printf("%-12.12s |\n", "Variance");
        for(int i = 0; i < d.n; i++){
            double se = V[(size_t)(d.Km+i)*Krep + (d.Km+i)] > 0
                      ? sqrt(V[(size_t)(d.Km+i)*Krep + (d.Km+i)]) : 0;
            char lab[40];
            snprintf(lab, sizeof lab, "var(%s)", c->f->vars[d.yi[i]].name);
            printf("%12.12s | %10.6g  %10.6g\n", lab, sig2[i], se);
        }
        printf("------------------------------------------------------------------------------\n");
    }
    /* ---- smfactor(stub): smoothed factors ---- */
    char stub[33] = "";
    if(opt_value(c->options, "smfactor", stub, sizeof stub) && stub[0]){
        int m = d.k*d.p, n = d.n;
        double Z[DF_MAXN*DF_MAXK*DF_MAXP]; memset(Z, 0, sizeof Z);
        double T[16*16] = {0}, R[16*DF_MAXK] = {0}, Q[DF_MAXK*DF_MAXK] = {0};
        double a1[16] = {0}, Pst[16*16], Pinf[16*16] = {0}, RQR[16*16] = {0};
        double Hd[DF_MAXN];
        for(int i = 0; i < n; i++) Hd[i] = sig2[i];
        for(int i = 0; i < n; i++) for(int g = 0; g < d.k; g++)
            Z[(size_t)g*n + i] = th[df_il(&d, i, g)];
        for(int l = 0; l < d.p; l++)
            for(int g = 0; g < d.k; g++) for(int h = 0; h < d.k; h++)
                T[(size_t)(l*d.k + h)*m + g] = th[df_ia(&d, l, g, h)];
        for(int j = 0; j < m - d.k; j++) T[(size_t)j*m + (d.k + j)] = 1.0;
        for(int g = 0; g < d.k; g++){ R[(size_t)g*m + g] = 1.0; Q[(size_t)g*d.k + g] = 1.0;
                                      RQR[(size_t)g*m + g] = 1.0; }
        if(!ss_lyapunov(T, RQR, m, Pst)){
            SSModel M = { m, n, d.k, Z, Hd, T, R, Q, a1, Pst, Pinf };
            double *w = malloc((size_t)n*Tn*sizeof(double));
            for(int i = 0; i < n; i++){
                double mu = d.hc ? th[df_im(&d, i)] : 0.0;
                for(long t = 0; t < Tn; t++){
                    double v = y[(size_t)i*Tn + t];
                    if(isnan(v)){ w[(size_t)i*Tn + t] = NAN; continue; }
                    double mm = mu;
                    for(int j = 0; j < d.Kx; j++)
                        mm += th[df_ib(&d, i, j)]*X[(size_t)j*Tn + t];
                    w[(size_t)i*Tn + t] = v - mm;
                }
            }
            double *ahat = malloc((size_t)Tn*m*sizeof(double));
            ss_smooth(&M, w, Tn, ahat, NULL, NULL, NULL);
            for(int g = 0; g < d.k; g++){
                char vn[65];
                snprintf(vn, sizeof vn, "%s%d", stub, g+1);
                if(var_find(c->f, vn) >= 0){
                    tea_err("dfactor: %s already defined (smfactor skipped)\n", vn);
                    break;
                }
                Variable *nv2 = var_add(c->f, vn, VT_NUM);
                for(size_t r = 0; r < c->f->nobs; r++) nv2->num[r] = SV_MISS;
                for(long t = 0; t < Tn; t++) nv2->num[first+t] = ahat[(size_t)t*m + g];
            }
            free(ahat); free(w);
            if(!c->quiet) printf("(smoothed factors saved as %s1..%s%d)\n", stub, stub, d.k);
        }
    }
    /* ---- Estimates post: paren-free e-names ---- */
    {
        Estimates *ee = est_new();
        snprintf(ee->cmd, 16, "dfactor");
        snprintf(ee->depvar, 33, "%s", c->f->vars[d.yi[0]].name);
        ee->K = Krep;
        ee->xnames = calloc(Krep, 33);
        for(int i = 0; i < d.Km; i++){
            /* [y1]f1 -> y1_f1 ; [f1]L.f1 -> f1_L1_f1 ; [y1]_cons -> y1_cons */
            char nm[40]; int o = 0;
            for(const char *p = mnames[i]; *p && o < 32; p++){
                if(*p == '[') continue;
                if(*p == ']'){ nm[o++] = '_'; continue; }
                if(*p == '.'){ continue; }
                if(*p == 'L' && p > mnames[i] && p[-1] == ']'){ nm[o++]='L'; if(p[1]=='.'){ nm[o++]='1'; } continue; }
                if(*p == '_' && o > 0 && nm[o-1] == '_') continue;
                nm[o++] = *p;
            }
            nm[o] = 0;
            snprintf(ee->xnames[i], 33, "%s", nm);
        }
        for(int i = 0; i < d.n; i++)
            snprintf(ee->xnames[d.Km + i], 33, "var_%s", c->f->vars[d.yi[i]].name);
        ee->omitted = calloc(Krep, sizeof(int));
        ee->b = malloc((size_t)Krep*sizeof(double));
        memcpy(ee->b, th, (size_t)d.Km*sizeof(double));
        for(int i = 0; i < d.n; i++) ee->b[d.Km + i] = sig2[i];
        ee->V = malloc((size_t)Krep*Krep*sizeof(double));
        memcpy(ee->V, V, (size_t)Krep*Krep*sizeof(double));
        ee->N = nobs_scalar;
        ee->df_r = (int)(nobs_scalar - Krep);
        ee->df_m = Krep;
        ee->has_cons = d.hc;
        ee->se_kind = SE_CLASSICAL;
        ee->nobs_at_fit = c->f->nobs;
        ee->used = calloc(c->f->nobs, 1);
        for(long t = 0; t < Tn; t++){
            int any = 0;
            for(int i = 0; i < d.n; i++)
                if(!isnan(y[(size_t)i*Tn + t])) any = 1;
            if(any) ee->used[first+t] = 1;
        }
        snprintf(ee->fitted_frame, 33, "%s", c->f->name);
        est_free(c->ws->last_est);
        c->ws->last_est = ee;
        store_coef_macros(ee, &c->ip->rret);
        char bb[32];
        snprintf(bb, sizeof bb, "%.10g", ll);
        mac_set(&c->ip->rret, "e(ll)", bb);
    }
    free(V); free(mnames); free(d.tp); free(d.Nb); free(y); free(X);
    return 0;
}

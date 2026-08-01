/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * panel2.c — the panel depth tier (DESIGN_PANEL.md):
 *   areg    — absorbed fixed effects (within transform, dummy df)
 *   xtivreg — panel IV, fe (within transform + 2SLS)
 *   xtabond — Arellano-Bond difference GMM, lags(1), one/two-step
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <gsl/gsl_cdf.h>
#include "cmd.h"
#include "dataset.h"
#include "value.h"
#include "estimates.h"
#include "interp.h"
#include "linalg.h"

extern void store_coef_macros(Estimates *e, MacroKV **tbl);

static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

/* OLS b = (X'X)^-1 X'y via Cholesky; XtXinv optional.  col-major X n x K */
static int p2_ols(const double *X, const double *y, long n, int K,
                  double *b, double *XtXinv){
    double *A = malloc((size_t)K*K*sizeof(double));
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)n,
                1.0, X, (int)n, X, (int)n, 0.0, A, K);
    cblas_dgemv(CblasColMajor, CblasTrans, (int)n, K, 1.0, X, (int)n,
                y, 1, 0.0, b, 1);
    if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',K,A,K)) { free(A); return 1; }
    if(LAPACKE_dpotrs(LAPACK_COL_MAJOR,'U',K,1,A,K,b,K)) { free(A); return 1; }
    if(XtXinv){
        LAPACKE_dpotri(LAPACK_COL_MAJOR,'U',K,A,K);
        for(int i=0;i<K;i++) for(int j=0;j<K;j++)
            XtXinv[(size_t)j*K+i] = A[(size_t)(i<j?j:i)*K + (i<j?i:j)];
    }
    free(A);
    return 0;
}

/* group demeaning: g[] group codes 0..G-1 */
static void p2_demean(double *v, long n, const int *g, int G){
    double *sum = calloc(G, sizeof(double));
    long *cnt = calloc(G, sizeof(long));
    for(long i=0;i<n;i++){ sum[g[i]] += v[i]; cnt[g[i]]++; }
    for(int j=0;j<G;j++) sum[j] /= cnt[j] > 0 ? cnt[j] : 1;
    for(long i=0;i<n;i++) v[i] -= sum[g[i]];
    free(sum); free(cnt);
}

/* map a variable's values to dense group codes; returns G */
static int p2_groups(const double *vals, long n, int *g){
    double *uniq = malloc((size_t)n*sizeof(double));
    int G = 0;
    for(long i=0;i<n;i++){
        int hit = -1;
        for(int j=0;j<G;j++) if(uniq[j] == vals[i]){ hit = j; break; }
        if(hit < 0){ uniq[G] = vals[i]; hit = G++; }
        g[i] = hit;
    }
    free(uniq);
    return G;
}

/* standard coefficient table */
static void p2_table(const char *dep, char (*names)[33], const double *b,
                     const double *V, int K, int dfr, int use_t){
    printf("------------------------------------------------------------------------------\n");
    printf("%12.12s | Coefficient  Std. err.  %s    P>|%s|    [95%% conf. interval]\n",
           dep, use_t ? "    t" : "    z", use_t ? "t" : "z");
    printf("-------------+----------------------------------------------------------------\n");
    double crit = use_t ? gsl_cdf_tdist_Pinv(0.975, dfr)
                        : gsl_cdf_ugaussian_Pinv(0.975);
    for(int k=0;k<K;k++){
        double se = V[(size_t)k*K+k] > 0 ? sqrt(V[(size_t)k*K+k]) : 0;
        double z = se > 0 ? b[k]/se : 0;
        double pv = se > 0 ? (use_t ? 2.0*(1.0 - gsl_cdf_tdist_P(fabs(z), dfr))
                                    : 2.0*(1.0 - gsl_cdf_ugaussian_P(fabs(z)))) : 1.0;
        printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
               names[k], b[k], se, z, pv, b[k]-crit*se, b[k]+crit*se);
    }
    printf("------------------------------------------------------------------------------\n");
}

static void p2_post(Cmd *c, const char *cmd, const char *dep,
                    char (*names)[33], const double *b, const double *V,
                    int K, long N, int dfr, const char *used, size_t Nfull){
    Estimates *ee = est_new();
    snprintf(ee->cmd, 16, "%s", cmd);
    snprintf(ee->depvar, 33, "%s", dep);
    ee->K = K;
    ee->xnames = calloc(K, 33);
    for(int k=0;k<K;k++){
        /* posted names are dot-free so _b[] can address them
         * (table labels keep the Stata L./D. dress) */
        int o = 0;
        for(const char *p = names[k]; *p && o < 32; p++)
            if(*p != '.') ee->xnames[k][o++] = *p;
        ee->xnames[k][o] = 0;
    }
    ee->omitted = calloc(K, sizeof(int));
    ee->b = malloc((size_t)K*sizeof(double));
    memcpy(ee->b, b, (size_t)K*sizeof(double));
    ee->V = malloc((size_t)K*K*sizeof(double));
    memcpy(ee->V, V, (size_t)K*K*sizeof(double));
    ee->N = N; ee->df_r = dfr; ee->df_m = K;
    ee->has_cons = 0;
    ee->se_kind = SE_CLASSICAL;
    ee->nobs_at_fit = Nfull;
    ee->used = calloc(Nfull, 1);
    if(used) memcpy(ee->used, used, Nfull);
    snprintf(ee->fitted_frame, 33, "%s", c->f->name);
    est_free(c->ws->last_est);
    c->ws->last_est = ee;
    store_coef_macros(ee, &c->ip->rret);
}

/* ================= areg ================= */
int do_areg(Cmd *c);
int do_areg(Cmd *c)
{
    char ab[65] = "";
    if(!opt_value(c->options, "absorb", ab, sizeof ab) || !ab[0]){
        tea_err("areg: absorb(varname) required\n"); return 198;
    }
    int ai = var_find(c->f, ab);
    if(ai < 0){ tea_err("areg: absorb variable %s not found\n", ab); return 111; }
    int *vv = NULL;
    int nv = varlist_expand(c->f, c->varlist, &vv);
    if(nv < 2){ tea_err("areg: depvar + at least one regressor\n"); free(vv); return 198; }
    int yi = vv[0], K = nv - 1;
    /* sample: complete cases, honoring if/in (house idiom) */
    size_t Nfull = c->f->nobs;
    char *used = calloc(Nfull, 1);
    const char *perr;
    Node *ifn = NULL;
    if(c->ifexp[0]){
        ifn = expr_parse(c->ifexp, c->f, &perr);
        if(!ifn){ tea_err("areg: bad if: %s\n", perr); free(used); free(vv); return 198; }
    }
    EvalCtx ec = {0}; ec.f = c->f;
    long n = 0;
    for(size_t i = 0; i < Nfull; i++){
        if(c->in_lo > 0 && (long)i + 1 < c->in_lo) continue;
        if(c->in_hi > 0 && (long)i + 1 > c->in_hi) continue;
        if(ifn){ ec.i = i; ec.n = (long)i + 1; ec.N = (long)Nfull;
                 if(!expr_eval_bool(ifn, &ec)) continue; }
        bool miss = sv_is_miss(c->f->vars[ai].num[i]);
        for(int k = 0; k < nv && !miss; k++)
            if(sv_is_miss(c->f->vars[vv[k]].num[i])) miss = true;
        if(miss) continue;
        used[i] = 1; n++;
    }
    node_free(ifn);
    if(n < K + 2){ tea_err("areg: insufficient observations\n");
        free(used); free(vv); return 2000; }
    double *X = malloc((size_t)n*K*sizeof(double));
    double *y = malloc((size_t)n*sizeof(double));
    double *av = malloc((size_t)n*sizeof(double));
    int *g = malloc((size_t)n*sizeof(int));
    long r = 0;
    for(size_t i = 0; i < Nfull; i++){
        if(!used[i]) continue;
        y[r] = c->f->vars[yi].num[i];
        for(int k = 0; k < K; k++) X[(size_t)k*n + r] = c->f->vars[vv[k+1]].num[i];
        av[r] = c->f->vars[ai].num[i];
        r++;
    }
    int G = p2_groups(av, n, g);
    p2_demean(y, n, g, G);
    for(int k = 0; k < K; k++) p2_demean(X + (size_t)k*n, n, g, G);
    double *b = malloc((size_t)K*sizeof(double));
    double *XtXinv = malloc((size_t)K*K*sizeof(double));
    if(p2_ols(X, y, n, K, b, XtXinv)){
        tea_err("areg: X'X singular after absorbing %s\n", ab);
        free(X);free(y);free(av);free(g);free(b);free(XtXinv);free(used);free(vv);
        return 2000;
    }
    long dfr = n - K - G;
    double *u = malloc((size_t)n*sizeof(double));
    double ssr = 0;
    for(long i = 0; i < n; i++){
        double f = 0;
        for(int k = 0; k < K; k++) f += X[(size_t)k*n + i]*b[k];
        u[i] = y[i] - f; ssr += u[i]*u[i];
    }
    double *V = malloc((size_t)K*K*sizeof(double));
    char cl[65] = "";
    bool robust = opt_present(c->options, "robust");
    bool clustered = opt_value(c->options, "cluster", cl, sizeof cl) && cl[0];
    if(clustered || robust){
        /* sandwich: meat from scores, HC1-style with the dummy df */
        double *S = calloc((size_t)K*K, sizeof(double));
        if(clustered){
            int ci = var_find(c->f, cl);
            if(ci < 0){ tea_err("areg: cluster var %s not found\n", cl);
                free(X);free(y);free(av);free(g);free(b);free(XtXinv);free(u);free(V);free(used);free(vv);
                return 111; }
            double *cvals = malloc((size_t)n*sizeof(double));
            int *cg = malloc((size_t)n*sizeof(int));
            long r2 = 0;
            for(size_t i = 0; i < Nfull; i++) if(used[i]) cvals[r2++] = c->f->vars[ci].num[i];
            int Gc = p2_groups(cvals, n, cg);
            double *sg = calloc((size_t)Gc*K, sizeof(double));
            for(long i = 0; i < n; i++)
                for(int k = 0; k < K; k++)
                    sg[(size_t)k*Gc + cg[i]] += X[(size_t)k*n + i]*u[i];
            cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, Gc,
                        1.0, sg, Gc, sg, Gc, 0.0, S, K);
            double adj = ((double)Gc/(Gc-1))*(((double)n-1)/dfr);
            for(int i2 = 0; i2 < K*K; i2++) S[i2] *= adj;
            free(sg); free(cg); free(cvals);
        } else {
            for(long i = 0; i < n; i++)
                for(int k = 0; k < K; k++) for(int k2 = 0; k2 <= k; k2++){
                    double v2 = X[(size_t)k*n+i]*X[(size_t)k2*n+i]*u[i]*u[i];
                    S[(size_t)k2*K+k] += v2;
                    if(k != k2) S[(size_t)k*K+k2] += v2;
                }
            double adj = (double)n/dfr;
            for(int i2 = 0; i2 < K*K; i2++) S[i2] *= adj;
        }
        double *T2 = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                    1.0, XtXinv, K, S, K, 0.0, T2, K);
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                    1.0, T2, K, XtXinv, K, 0.0, V, K);
        free(T2); free(S);
    } else {
        double s2 = ssr/dfr;
        for(int i2 = 0; i2 < K*K; i2++) V[i2] = XtXinv[i2]*s2;
    }
    char (*names)[33] = calloc(K, 33);
    for(int k = 0; k < K; k++) snprintf(names[k], 33, "%s", c->f->vars[vv[k+1]].name);
    if(!c->quiet){
        printf("Linear regression, absorbing indicators        Number of obs   = %8ld\n", n);
        printf("Absorbed variable: %s                          Categories      = %8d\n", ab, G);
        printf("%s\n", clustered ? "(Std. err. adjusted for clusters)"
                     : robust ? "(Robust standard errors)" : "");
        p2_table(c->f->vars[yi].name, names, b, V, K, (int)dfr, 1);
        printf("Note: coefficients identical to including %d group indicators; df = N - K - G.\n", G);
    }
    p2_post(c, "areg", c->f->vars[yi].name, names, b, V, K, n, (int)dfr, used, Nfull);
    free(names); free(X); free(y); free(av); free(g); free(b);
    free(XtXinv); free(u); free(V); free(used); free(vv);
    return 0;
}

/* ================= xtivreg, fe ================= */
int do_xtivreg(Cmd *c);
int do_xtivreg(Cmd *c)
{
    if(!opt_present(c->options, "fe")){
        tea_err("xtivreg: only the fe estimator is implemented (specify , fe)\n");
        return 198;
    }
    if(c->f->ts_panel < 0){ tea_err("xtivreg: xtset panelvar timevar first\n"); return 459; }
    /* parse: dep [exog...] (endo... = inst...) */
    char vb[1024]; snprintf(vb, sizeof vb, "%s", c->varlist);
    char *lp = strchr(vb, '('), *rp = lp ? strchr(lp, ')') : NULL;
    if(!lp || !rp){ tea_err("xtivreg: syntax depvar [exog] (endog = instruments), fe\n"); return 198; }
    *lp = 0; *rp = 0;
    char *inside = lp + 1;
    char *eq = strchr(inside, '=');
    if(!eq){ tea_err("xtivreg: need = inside parentheses\n"); return 198; }
    *eq = 0;
    int vy_exog[64]; int n_head = 0;
    for(char *t = strtok(vb, " \t"); t; t = strtok(NULL, " \t")){
        int vi = var_find(c->f, t);
        if(vi < 0){ tea_err("xtivreg: %s not found\n", t); return 111; }
        vy_exog[n_head++] = vi;
    }
    if(n_head < 1){ tea_err("xtivreg: depvar required\n"); return 198; }
    int yi = vy_exog[0], Kx = n_head - 1;
    int endo[16], nend = 0, inst[32], nin = 0;
    for(char *t = strtok(inside, " \t"); t; t = strtok(NULL, " \t")){
        int vi = var_find(c->f, t);
        if(vi < 0){ tea_err("xtivreg: %s not found\n", t); return 111; }
        endo[nend++] = vi;
    }
    for(char *t = strtok(eq+1, " \t"); t; t = strtok(NULL, " \t")){
        int vi = var_find(c->f, t);
        if(vi < 0){ tea_err("xtivreg: %s not found\n", t); return 111; }
        inst[nin++] = vi;
    }
    if(!nend || nin < nend){ tea_err("xtivreg: need instruments >= endogenous vars\n"); return 198; }
    int K = Kx + nend;              /* regressors: exog + endo (demeaned; no cons) */
    int L = Kx + nin;               /* instruments: exog + excluded */
    /* complete cases */
    size_t Nfull = c->f->nobs;
    char *used = calloc(Nfull, 1);
    long n = 0;
    int pi = c->f->ts_panel;
    for(size_t i = 0; i < Nfull; i++){
        bool miss = sv_is_miss(c->f->vars[pi].num[i]) ||
                    sv_is_miss(c->f->vars[yi].num[i]);
        for(int k = 1; k < n_head && !miss; k++) miss = sv_is_miss(c->f->vars[vy_exog[k]].num[i]);
        for(int k = 0; k < nend && !miss; k++) miss = sv_is_miss(c->f->vars[endo[k]].num[i]);
        for(int k = 0; k < nin && !miss; k++) miss = sv_is_miss(c->f->vars[inst[k]].num[i]);
        if(miss) continue;
        used[i] = 1; n++;
    }
    double *y = malloc((size_t)n*sizeof(double));
    double *X = malloc((size_t)n*K*sizeof(double));
    double *Z = malloc((size_t)n*L*sizeof(double));
    double *pv = malloc((size_t)n*sizeof(double));
    int *g = malloc((size_t)n*sizeof(int));
    long r = 0;
    for(size_t i = 0; i < Nfull; i++){
        if(!used[i]) continue;
        y[r] = c->f->vars[yi].num[i];
        for(int k = 0; k < Kx; k++){
            X[(size_t)k*n + r] = c->f->vars[vy_exog[k+1]].num[i];
            Z[(size_t)k*n + r] = c->f->vars[vy_exog[k+1]].num[i];
        }
        for(int k = 0; k < nend; k++) X[(size_t)(Kx+k)*n + r] = c->f->vars[endo[k]].num[i];
        for(int k = 0; k < nin; k++)  Z[(size_t)(Kx+k)*n + r] = c->f->vars[inst[k]].num[i];
        pv[r] = c->f->vars[pi].num[i];
        r++;
    }
    int Ng = p2_groups(pv, n, g);
    p2_demean(y, n, g, Ng);
    for(int k = 0; k < K; k++) p2_demean(X + (size_t)k*n, n, g, Ng);
    for(int k = 0; k < L; k++) p2_demean(Z + (size_t)k*n, n, g, Ng);
    /* 2SLS: Xhat = Z (Z'Z)^-1 Z'X ; b = (Xhat'X)^-1 Xhat'y */
    double *bz = malloc((size_t)L*sizeof(double));
    double *Xhat = malloc((size_t)n*K*sizeof(double));
    for(int k = 0; k < K; k++){
        if(p2_ols(Z, X + (size_t)k*n, n, L, bz, NULL)){
            tea_err("xtivreg: Z'Z singular\n"); return 2000; }
        cblas_dgemv(CblasColMajor, CblasNoTrans, (int)n, L, 1.0, Z, (int)n,
                    bz, 1, 0.0, Xhat + (size_t)k*n, 1);
    }
    double *b = malloc((size_t)K*sizeof(double));
    double *A = malloc((size_t)K*K*sizeof(double));
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)n,
                1.0, Xhat, (int)n, X, (int)n, 0.0, A, K);
    cblas_dgemv(CblasColMajor, CblasTrans, (int)n, K, 1.0, Xhat, (int)n,
                y, 1, 0.0, b, 1);
    int *ipiv = malloc((size_t)K*sizeof(int));
    double *Ainv = malloc((size_t)K*K*sizeof(double));
    memcpy(Ainv, A, (size_t)K*K*sizeof(double));
    LAPACKE_dgesv(LAPACK_COL_MAJOR, K, 1, A, K, ipiv, b, K);
    LAPACKE_dgetrf(LAPACK_COL_MAJOR, K, K, Ainv, K, ipiv);
    LAPACKE_dgetri(LAPACK_COL_MAJOR, K, Ainv, K, ipiv);
    long dfr = n - K - Ng;
    double ssr = 0;
    double *u = malloc((size_t)n*sizeof(double));
    for(long i2 = 0; i2 < n; i2++){
        double f = 0;
        for(int k = 0; k < K; k++) f += X[(size_t)k*n + i2]*b[k];
        u[i2] = y[i2] - f; ssr += u[i2]*u[i2];
    }
    double *V = malloc((size_t)K*K*sizeof(double));
    double *XhX = malloc((size_t)K*K*sizeof(double));
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)n,
                1.0, Xhat, (int)n, Xhat, (int)n, 0.0, XhX, K);
    /* V = s2 * (Xhat'X)^-1 Xhat'Xhat (X'Xhat)^-1 = s2 * (Xhat'Xhat)^-1
     * since Xhat'X = Xhat'Xhat for 2SLS projections */
    if(opt_present(c->options, "robust")){
        /* panel-clustered sandwich (Stata's xtivreg,fe robust clusters on panel) */
        double *sg = calloc((size_t)Ng*K, sizeof(double));
        for(long i2 = 0; i2 < n; i2++)
            for(int k = 0; k < K; k++)
                sg[(size_t)k*Ng + g[i2]] += Xhat[(size_t)k*n + i2]*u[i2];
        double *S = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, Ng,
                    1.0, sg, Ng, sg, Ng, 0.0, S, K);
        double adj = ((double)Ng/(Ng-1))*(((double)n-1)/dfr);
        for(int i3 = 0; i3 < K*K; i3++) S[i3] *= adj;
        double *T2 = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                    1.0, Ainv, K, S, K, 0.0, T2, K);
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, K, K, K,
                    1.0, T2, K, Ainv, K, 0.0, V, K);
        free(T2); free(S); free(sg);
    } else {
        double s2 = ssr/dfr;
        double *T2 = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                    1.0, Ainv, K, XhX, K, 0.0, T2, K);
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, K, K, K,
                    s2, T2, K, Ainv, K, 0.0, V, K);
        free(T2);
    }
    char (*names)[33] = calloc(K, 33);
    for(int k = 0; k < Kx; k++) snprintf(names[k], 33, "%s", c->f->vars[vy_exog[k+1]].name);
    for(int k = 0; k < nend; k++) snprintf(names[Kx+k], 33, "%s", c->f->vars[endo[k]].name);
    if(!c->quiet){
        printf("Fixed-effects (within) IV regression           Number of obs   = %8ld\n", n);
        printf("Group variable: %s                             Number of groups= %8d\n",
               c->f->vars[pi].name, Ng);
        p2_table(c->f->vars[yi].name, names, b, V, K, (int)dfr, 1);
        printf("Instrumented: %s...   Instruments: within-demeaned exog + excluded\n",
               c->f->vars[endo[0]].name);
    }
    p2_post(c, "xtivreg", c->f->vars[yi].name, names, b, V, K, n, (int)dfr, used, Nfull);
    free(names); free(y); free(X); free(Z); free(pv); free(g); free(bz);
    free(Xhat); free(b); free(A); free(Ainv); free(ipiv); free(u); free(V);
    free(XhX); free(used);
    return 0;
}

/* ================= xtabond ================= */
int do_xtabond(Cmd *c);
int do_xtabond(Cmd *c)
{
    if(c->f->ts_panel < 0 || c->f->ts_time < 0){
        tea_err("xtabond: xtset panelvar timevar first\n"); return 459;
    }
    int *vv = NULL;
    int nv = varlist_expand(c->f, c->varlist, &vv);
    if(nv < 1){ tea_err("xtabond: depvar required\n"); free(vv); return 198; }
    int yi = vv[0], Kx = nv - 1;
    char lb[16] = "";
    int lags = opt_value(c->options, "lags", lb, sizeof lb) ? atoi(lb) : 1;
    if(lags != 1){ tea_err("xtabond: lags(1) only in this release\n"); free(vv); return 198; }
    char mb[16] = "";
    int maxldep = opt_value(c->options, "maxldep", mb, sizeof mb) ? atoi(mb) : 99;
    bool twostep = opt_present(c->options, "twostep");
    bool robust = opt_present(c->options, "robust");
    int pi = c->f->ts_panel;
    /* collect panel layout: assume xtset-sorted (panel, time) */
    size_t Nf = c->f->nobs;
    /* per panel: contiguous run */
    int K = 1 + Kx;                     /* L.dy + dX */
    /* First pass: build per-panel index lists */
    long *pstart = malloc((size_t)(Nf+1)*sizeof(long));
    int NP = 0;
    double curp = SV_MISS;
    for(size_t i = 0; i < Nf; i++){
        double p = c->f->vars[pi].num[i];
        if(sv_is_miss(p)) continue;
        if(NP == 0 || p != curp){ pstart[NP++] = (long)i; curp = p; }
    }
    pstart[NP] = (long)Nf;
    /* instrument count: max Ti determines columns.  GMM cols for y:
     * for diff eq at time index t (>=2 within panel, 0-based),
     * instruments y_{s}, s=0..t-2 (capped maxldep) -> column id keyed
     * by (t, s).  Plus Kx standard columns (dX self-instruments). */
    int maxT = 0;
    for(int pnl = 0; pnl < NP; pnl++){
        int Ti = (int)(pstart[pnl+1] - pstart[pnl]);
        if(Ti > maxT) maxT = Ti;
    }
    if(maxT < 3){ tea_err("xtabond: need T >= 3\n"); free(pstart); free(vv); return 2000; }
    /* enumerate GMM columns */
    int nzcol = 0;
    int *colt = malloc((size_t)maxT*maxT*sizeof(int));
    int *cols = malloc((size_t)maxT*maxT*sizeof(int));
    for(int t = 2; t < maxT; t++)
        for(int s0 = (t-1-maxldep > 0 ? t-1-maxldep : 0); s0 <= t-2; s0++){
            colt[nzcol] = t; cols[nzcol] = s0; nzcol++;
        }
    int L = nzcol + Kx;
    /* build stacked arrays: rows = usable diff observations */
    long nrow = 0;
    for(int pnl = 0; pnl < NP; pnl++)
        nrow += (pstart[pnl+1]-pstart[pnl]) >= 3 ? (pstart[pnl+1]-pstart[pnl]) - 2 : 0;
    double *DY = malloc((size_t)nrow*sizeof(double));     /* dy_t */
    double *DX = calloc((size_t)nrow*K, sizeof(double));  /* [dy_{t-1}, dx_t] */
    double *ZM = calloc((size_t)nrow*L, sizeof(double));
    int *rowg = malloc((size_t)nrow*sizeof(int));         /* panel of row */
    long rr = 0;
    int npan_used = 0;
    for(int pnl = 0; pnl < NP; pnl++){
        long a0 = pstart[pnl], a1 = pstart[pnl+1];
        int Ti = (int)(a1 - a0);
        if(Ti < 3) continue;
        int any = 0;
        for(int t = 2; t < Ti; t++){
            double yt = c->f->vars[yi].num[a0+t], y1 = c->f->vars[yi].num[a0+t-1],
                   y2v = c->f->vars[yi].num[a0+t-2];
            if(sv_is_miss(yt)||sv_is_miss(y1)||sv_is_miss(y2v)) continue;
            bool xmiss = false;
            for(int k = 0; k < Kx; k++){
                if(sv_is_miss(c->f->vars[vv[k+1]].num[a0+t]) ||
                   sv_is_miss(c->f->vars[vv[k+1]].num[a0+t-1])) { xmiss = true; break; }
            }
            if(xmiss) continue;
            DY[rr] = yt - y1;
            DX[rr] = y1 - y2v;                         /* col 0: L.dy */
            for(int k = 0; k < Kx; k++)
                DX[(size_t)(1+k)*nrow + rr] =
                    c->f->vars[vv[k+1]].num[a0+t] - c->f->vars[vv[k+1]].num[a0+t-1];
            /* GMM instrument row: y levels s=0..t-2 in the (t,s) columns */
            for(int cc = 0; cc < nzcol; cc++){
                if(colt[cc] != t) continue;
                double ys = c->f->vars[yi].num[a0+cols[cc]];
                if(!sv_is_miss(ys)) ZM[(size_t)cc*nrow + rr] = ys;
            }
            for(int k = 0; k < Kx; k++)
                ZM[(size_t)(nzcol+k)*nrow + rr] = DX[(size_t)(1+k)*nrow + rr];
            rowg[rr] = pnl;
            rr++; any = 1;
        }
        if(any) npan_used++;
    }
    nrow = rr;
    if(nrow < K + 1){ tea_err("xtabond: insufficient usable observations\n");
        free(pstart);free(colt);free(cols);free(DY);free(DX);free(ZM);free(rowg);free(vv);
        return 2000; }
    /* one-step GMM: W1 = (sum_i Z_i' H Z_i)^-1, H tridiag(2,-1).
     * Implement via rows: Z'HZ = sum over panels of Zi' H Zi.  For rows
     * of the same panel at consecutive t, H couples them.  Build A = Z'HZ. */
    double *Aw = calloc((size_t)L*L, sizeof(double));
    {
        /* per panel, rows are consecutive in rr order with t increasing */
        long i0 = 0;
        while(i0 < nrow){
            long i1 = i0;
            while(i1 < nrow && rowg[i1] == rowg[i0]) i1++;
            long Tn2 = i1 - i0;
            /* Zi' H Zi with H = 2I - offdiag(1) of size Tn2 */
            for(long a = 0; a < Tn2; a++){
                for(int l1 = 0; l1 < L; l1++){
                    double z1 = ZM[(size_t)l1*nrow + i0 + a];
                    if(z1 == 0) continue;
                    for(int l2 = 0; l2 < L; l2++){
                        double acc = 2.0*z1*ZM[(size_t)l2*nrow + i0 + a];
                        if(a > 0)     acc -= z1*ZM[(size_t)l2*nrow + i0 + a - 1];
                        if(a < Tn2-1) acc -= z1*ZM[(size_t)l2*nrow + i0 + a + 1];
                        Aw[(size_t)l2*L + l1] += acc;
                    }
                }
            }
            i0 = i1;
        }
    }
    /* invert Aw (may be singular if too many instruments: use pinv via dgelsd? keep dgesv with ridge) */
    double *W = malloc((size_t)L*L*sizeof(double));
    {
        double *Ac = malloc((size_t)L*L*sizeof(double));
        memcpy(Ac, Aw, (size_t)L*L*sizeof(double));
        for(int i3 = 0; i3 < L; i3++) Ac[(size_t)i3*L + i3] += 1e-9*(1.0 + Ac[(size_t)i3*L+i3]);
        memset(W, 0, (size_t)L*L*sizeof(double));
        for(int i3 = 0; i3 < L; i3++) W[(size_t)i3*L + i3] = 1.0;
        int *ip2 = malloc((size_t)L*sizeof(int));
        if(LAPACKE_dgesv(LAPACK_COL_MAJOR, L, L, Ac, L, ip2, W, L)){
            tea_err("xtabond: instrument weight matrix singular\n");
            free(ip2); free(Ac);
            free(pstart);free(colt);free(cols);free(DY);free(DX);free(ZM);free(rowg);free(vv);
            free(Aw); free(W);
            return 2000;
        }
        free(ip2); free(Ac);
    }
    /* b = (X'Z W Z'X)^-1 X'Z W Z'y */
    double *ZX = malloc((size_t)L*K*sizeof(double));
    double *Zy = malloc((size_t)L*sizeof(double));
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, L, K, (int)nrow,
                1.0, ZM, (int)nrow, DX, (int)nrow, 0.0, ZX, L);
    cblas_dgemv(CblasColMajor, CblasTrans, (int)nrow, L, 1.0, ZM, (int)nrow,
                DY, 1, 0.0, Zy, 1);
    double *WZX = malloc((size_t)L*K*sizeof(double));
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, L, K, L,
                1.0, W, L, ZX, L, 0.0, WZX, L);
    double *M = malloc((size_t)K*K*sizeof(double));      /* X'Z W Z'X */
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, L,
                1.0, ZX, L, WZX, L, 0.0, M, K);
    double *b = malloc((size_t)K*sizeof(double));
    cblas_dgemv(CblasColMajor, CblasTrans, L, K, 1.0, WZX, L, Zy, 1, 0.0, b, 1);
    double *Minv = malloc((size_t)K*K*sizeof(double));
    {
        memcpy(Minv, M, (size_t)K*K*sizeof(double));
        int *ip3 = malloc((size_t)K*sizeof(int));
        LAPACKE_dgesv(LAPACK_COL_MAJOR, K, 1, M, K, ip3, b, K);
        LAPACKE_dgetrf(LAPACK_COL_MAJOR, K, K, Minv, K, ip3);
        LAPACKE_dgetri(LAPACK_COL_MAJOR, K, Minv, K, ip3);
        free(ip3);
    }
    /* residuals; two-step and/or robust need Z_i' u_i per panel */
    double *u = malloc((size_t)nrow*sizeof(double));
    for(long i3 = 0; i3 < nrow; i3++){
        double f = 0;
        for(int k = 0; k < K; k++) f += DX[(size_t)k*nrow + i3]*b[k];
        u[i3] = DY[i3] - f;
    }
    double *sg = calloc((size_t)npan_used*L, sizeof(double));  /* per-panel Z'u */
    {
        long i0 = 0; int gp = 0;
        while(i0 < nrow){
            long i1 = i0;
            while(i1 < nrow && rowg[i1] == rowg[i0]) i1++;
            for(long a = i0; a < i1; a++)
                for(int l1 = 0; l1 < L; l1++)
                    sg[(size_t)l1*npan_used + gp] += ZM[(size_t)l1*nrow + a]*u[a];
            gp++; i0 = i1;
        }
    }
    double *S = malloc((size_t)L*L*sizeof(double));     /* sum Z'u u'Z */
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, L, L, npan_used,
                1.0, sg, npan_used, sg, npan_used, 0.0, S, L);
    if(twostep){
        /* W2 = S^-1; re-estimate */
        double *W2 = malloc((size_t)L*L*sizeof(double));
        double *Sc = malloc((size_t)L*L*sizeof(double));
        memcpy(Sc, S, (size_t)L*L*sizeof(double));
        for(int i3 = 0; i3 < L; i3++) Sc[(size_t)i3*L+i3] += 1e-9*(1.0 + Sc[(size_t)i3*L+i3]);
        memset(W2, 0, (size_t)L*L*sizeof(double));
        for(int i3 = 0; i3 < L; i3++) W2[(size_t)i3*L+i3] = 1.0;
        int *ip4 = malloc((size_t)L*sizeof(int));
        if(!LAPACKE_dgesv(LAPACK_COL_MAJOR, L, L, Sc, L, ip4, W2, L)){
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, L, K, L,
                        1.0, W2, L, ZX, L, 0.0, WZX, L);
            cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, L,
                        1.0, ZX, L, WZX, L, 0.0, M, K);
            cblas_dgemv(CblasColMajor, CblasTrans, L, K, 1.0, WZX, L, Zy, 1, 0.0, b, 1);
            memcpy(Minv, M, (size_t)K*K*sizeof(double));
            LAPACKE_dgesv(LAPACK_COL_MAJOR, K, 1, M, K, ip4, b, K);
            LAPACKE_dgetrf(LAPACK_COL_MAJOR, K, K, Minv, K, ip4);
            LAPACKE_dgetri(LAPACK_COL_MAJOR, K, Minv, K, ip4);
            memcpy(W, W2, (size_t)L*L*sizeof(double));
            /* refresh residual moments at the two-step b */
            for(long i3 = 0; i3 < nrow; i3++){
                double f = 0;
                for(int k = 0; k < K; k++) f += DX[(size_t)k*nrow + i3]*b[k];
                u[i3] = DY[i3] - f;
            }
            memset(sg, 0, (size_t)npan_used*L*sizeof(double));
            long i0b = 0; int gp = 0;
            while(i0b < nrow){
                long i1 = i0b;
                while(i1 < nrow && rowg[i1] == rowg[i0b]) i1++;
                for(long a = i0b; a < i1; a++)
                    for(int l1 = 0; l1 < L; l1++)
                        sg[(size_t)l1*npan_used + gp] += ZM[(size_t)l1*nrow + a]*u[a];
                gp++; i0b = i1;
            }
            cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, L, L, npan_used,
                        1.0, sg, npan_used, sg, npan_used, 0.0, S, L);
        }
        free(ip4); free(Sc); free(W2);
    }
    /* VCE: robust sandwich Minv (X'Z W S W Z'X) Minv ; one-step
     * conventional uses sigma2 * Minv with the H-weight (already the
     * AB one-step formula since W = (Z'HZ)^-1 up to sigma2). */
    double *V = malloc((size_t)K*K*sizeof(double));
    if(robust || twostep){
        double *WS = malloc((size_t)L*L*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, L, L, L,
                    1.0, W, L, S, L, 0.0, WS, L);
        double *WSW = malloc((size_t)L*L*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, L, L, L,
                    1.0, WS, L, W, L, 0.0, WSW, L);
        double *ZXW = malloc((size_t)L*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, L, K, L,
                    1.0, WSW, L, ZX, L, 0.0, ZXW, L);
        double *mid = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, L,
                    1.0, ZX, L, ZXW, L, 0.0, mid, K);
        double *T2 = malloc((size_t)K*K*sizeof(double));
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                    1.0, Minv, K, mid, K, 0.0, T2, K);
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, K, K, K,
                    1.0, T2, K, Minv, K, 0.0, V, K);
        free(WS); free(WSW); free(ZXW); free(mid); free(T2);
    } else {
        double ssr = 0;
        for(long i3 = 0; i3 < nrow; i3++) ssr += u[i3]*u[i3];
        double s2 = ssr/(2.0*(nrow - K));   /* var of eps from var(du)=2 s2 */
        for(int i3 = 0; i3 < K*K; i3++) V[i3] = 2.0*s2*Minv[i3];
    }
    /* Sargan J at the current weight */
    double sargan = 0;
    {
        double *Zu = calloc((size_t)L, sizeof(double));
        for(int l1 = 0; l1 < L; l1++)
            for(int gp = 0; gp < npan_used; gp++) Zu[l1] += sg[(size_t)l1*npan_used + gp];
        double *WZu = malloc((size_t)L*sizeof(double));
        cblas_dgemv(CblasColMajor, CblasNoTrans, L, L, 1.0, W, L, Zu, 1, 0.0, WZu, 1);
        for(int l1 = 0; l1 < L; l1++) sargan += Zu[l1]*WZu[l1];
        if(!twostep){
            double ssr2 = 0;
            for(long i3 = 0; i3 < nrow; i3++) ssr2 += u[i3]*u[i3];
            double s2 = ssr2/(2.0*(nrow - K));
            sargan /= s2;
        }
        free(Zu); free(WZu);
    }
    int jdf = L - K;
    char (*names)[33] = calloc(K, 33);
    snprintf(names[0], 33, "L.%s", c->f->vars[yi].name);
    for(int k = 0; k < Kx; k++)
        snprintf(names[1+k], 33, "D.%s", c->f->vars[vv[k+1]].name);
    if(!c->quiet){
        printf("Arellano-Bond dynamic panel-data estimation    Number of obs   = %8ld\n", nrow);
        printf("Group variable: %s                             Number of groups= %8d\n",
               c->f->vars[pi].name, npan_used);
        printf("Number of instruments = %d                     %s GMM\n", L,
               twostep ? "Two-step" : "One-step");
        if(twostep && !robust)
            printf("Note: two-step SEs without Windmeijer correction (staged) are downward biased.\n");
        p2_table(c->f->vars[yi].name, names, b, V, K, (int)(nrow-K), 0);
        printf("Sargan test of overidentifying restrictions: chi2(%d) = %.4f   Prob > chi2 = %.4f\n",
               jdf, sargan, jdf > 0 ? 1.0 - gsl_cdf_chisq_P(sargan, jdf) : 1.0);
    }
    p2_post(c, "xtabond", c->f->vars[yi].name, names, b, V, K, nrow,
            (int)(nrow-K), NULL, Nf);
    {
        char bb[32];
        snprintf(bb, sizeof bb, "%.10g", sargan);
        mac_set(&c->ip->rret, "e(sargan)", bb);
    }
    free(names); free(pstart); free(colt); free(cols); free(DY); free(DX);
    free(ZM); free(rowg); free(Aw); free(W); free(ZX); free(Zy); free(WZX);
    free(M); free(b); free(Minv); free(u); free(sg); free(S); free(V); free(vv);
    return 0;
}

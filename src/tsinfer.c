/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The macro time-series inference tier (DESIGN_TSINFER.md):
 * newey, dfuller, pperron, tsfilter (hp/bk/hamilton), var, vargranger,
 * irf (in-memory subset), lpirf, vecrank.
 *
 * Numerical notes:
 *  - HAC meat and scalar long-run variance live in src/vce.c.
 *  - Unit-root critical values: MacKinnon (2010) response surfaces.
 *  - Unit-root p-values: probit-space interpolation through the
 *    finite-T 1/5/10%% response-surface quantiles (documented
 *    approximation; COMPATIBILITY.md).
 *  - vecrank critical values: Osterwald-Lenum (1992), 5%%.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gsl/gsl_cdf.h>
#include "cmd.h"
#include "dataset.h"
#include "vce.h"
#include "linalg.h"
#include "tsop.h"
#include "estimates.h"

#include "value.h"
#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdarg.h>
static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

/* ---------- small dense OLS via Cholesky (X full rank by construction) - */
/* X: N x K col-major.  Returns 0; fills b (K), resid (N), and if XtXinv
 * non-NULL the K x K inverse of X'X. */
static int ts_ols(const double *X, const double *y, long N, int K,
                  double *b, double *resid, double *XtXinv){
    double *A = calloc((size_t)K*K, sizeof(double));
    double *g = calloc((size_t)K,   sizeof(double));
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)N,
                1.0, X, (int)N, X, (int)N, 0.0, A, K);
    cblas_dgemv(CblasColMajor, CblasTrans, (int)N, K, 1.0, X, (int)N,
                y, 1, 0.0, g, 1);
    if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',K,A,K) != 0){ free(A); free(g); return 1; }
    memcpy(b, g, (size_t)K*sizeof(double));
    LAPACKE_dpotrs(LAPACK_COL_MAJOR,'U',K,1,A,K,b,K);
    if(XtXinv){
        LAPACKE_dpotri(LAPACK_COL_MAJOR,'U',K,A,K);
        for(int i=0;i<K;i++) for(int j=0;j<K;j++)
            XtXinv[(size_t)i*K+j] = A[(size_t)(i<j?j:i)*K + (i<j?i:j)];
    }
    if(resid){
        memcpy(resid, y, (size_t)N*sizeof(double));
        cblas_dgemv(CblasColMajor, CblasNoTrans, (int)N, K, -1.0, X, (int)N,
                    b, 1, 1.0, resid, 1);
    }
    free(A); free(g);
    return 0;
}

/* ---------- tsset guard: numeric time, no panel, contiguous ---------- */
/* Builds the time-ordered row map of nonmissing rows of the given vars.
 * Returns count or -1 with message.  rows must hold f->nobs entries. */
static long ts_sample(Frame *f, const int *vidx, int nv, size_t *rows){
    if(f->ts_time < 0){ tea_err("time-series command: data not tsset\n"); return -1; }
    if(f->ts_panel >= 0){ tea_err("time-series command: panel data not supported here (use the panel tier)\n"); return -1; }
    /* frame is kept sorted by tsset; walk in order, require contiguity */
    Variable *tv = &f->vars[f->ts_time];
    long n = 0; double prev = 0; int have_prev = 0;
    for(size_t r = 0; r < f->nobs; r++){
        int ok = !sv_is_miss(tv->num[r]);
        for(int j = 0; ok && j < nv; j++)
            if(sv_is_miss(f->vars[vidx[j]].num[r])) ok = 0;
        if(!ok){
            if(have_prev){ tea_err("time-series command: sample contains gaps (missing values inside the range)\n"); return -1; }
            continue;
        }
        if(have_prev && tv->num[r] != prev + f->ts_delta){
            tea_err("time-series command: time variable has gaps\n"); return -1;
        }
        prev = tv->num[r]; have_prev = 1;
        rows[n++] = r;
    }
    return n;
}

/* ================= newey ============================================= */
int do_newey(Cmd *c);
int do_newey(Cmd *c){
    char lagbuf[16]="";
    if(!opt_value(c->options,"lag",lagbuf,sizeof lagbuf) || !lagbuf[0]){
        tea_err("newey: lag(#) required\n"); return 198;
    }
    int L = atoi(lagbuf);
    if(L < 0){ tea_err("newey: lag(#) must be >= 0\n"); return 198; }
    int *vs=NULL, nv, n_temps=0; const char *vlerr=NULL;
    nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    if(nv < 2){ tea_err("newey: depvar and regressors required\n"); free(vs); return 111; }
    size_t *rows = malloc(c->f->nobs * sizeof(size_t));
    long N = ts_sample(c->f, vs, nv, rows);
    if(N < 0){ free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 111; }
    int K = nv;                       /* regressors + constant */
    if(N <= K){ tea_err("newey: insufficient observations\n");
        free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 2000; }
    double *X = malloc((size_t)N*K*sizeof(double));
    double *y = malloc((size_t)N*sizeof(double));
    for(long t=0;t<N;t++){
        y[t] = c->f->vars[vs[0]].num[rows[t]];
        for(int j=1;j<nv;j++) X[(size_t)(j-1)*N + t] = c->f->vars[vs[j]].num[rows[t]];
        X[(size_t)(K-1)*N + t] = 1.0;
    }
    double *b = calloc(K,sizeof(double)), *e = malloc((size_t)N*sizeof(double));
    double *Binv = calloc((size_t)K*K,sizeof(double));
    if(ts_ols(X,y,N,K,b,e,Binv)){ tea_err("newey: X'X not positive definite (collinearity?)\n");
        free(X);free(y);free(b);free(e);free(Binv);free(vs);free(rows);
        tsop_drop_temps(c->f,n_temps); return 2000; }
    double *S = malloc((size_t)N*K*sizeof(double));
    for(int j=0;j<K;j++) for(long t=0;t<N;t++)
        S[(size_t)j*N+t] = e[t]*X[(size_t)j*N+t];
    double *V = vce_hac(Binv,S,N,K,L, vce_adj_ls_robust(N,K));
    /* joint F on slopes via V */
    double F = 0.0, Fp = 1.0; int q = K-1;
    if(q > 0){
        double *Vss = malloc((size_t)q*q*sizeof(double));
        for(int i=0;i<q;i++) for(int j=0;j<q;j++) Vss[(size_t)i*q+j]=V[(size_t)i*K+j];
        double *bb = malloc((size_t)q*sizeof(double));
        memcpy(bb,b,(size_t)q*sizeof(double));
        if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',q,Vss,q)==0){
            LAPACKE_dpotrs(LAPACK_COL_MAJOR,'U',q,1,Vss,q,bb,q);
            double W=0; for(int i=0;i<q;i++) W += b[i]*bb[i];
            F = W/q;
            Fp = 1.0 - gsl_cdf_fdist_P(F,q,(double)(N-K));
        }
        free(Vss); free(bb);
    }
    printf("Regression with Newey-West standard errors      Number of obs  = %10ld\n", N);
    printf("Maximum lag = %-4d                              F(%3d,%7ld)  = %10.2f\n",
           L, q, N-(long)K, F);
    printf("%48s Prob > F       = %10.4f\n","",Fp);
    printf("------------------------------------------------------------------------------\n");
    printf("             |             Newey-West\n");
    printf("%12s | Coefficient  std. err.      t    P>|t|     [95%% conf. interval]\n",
           c->f->vars[vs[0]].name);
    printf("-------------+----------------------------------------------------------------\n");
    double tcrit = gsl_cdf_tdist_Pinv(0.975,(double)(N-K));
    for(int j=0;j<K;j++){
        const char *nm = j<K-1 ? c->f->vars[vs[j+1]].name : "_cons";
        double se = sqrt(V[(size_t)j*K+j]);
        double tt = b[j]/se;
        double p  = 2.0*(1.0 - gsl_cdf_tdist_P(fabs(tt),(double)(N-K)));
        printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
               nm, b[j], se, tt, p, b[j]-tcrit*se, b[j]+tcrit*se);
    }
    printf("------------------------------------------------------------------------------\n");
    {
        Estimates *ee = est_new();
        snprintf(ee->cmd,16,"newey");
        snprintf(ee->depvar,33,"%s",c->f->vars[vs[0]].name);
        ee->K = K;
        ee->xnames = calloc(K, 33);
        for(int j=0;j<K-1;j++) snprintf(ee->xnames[j],33,"%s",c->f->vars[vs[j+1]].name);
        snprintf(ee->xnames[K-1],33,"_cons");
        ee->omitted = calloc(K,sizeof(int));
        ee->b = malloc((size_t)K*sizeof(double)); memcpy(ee->b,b,(size_t)K*sizeof(double));
        ee->V = malloc((size_t)K*K*sizeof(double));
        for(int i=0;i<K;i++) for(int j=0;j<K;j++)
            ee->V[(size_t)i*K+j] = V[(size_t)j*K+i];
        ee->N = N; ee->df_r = (int)(N-K); ee->df_m = q; ee->has_cons = 1;
        ee->F = F; ee->F_p = Fp; ee->se_kind = SE_ROBUST;
        ee->nobs_at_fit = c->f->nobs;
        ee->used = calloc(c->f->nobs,1);
        for(long t=0;t<N;t++) ee->used[rows[t]] = 1;
        snprintf(ee->fitted_frame,33,"%s",c->f->name);
        est_free(c->ws->last_est);
        c->ws->last_est = ee;
    }
    free(X);free(y);free(b);free(e);free(Binv);free(S);free(V);
    free(vs); free(rows); tsop_drop_temps(c->f,n_temps);
    return 0;
}

/* ============ MacKinnon (2010) response-surface critical values ======= */
/* variant: 0=noconstant, 1=constant, 2=trend.  Level: 0=1%%,1=5%%,2=10%%. */
static double mackinnon_cv(int variant, int level, long T){
    static const double b[3][3][4] = {
        { /* nc */
          {-2.56574, -2.2358,  -3.627,    0.0   },
          {-1.94100, -0.2686,  -3.365,   31.223 },
          {-1.61682,  0.2656,  -2.714,   25.364 } },
        { /* c */
          {-3.43035, -6.5393, -16.786,  -79.433 },
          {-2.86154, -2.8903,  -4.234,  -40.040 },
          {-2.56677, -1.5384,  -2.809,    0.0   } },
        { /* ct */
          {-3.95877, -9.0531, -28.428, -134.155 },
          {-3.41049, -4.3904,  -9.036,  -45.374 },
          {-3.12705, -2.5856,  -3.925,  -22.380 } },
    };
    const double *q = b[variant][level];
    double Ti = 1.0/(double)T;
    return q[0] + q[1]*Ti + q[2]*Ti*Ti + q[3]*Ti*Ti*Ti;
}
/* p-value: linear interpolation in probit space through the three
 * finite-T quantiles (approximation; DESIGN_TSINFER.md). */
static double mackinnon_p(int variant, long T, double tau){
    double q1  = mackinnon_cv(variant,0,T);   /* p=.01 */
    double q5  = mackinnon_cv(variant,1,T);   /* p=.05 */
    double q10 = mackinnon_cv(variant,2,T);   /* p=.10 */
    double z1  = gsl_cdf_ugaussian_Pinv(0.01);
    double z5  = gsl_cdf_ugaussian_Pinv(0.05);
    double z10 = gsl_cdf_ugaussian_Pinv(0.10);
    double z;
    if(tau <= q5)  z = z5  + (tau - q5) * (z5 - z1 ) / (q5 - q1 );
    else           z = z5  + (tau - q5) * (z10 - z5) / (q10 - q5);
    double p = gsl_cdf_ugaussian_P(z);
    if(p < 0.0) p = 0.0;
    if(p > 1.0) p = 1.0;
    return p;
}

/* shared DF-style regression builder.  aug = number of lagged
 * differences; trend/cons flags; returns T (rows used) and outputs. */
static long df_regression(Frame *f, int yi, int aug, int cons, int trend,
                          double *tau_out, double *rho_out, double *se_rho_out,
                          double **resid_out, long *K_out, int print_reg){
    size_t *rows = malloc(f->nobs * sizeof(size_t));
    int one = yi;
    long n = ts_sample(f, &one, 1, rows);
    if(n < 0){ free(rows); return -1; }
    long T = n - 1 - aug;             /* usable Delta-y rows */
    int K = 1 + aug + (cons?1:0) + (trend?1:0);
    if(T <= K + 1){ tea_err("unit-root test: insufficient observations\n"); free(rows); return -1; }
    double *X = malloc((size_t)T*K*sizeof(double));
    double *dy = malloc((size_t)T*sizeof(double));
    Variable *yv = &f->vars[yi];
    for(long t = 0; t < T; t++){
        long i = t + 1 + aug;         /* index into rows[] */
        dy[t] = yv->num[rows[i]] - yv->num[rows[i-1]];
        int col = 0;
        X[(size_t)col*T + t] = yv->num[rows[i-1]]; col++;      /* y_{t-1} */
        for(int l = 1; l <= aug; l++){
            X[(size_t)col*T + t] =
                yv->num[rows[i-l]] - yv->num[rows[i-l-1]]; col++;
        }
        if(trend){ X[(size_t)col*T + t] = (double)(t+1); col++; }
        if(cons){  X[(size_t)col*T + t] = 1.0; col++; }
    }
    double *b = calloc(K,sizeof(double)), *e = malloc((size_t)T*sizeof(double));
    double *Binv = calloc((size_t)K*K,sizeof(double));
    if(ts_ols(X,dy,T,K,b,e,Binv)){ tea_err("unit-root test: singular regression\n");
        free(rows);free(X);free(dy);free(b);free(e);free(Binv); return -1; }
    double s2 = 0; for(long t=0;t<T;t++) s2 += e[t]*e[t];
    s2 /= (T - K);
    double se0 = sqrt(s2 * Binv[0]);
    *tau_out = b[0] / se0;
    if(rho_out)    *rho_out = b[0];
    if(se_rho_out) *se_rho_out = se0;
    if(print_reg){
        printf("\n------------------------------------------------------------------------------\n");
        printf("        D.%s | Coefficient  Std. err.      t    P>|t|     [95%% conf. interval]\n",
               f->vars[yi].name);
        printf("-------------+----------------------------------------------------------------\n");
        double tcrit = gsl_cdf_tdist_Pinv(0.975,(double)(T-K));
        for(int j=0;j<K;j++){
            char nm[40];
            int col=0;
            if(j==0) snprintf(nm,sizeof nm,"%s L1", f->vars[yi].name);
            else{
                col=j;
                if(col <= aug) snprintf(nm,sizeof nm,"LD%d", col);
                else if(trend && col == aug+1) snprintf(nm,sizeof nm,"_trend");
                else snprintf(nm,sizeof nm,"_cons");
            }
            double se = sqrt(s2 * Binv[(size_t)j*K+j]);
            double tt = b[j]/se;
            double p  = 2.0*(1.0 - gsl_cdf_tdist_P(fabs(tt),(double)(T-K)));
            printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
                   nm, b[j], se, tt, p, b[j]-tcrit*se, b[j]+tcrit*se);
        }
        printf("------------------------------------------------------------------------------\n");
    }
    if(resid_out){ *resid_out = e; } else free(e);
    if(K_out) *K_out = K;
    free(rows); free(X); free(dy); free(b); free(Binv);
    return T;
}

/* ================= dfuller =========================================== */
int do_dfuller(Cmd *c);
int do_dfuller(Cmd *c){
    int *vv=NULL, nvv, ntp=0; const char *ve=NULL;
    nvv = tsop_expand_varlist(c->f, c->varlist, &vv, &ntp, &ve);
    if(nvv != 1){ tea_err("dfuller: one numeric variable required\n"); free(vv); return 111; }
    int yi = vv[0]; free(vv);
    char v[65]; snprintf(v,sizeof v,"%s",c->f->vars[yi].name);
    if(c->f->vars[yi].type!=VT_NUM){ tea_err("dfuller: numeric variable required\n");
        tsop_drop_temps(c->f,ntp); return 111; }
    char lb[16]=""; int aug = opt_value(c->options,"lags",lb,sizeof lb) ? atoi(lb) : 0;
    int trend = opt_present(c->options,"trend");
    int nocons = opt_present(c->options,"noconstant");
    int drift = opt_present(c->options,"drift");
    if(trend && nocons){ tea_err("dfuller: trend requires a constant\n"); return 198; }
    int cons = !nocons;
    int variant = nocons ? 0 : (trend ? 2 : 1);
    double tau; long T;
    T = df_regression(c->f, yi, aug, cons, trend, &tau, NULL, NULL, NULL, NULL,
                      opt_present(c->options,"regress"));
    if(T < 0) return 111;
    printf("\n%s test for unit root\n",
           aug>0 ? "Augmented Dickey-Fuller" : "Dickey-Fuller");
    printf("Variable: %-12s                    Number of obs  = %9ld\n", v, T);
    printf("%-42s Number of lags = %9d\n",
           trend?"Constant and trend":(nocons?"No constant or trend":"Constant only"), aug);
    printf("\n                  Test         1%% critical    5%% critical   10%% critical\n");
    printf("                 statistic         value          value          value\n");
    printf("------------------------------------------------------------------------------\n");
    if(drift){
        /* drift: the same regression, but Z(t) is t-distributed */
        long dfr = T - 1 - aug - 1;
        double p = gsl_cdf_tdist_P(tau, (double)dfr);   /* one-sided lower tail */
        printf(" Z(t)          %9.3f          (Z(t) has t(%ld) distribution under drift)\n",
               tau, dfr);
        printf("------------------------------------------------------------------------------\n");
        printf("p-value (one-sided) = %6.4f\n", p);
    } else {
        printf(" Z(t)          %9.3f       %9.3f      %9.3f      %9.3f\n",
               tau,
               mackinnon_cv(variant,0,T),
               mackinnon_cv(variant,1,T),
               mackinnon_cv(variant,2,T));
        printf("------------------------------------------------------------------------------\n");
        printf("MacKinnon approximate p-value for Z(t) = %6.4f\n", mackinnon_p(variant,T,tau));
    }
    tsop_drop_temps(c->f,ntp);
    return 0;
}

/* ================= pperron =========================================== */
int do_pperron(Cmd *c);
int do_pperron(Cmd *c){
    int *vv=NULL, nvv, ntp=0; const char *ve=NULL;
    nvv = tsop_expand_varlist(c->f, c->varlist, &vv, &ntp, &ve);
    if(nvv != 1){ tea_err("pperron: one numeric variable required\n"); free(vv); return 111; }
    int yi = vv[0]; free(vv);
    char v[65]; snprintf(v,sizeof v,"%s",c->f->vars[yi].name);
    if(c->f->vars[yi].type!=VT_NUM){ tea_err("pperron: numeric variable required\n");
        tsop_drop_temps(c->f,ntp); return 111; }
    int trend = opt_present(c->options,"trend");
    int nocons = opt_present(c->options,"noconstant");
    int cons = !nocons;
    int variant = nocons ? 0 : (trend ? 2 : 1);
    double tau, rho, se_rho; double *e=NULL; long K;
    long T = df_regression(c->f, yi, 0, cons, trend, &tau, &rho, &se_rho, &e, &K,
                           opt_present(c->options,"regress"));
    if(T < 0) return 111;
    char lb[16]="";
    int L = opt_value(c->options,"lags",lb,sizeof lb) ? atoi(lb)
            : (int)floor(4.0*pow((double)T/100.0, 2.0/9.0));
    double g0 = 0; for(long t=0;t<T;t++) g0 += e[t]*e[t];
    double s2 = g0/(T-K);
    g0 /= T;
    double lrv = vce_lrvar(e,T,L);
    /* Z(t) = tau*sqrt(g0/lrv) - (lrv-g0)*T*se_rho/(2*lrv*sqrt(s2*T... )
     * standard PP correction: */
    double Zt = tau*sqrt(g0/lrv) - (lrv - g0)*(double)T*se_rho/(2.0*sqrt(lrv)*sqrt(s2));
    double Zrho = (double)T*rho - ((double)T*(double)T*se_rho*se_rho/s2)*(lrv-g0)/2.0;
    free(e);
    printf("\nPhillips-Perron test for unit root\n");
    printf("Variable: %-12s                    Number of obs  = %9ld\n", v, T);
    printf("%-42s Newey-West lags = %8d\n",
           trend?"Constant and trend":(nocons?"No constant or trend":"Constant only"), L);
    printf("\n                  Test         1%% critical    5%% critical   10%% critical\n");
    printf("                 statistic         value          value          value\n");
    printf("------------------------------------------------------------------------------\n");
    printf(" Z(rho)        %9.3f\n", Zrho);
    printf(" Z(t)          %9.3f       %9.3f      %9.3f      %9.3f\n",
           Zt, mackinnon_cv(variant,0,T), mackinnon_cv(variant,1,T), mackinnon_cv(variant,2,T));
    printf("------------------------------------------------------------------------------\n");
    printf("MacKinnon approximate p-value for Z(t) = %6.4f\n", mackinnon_p(variant,T,Zt));
    tsop_drop_temps(c->f,ntp);
    return 0;
}

/* ================= tsfilter ========================================== */
static double ts_default_lambda(const Frame *f){
    if(strstr(f->ts_fmt,"tq")) return 1600.0;
    if(strstr(f->ts_fmt,"tm")) return 129600.0;
    if(strstr(f->ts_fmt,"ty") || strstr(f->ts_fmt,"y")) return 100.0;
    return 1600.0;
}
static int ts_freq_scale(const Frame *f){   /* periods per year-ish */
    if(strstr(f->ts_fmt,"tq")) return 4;
    if(strstr(f->ts_fmt,"tm")) return 12;
    return 1;
}
int do_tsfilter(Cmd *c);
int do_tsfilter(Cmd *c){
    /* syntax: tsfilter METHOD new = var [, options] */
    char method[16]="", newv[65]="", eq[8]="", oldv[65]="";
    if(sscanf(c->args, "%15s %64s %7s %64s", method, newv, eq, oldv) < 4
       || strcmp(eq,"=") != 0){
        /* allow "new=var" without spaces */
        char rest[200]="";
        if(sscanf(c->args, "%15s %199s", method, rest)==2 && strchr(rest,'=')){
            char *p = strchr(rest,'=');
            *p = 0;
            snprintf(newv,sizeof newv,"%s",rest);
            snprintf(oldv,sizeof oldv,"%s",p+1);
            /* strip trailing option comma from oldv */
            char *q = strchr(oldv,','); if(q) *q=0;
        } else {
            tea_err("tsfilter: syntax is  tsfilter hp|bk|hamilton NEW = VAR [, options]\n");
            return 198;
        }
    }
    { char *q = strchr(oldv,','); if(q) *q=0; }
    int yi = var_find(c->f, oldv);
    if(yi<0 || c->f->vars[yi].type!=VT_NUM){ tea_err("tsfilter: variable %s not found\n", oldv); return 111; }
    if(var_find(c->f,newv) >= 0){ tea_err("tsfilter: %s already defined\n", newv); return 110; }
    size_t *rows = malloc(c->f->nobs*sizeof(size_t));
    long T = ts_sample(c->f, &yi, 1, rows);
    if(T < 0){ free(rows); return 111; }
    double *y = malloc((size_t)T*sizeof(double));
    for(long t=0;t<T;t++) y[t] = c->f->vars[yi].num[rows[t]];
    double *cyc = malloc((size_t)T*sizeof(double));
    for(long t=0;t<T;t++) cyc[t] = SV_MISS;
    double *tr = NULL;

    if(!strcmp(method,"hp")){
        char sb[32]="";
        double lam = opt_value(c->options,"smooth",sb,sizeof sb) ? atof(sb)
                     : ts_default_lambda(c->f);
        /* (I + lam D''D) tau = y ; D''D is pentadiagonal.  Banded solve. */
        /* build the full pentadiagonal explicitly to avoid pattern mistakes */
        double *P = calloc((size_t)T*T, sizeof(double));
        for(long i=0;i+2<T;i++){
            /* row i of D (second difference operator): +1 -2 +1 at i,i+1,i+2 */
            long a=i,b2=i+1,c2=i+2;
            double w[3]={1,-2,1}; long idx[3]={a,b2,c2};
            for(int p1=0;p1<3;p1++) for(int p2=0;p2<3;p2++)
                P[(size_t)idx[p1]*T+idx[p2]] += w[p1]*w[p2];
        }
        for(long i=0;i<(long)T;i++){
            for(long j=0;j<T;j++) P[(size_t)j*T+i] *= lam;
            P[(size_t)i*T+i] += 1.0;
        }
        /* banded copy for dgbsv */
        int kl=2, ku=2; int ldab2 = 2*kl+ku+1;
        double *ABn = calloc((size_t)ldab2*T, sizeof(double));
        for(long j=0;j<T;j++)
            for(long i= j-ku<0?0:j-ku; i<= (j+kl>=T?T-1:j+kl); i++)
                ABn[(size_t)j*ldab2 + (kl+ku+i-j)] = P[(size_t)j*T+i];
        tr = malloc((size_t)T*sizeof(double));
        memcpy(tr,y,(size_t)T*sizeof(double));
        int *ipiv = malloc((size_t)T*sizeof(int));
        int info = LAPACKE_dgbsv(LAPACK_COL_MAJOR,(int)T,kl,ku,1,ABn,ldab2,ipiv,tr,(int)T);
        free(P); free(ABn); free(ipiv);
        if(info!=0){ tea_err("tsfilter hp: solve failed\n");
            free(rows);free(y);free(cyc);free(tr); return 2000; }
        for(long t=0;t<T;t++) cyc[t] = y[t]-tr[t];
        printf("(hp filter: lambda = %g, %ld obs)\n", lam, T);
    } else if(!strcmp(method,"bk")){
        char b1[16]="",b2[16]="",b3[16]="";
        int scale = ts_freq_scale(c->f);
        double pl = opt_value(c->options,"minperiod",b1,sizeof b1)?atof(b1):1.5*scale;
        double pu = opt_value(c->options,"maxperiod",b2,sizeof b2)?atof(b2):8.0*scale;
        int Kk = opt_value(c->options,"k",b3,sizeof b3)?atoi(b3):3*scale;
        double wl = 2.0*M_PI/pu, wu = 2.0*M_PI/pl;
        double *w = malloc((size_t)(Kk+1)*sizeof(double));
        w[0] = (wu-wl)/M_PI;
        double sum = w[0];
        for(int j=1;j<=Kk;j++){ w[j]=(sin(wu*j)-sin(wl*j))/(M_PI*j); sum += 2*w[j]; }
        double theta = -sum/(2*Kk+1);
        for(long t=Kk;t<T-Kk;t++){
            double s = (w[0]+theta)*y[t];
            for(int j=1;j<=Kk;j++) s += (w[j]+theta)*(y[t-j]+y[t+j]);
            cyc[t]=s;
        }
        free(w);
        printf("(bk filter: periods %g-%g, K = %d, %ld obs)\n", pl, pu, Kk, T);
    } else if(!strcmp(method,"hamilton")){
        char b1[16]="",b2[16]="";
        int scale = ts_freq_scale(c->f);
        int h = opt_value(c->options,"h",b1,sizeof b1)?atoi(b1):2*scale;
        int p = opt_value(c->options,"p",b2,sizeof b2)?atoi(b2):scale;
        if(scale==4){ h=opt_value(c->options,"h",b1,sizeof b1)?atoi(b1):8;
                      p=opt_value(c->options,"p",b2,sizeof b2)?atoi(b2):4; }
        if(scale==12){ h=opt_value(c->options,"h",b1,sizeof b1)?atoi(b1):24;
                       p=opt_value(c->options,"p",b2,sizeof b2)?atoi(b2):12; }
        long Tn = T - h - (p-1);
        if(Tn < p+2){ tea_err("tsfilter hamilton: insufficient observations\n");
            free(rows);free(y);free(cyc); return 2000; }
        int K = p+1;
        double *X = malloc((size_t)Tn*K*sizeof(double));
        double *yy = malloc((size_t)Tn*sizeof(double));
        for(long t=0;t<Tn;t++){
            long base = t + (p-1);
            yy[t] = y[base+h];
            for(int j=0;j<p;j++) X[(size_t)j*Tn+t] = y[base-j];
            X[(size_t)(K-1)*Tn+t] = 1.0;
        }
        double *b=calloc(K,sizeof(double)), *e=malloc((size_t)Tn*sizeof(double));
        if(ts_ols(X,yy,Tn,K,b,e,NULL)){ tea_err("tsfilter hamilton: singular\n");
            free(X);free(yy);free(b);free(e);free(rows);free(y);free(cyc); return 2000; }
        for(long t=0;t<Tn;t++) cyc[t+(p-1)+h] = e[t];
        free(X);free(yy);free(b);free(e);
        printf("(hamilton filter: h = %d, p = %d, %ld obs)\n", h, p, Tn);
    } else {
        tea_err("tsfilter: method must be hp, bk, or hamilton\n");
        free(rows); free(y); free(cyc); free(tr); return 198;
    }
    Variable *nv = var_add(c->f, newv, VT_NUM);
    for(size_t r=0;r<c->f->nobs;r++) nv->num[r]=SV_MISS;
    for(long t=0;t<T;t++) nv->num[rows[t]] = cyc[t];
    char trname[65]="";
    if(tr && opt_value(c->options,"trend",trname,sizeof trname) && trname[0]){
        if(var_find(c->f,trname)>=0){ tea_err("tsfilter: %s already defined\n",trname); }
        else {
            Variable *tv2 = var_add(c->f, trname, VT_NUM);
            for(size_t r=0;r<c->f->nobs;r++) tv2->num[r]=SV_MISS;
            for(long t=0;t<T;t++) tv2->num[rows[t]] = tr[t];
        }
    }
    free(tr); free(rows); free(y); free(cyc);
    return 0;
}

/* ================= var / vargranger / irf ============================ */
#define VAR_MAXM 8
#define VAR_MAXP 8
static struct {
    int    valid;
    int    m, p;                       /* variables, lags */
    char   names[VAR_MAXM][33];
    double A[VAR_MAXP][VAR_MAXM*VAR_MAXM];  /* A[l][i + m*j]: eq i, var j at lag l+1 */
    double cons_[VAR_MAXM];
    double Sigma[VAR_MAXM*VAR_MAXM];   /* ML (divisor T) */
    long   T;
    /* per-equation classical VCE over the stacked coef vector (for granger) */
    int    Keq;                        /* m*p + 1 */
    double *beq;                       /* m x Keq */
    double *Veq;                       /* m x Keq x Keq */
    /* irf storage */
    int    steps;
    double *irf;                       /* (steps+1) x m x m simple */
    double *oirf;                      /* (steps+1) x m x m orthogonalized */
} g_var = {0};

int do_var(Cmd *c);
int do_var(Cmd *c){
    int *vs=NULL, nv, n_temps=0; const char *vlerr=NULL;
    nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    if(nv < 2 || nv > VAR_MAXM){ tea_err("var: 2..%d variables required\n", VAR_MAXM);
        free(vs); return 111; }
    int p = 2;
    char lb[16]="";
    if(opt_value(c->options,"lags",lb,sizeof lb) && lb[0]){
        const char *sl = strchr(lb,'/');
        p = atoi(sl? sl+1 : lb);
    }
    if(p < 1 || p > VAR_MAXP){ tea_err("var: lags out of range\n"); free(vs); return 198; }
    size_t *rows = malloc(c->f->nobs*sizeof(size_t));
    long n = ts_sample(c->f, vs, nv, rows);
    if(n < 0){ free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 111; }
    long T = n - p;
    int m = nv, K = m*p + 1;
    if(T <= K+1){ tea_err("var: insufficient observations\n");
        free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 2000; }
    double *X = malloc((size_t)T*K*sizeof(double));
    for(long t=0;t<T;t++){
        int col=0;
        for(int l=1;l<=p;l++)
            for(int j=0;j<m;j++)
                X[(size_t)(col++)*T + t] = c->f->vars[vs[j]].num[rows[t+p-l]];
        X[(size_t)(K-1)*T + t] = 1.0;
    }
    free(g_var.beq); free(g_var.Veq); free(g_var.irf); free(g_var.oirf);
    memset(&g_var, 0, sizeof g_var);
    g_var.m=m; g_var.p=p; g_var.T=T; g_var.Keq=K;
    for(int j=0;j<m;j++) snprintf(g_var.names[j],33,"%s",c->f->vars[vs[j]].name);
    g_var.beq = calloc((size_t)m*K, sizeof(double));
    g_var.Veq = calloc((size_t)m*K*K, sizeof(double));
    double *E = malloc((size_t)T*m*sizeof(double));
    double *Binv = calloc((size_t)K*K,sizeof(double));
    double loglik = 0; (void)loglik;
    double *r2 = calloc(m,sizeof(double)), *rmse = calloc(m,sizeof(double));
    for(int i=0;i<m;i++){
        double *y = malloc((size_t)T*sizeof(double));
        for(long t=0;t<T;t++) y[t] = c->f->vars[vs[i]].num[rows[t+p]];
        double *b = g_var.beq + (size_t)i*K;
        double *e = E + (size_t)i*T;
        if(ts_ols(X,y,T,K,b,e,Binv)){ tea_err("var: singular system\n");
            free(y); free(X); free(E); free(Binv); free(vs); free(rows);
            free(r2); free(rmse); tsop_drop_temps(c->f,n_temps); return 2000; }
        double rss=0, tss=0, mean=0;
        for(long t=0;t<T;t++) mean += y[t]; mean/=T;
        for(long t=0;t<T;t++){ rss+=e[t]*e[t]; tss+=(y[t]-mean)*(y[t]-mean); }
        r2[i] = tss>0? 1.0-rss/tss : 0;
        rmse[i] = sqrt(rss/(T-K));
        double s2 = rss/(T-K);
        double *V = g_var.Veq + (size_t)i*K*K;
        for(int a=0;a<K;a++) for(int b2=0;b2<K;b2++)
            V[(size_t)a*K+b2] = s2*Binv[(size_t)a*K+b2];
        for(int l=0;l<p;l++) for(int j=0;j<m;j++)
            g_var.A[l][i + m*j] = b[l*m + j];
        g_var.cons_[i] = b[K-1];
        free(y);
    }
    /* Sigma (ML) */
    for(int i=0;i<m;i++) for(int j=0;j<m;j++){
        double s=0; for(long t=0;t<T;t++) s += E[(size_t)i*T+t]*E[(size_t)j*T+t];
        g_var.Sigma[i + m*j] = s/T;
    }
    /* log likelihood, information criteria */
    {
        double *Sg = malloc((size_t)m*m*sizeof(double));
        memcpy(Sg, g_var.Sigma, (size_t)m*m*sizeof(double));
        double ldet=0;
        if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',m,Sg,m)==0)
            for(int i=0;i<m;i++) ldet += 2.0*log(Sg[(size_t)i*m+i]);
        double ll = -0.5*T*(m*log(2*M_PI) + m) - 0.5*T*ldet;
        double aic = (-2.0*ll + 2.0*(double)m*K)/T;
        double hq  = (-2.0*ll + 2.0*log(log((double)T))*(double)m*K)/T;
        double sb  = (-2.0*ll + log((double)T)*(double)m*K)/T;
        printf("Vector autoregression\n\n");
        printf("Sample: %ld obs                                 Number of obs = %8ld\n", T, T);
        printf("Log likelihood = %10.4f                     AIC           = %10.5f\n", ll, aic);
        printf("                                                HQIC          = %10.5f\n", hq);
        printf("                                                SBIC          = %10.5f\n", sb);
        free(Sg);
    }
    printf("\nEquation           Parms      RMSE     R-sq\n");
    printf("--------------------------------------------\n");
    for(int i=0;i<m;i++)
        printf("%-18s %5d %9.4g %8.4f\n", g_var.names[i], K, rmse[i], r2[i]);
    printf("--------------------------------------------\n");
    double zc = gsl_cdf_ugaussian_Pinv(0.975);
    for(int i=0;i<m;i++){
        printf("\n------------------------------------------------------------------------------\n");
        printf("%12.12s | Coefficient  Std. err.      z    P>|z|     [95%% conf. interval]\n",
               g_var.names[i]);
        printf("-------------+----------------------------------------------------------------\n");
        double *b = g_var.beq + (size_t)i*K;
        double *V = g_var.Veq + (size_t)i*K*K;
        for(int col=0;col<K;col++){
            char nm[48];
            if(col==K-1) snprintf(nm,sizeof nm,"_cons");
            else snprintf(nm,sizeof nm,"%s L%d", g_var.names[col%m], col/m + 1);
            double se = sqrt(V[(size_t)col*K+col]);
            double z = b[col]/se;
            double pv = 2.0*(1.0-gsl_cdf_ugaussian_P(fabs(z)));
            printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
                   nm, b[col], se, z, pv, b[col]-zc*se, b[col]+zc*se);
        }
    }
    printf("------------------------------------------------------------------------------\n");
    g_var.valid = 1;
    free(X); free(E); free(Binv); free(vs); free(rows); free(r2); free(rmse);
    tsop_drop_temps(c->f,n_temps);
    return 0;
}

int do_vargranger(Cmd *c);
int do_vargranger(Cmd *c){
    (void)c;
    if(!g_var.valid){ tea_err("vargranger: run var first\n"); return 301; }
    int m=g_var.m, p=g_var.p, K=g_var.Keq;
    printf("Granger causality Wald tests\n");
    printf("------------------------------------------------------------------\n");
    printf("  Equation           Excluded             chi2     df  Prob > chi2\n");
    printf("------------------------------------------------------------------\n");
    for(int i=0;i<m;i++){
        double *b = g_var.beq + (size_t)i*K;
        double *V = g_var.Veq + (size_t)i*K*K;
        double all_chi2=0; int all_df=0;
        /* per excluded variable j != i: coefficients at cols l*m+j, l=0..p-1 */
        for(int j=0;j<m;j++){
            if(j==i) continue;
            int q=p;
            double *Vs=malloc((size_t)q*q*sizeof(double));
            double *bs=malloc((size_t)q*sizeof(double));
            for(int a=0;a<q;a++){
                bs[a]=b[a*m+j];
                for(int b2=0;b2<q;b2++)
                    Vs[(size_t)a*q+b2]=V[(size_t)(a*m+j)*K + (b2*m+j)];
            }
            double W=0;
            if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',q,Vs,q)==0){
                double *x=malloc((size_t)q*sizeof(double));
                memcpy(x,bs,(size_t)q*sizeof(double));
                LAPACKE_dpotrs(LAPACK_COL_MAJOR,'U',q,1,Vs,q,x,q);
                for(int a=0;a<q;a++) W+=bs[a]*x[a];
                free(x);
            }
            printf("  %-18s %-18s %8.3f  %5d  %10.3f\n",
                   g_var.names[i], g_var.names[j], W, q,
                   1.0-gsl_cdf_chisq_P(W,q));
            all_chi2 += W; all_df += p;   /* block-diagonal approx not exact; do joint properly */
            free(Vs); free(bs);
        }
        /* joint ALL: all lags of all other variables */
        {
            int q=(m-1)*p;
            int *idx=malloc((size_t)q*sizeof(int)); int w=0;
            for(int l=0;l<p;l++) for(int j=0;j<m;j++) if(j!=i) idx[w++]=l*m+j;
            double *Vs=malloc((size_t)q*q*sizeof(double));
            double *bs=malloc((size_t)q*sizeof(double));
            for(int a=0;a<q;a++){ bs[a]=b[idx[a]];
                for(int b2=0;b2<q;b2++) Vs[(size_t)a*q+b2]=V[(size_t)idx[a]*K+idx[b2]]; }
            double W=0;
            if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',q,Vs,q)==0){
                double *x=malloc((size_t)q*sizeof(double));
                memcpy(x,bs,(size_t)q*sizeof(double));
                LAPACKE_dpotrs(LAPACK_COL_MAJOR,'U',q,1,Vs,q,x,q);
                for(int a=0;a<q;a++) W+=bs[a]*x[a];
                free(x);
            }
            printf("  %-18s %-18s %8.3f  %5d  %10.3f\n",
                   g_var.names[i], "ALL", W, q, 1.0-gsl_cdf_chisq_P(W,q));
            free(Vs); free(bs); free(idx);
        }
    }
    printf("------------------------------------------------------------------\n");
    return 0;
}

int do_irf(Cmd *c);
int do_irf(Cmd *c){
    char sub[16]=""; sscanf(c->args,"%15[a-z]",sub);
    if(!strcmp(sub,"create")){
        if(!g_var.valid){ tea_err("irf create: run var first\n"); return 301; }
        char sb[16]="";
        int steps = opt_value(c->options,"step",sb,sizeof sb)? atoi(sb) : 8;
        if(steps<1 || steps>64){ tea_err("irf: step(#) out of range\n"); return 198; }
        int m=g_var.m, p=g_var.p;
        free(g_var.irf); free(g_var.oirf);
        g_var.irf  = calloc((size_t)(steps+1)*m*m, sizeof(double));
        g_var.oirf = calloc((size_t)(steps+1)*m*m, sizeof(double));
        g_var.steps = steps;
        /* Phi_0 = I; Phi_h = sum_{l=1..min(h,p)} A_l Phi_{h-l} */
        double *Phi = g_var.irf;
        for(int i=0;i<m;i++) Phi[(size_t)0*m*m + i + m*i] = 1.0;
        for(int h=1;h<=steps;h++){
            double *Ph = Phi + (size_t)h*m*m;
            for(int l=1;l<=p && l<=h;l++){
                const double *Al = g_var.A[l-1];
                const double *Pl = Phi + (size_t)(h-l)*m*m;
                cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,m,m,m,
                            1.0,Al,m,Pl,m,1.0,Ph,m);
            }
        }
        /* Cholesky lower of Sigma; oirf_h = Phi_h * P */
        double *P = malloc((size_t)m*m*sizeof(double));
        memcpy(P,g_var.Sigma,(size_t)m*m*sizeof(double));
        if(LAPACKE_dpotrf(LAPACK_COL_MAJOR,'L',m,P,m)!=0){
            tea_err("irf: Sigma not positive definite\n"); return 2000; }
        for(int i=0;i<m;i++) for(int j=i+1;j<m;j++) P[(size_t)j*m+i]=0.0;
        for(int h=0;h<=steps;h++)
            cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,m,m,m,
                        1.0,g_var.irf+(size_t)h*m*m,m,P,m,0.0,
                        g_var.oirf+(size_t)h*m*m,m);
        free(P);
        printf("(irfs computed: %d steps, %d variables, Cholesky order as given)\n",
               steps, m);
        return 0;
    }
    if(!strcmp(sub,"table")){
        if(!g_var.irf){ tea_err("irf table: run irf create first\n"); return 301; }
        char kind[16]="oirf"; sscanf(c->args,"%*s %15s",kind);
        const double *R = strcmp(kind,"irf")? g_var.oirf : g_var.irf;
        int m=g_var.m;
        for(int imp=0;imp<m;imp++){
            printf("\nResponses to %s impulse (%s)\n", g_var.names[imp],
                   strcmp(kind,"irf")? "orthogonalized":"simple");
            printf("  step");
            for(int r=0;r<m;r++) printf(" %12.12s", g_var.names[r]);
            printf("\n");
            for(int h=0;h<=g_var.steps;h++){
                printf("  %4d", h);
                for(int r=0;r<m;r++)
                    printf(" %12.6g", R[(size_t)h*m*m + r + m*imp]);
                printf("\n");
            }
        }
        return 0;
    }
    tea_err("irf: subcommands are  irf create[, step(#)]  |  irf table [irf|oirf]\n");
    return 198;
}

/* ================= lpirf ============================================= */
int do_lpirf(Cmd *c);
int do_lpirf(Cmd *c){
    char v[65]=""; sscanf(c->varlist,"%64s",v);
    int yi = var_find(c->f, v);
    if(yi<0 || c->f->vars[yi].type!=VT_NUM){ tea_err("lpirf: numeric variable required\n"); return 111; }
    char sb[16]="", lb[16]="";
    int H = opt_value(c->options,"step",sb,sizeof sb)? atoi(sb) : 8;
    int p = opt_value(c->options,"lags",lb,sizeof lb)? atoi(lb) : 4;
    if(H<1||H>64||p<0||p>24){ tea_err("lpirf: step/lags out of range\n"); return 198; }
    size_t *rows = malloc(c->f->nobs*sizeof(size_t));
    long n = ts_sample(c->f,&yi,1,rows);
    if(n<0){ free(rows); return 111; }
    printf("Local-projection impulse responses of %s to its own shock\n", v);
    printf("(Jorda 2005; Newey-West SEs, lag = horizon)\n\n");
    printf("  step   coefficient    std. err.        z     [95%% conf. interval]\n");
    printf("---------------------------------------------------------------------\n");
    Variable *yv=&c->f->vars[yi];
    double zc = gsl_cdf_ugaussian_Pinv(0.975);
    for(int h=1;h<=H;h++){
        long T = n - h - p;
        int K = 1 + p + 1;
        if(T <= K+1){ tea_err("lpirf: insufficient observations at step %d\n",h); break; }
        double *X=malloc((size_t)T*K*sizeof(double));
        double *yy=malloc((size_t)T*sizeof(double));
        for(long t=0;t<T;t++){
            long base=t+p;
            yy[t]=yv->num[rows[base+h]];
            X[(size_t)0*T+t]=yv->num[rows[base]];
            for(int l=1;l<=p;l++) X[(size_t)l*T+t]=yv->num[rows[base-l]];
            X[(size_t)(K-1)*T+t]=1.0;
        }
        double *b=calloc(K,sizeof(double)), *e=malloc((size_t)T*sizeof(double));
        double *Binv=calloc((size_t)K*K,sizeof(double));
        if(ts_ols(X,yy,T,K,b,e,Binv)){ tea_err("lpirf: singular at step %d\n",h);
            free(X);free(yy);free(b);free(e);free(Binv); break; }
        double *S=malloc((size_t)T*K*sizeof(double));
        for(int j=0;j<K;j++) for(long t=0;t<T;t++) S[(size_t)j*T+t]=e[t]*X[(size_t)j*T+t];
        double *V=vce_hac(Binv,S,T,K,h,vce_adj_ls_robust(T,K));
        double se=sqrt(V[0]);
        printf("  %4d   %11.6g  %11.6g  %7.2f    %10.6g  %10.6g\n",
               h, b[0], se, b[0]/se, b[0]-zc*se, b[0]+zc*se);
        free(X);free(yy);free(b);free(e);free(Binv);free(S);free(V);
    }
    printf("---------------------------------------------------------------------\n");
    free(rows);
    return 0;
}

/* ================= vecrank =========================================== */
int do_vecrank(Cmd *c);
int do_vecrank(Cmd *c){
    int *vs=NULL, nv, n_temps=0; const char *vlerr=NULL;
    nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    if(nv<2 || nv>6){ tea_err("vecrank: 2..6 variables required\n"); free(vs); return 111; }
    char lb[16]="";
    int p = opt_value(c->options,"lags",lb,sizeof lb)? atoi(lb) : 2;   /* VAR lags */
    if(p<1||p>8){ tea_err("vecrank: lags out of range\n"); free(vs); return 198; }
    size_t *rows=malloc(c->f->nobs*sizeof(size_t));
    long n=ts_sample(c->f,vs,nv,rows);
    if(n<0){ free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 111; }
    int m=nv;
    long T=n-p;
    int Kz = m*(p-1)+1;                 /* lagged differences + constant */
    if(T <= m*p+2){ tea_err("vecrank: insufficient observations\n");
        free(vs); free(rows); tsop_drop_temps(c->f,n_temps); return 2000; }
    /* Z0 = D.y_t (T x m); Z1 = y_{t-1} (T x m); Z2 = [D.y lags, 1] (T x Kz) */
    double *Z0=malloc((size_t)T*m*sizeof(double));
    double *Z1=malloc((size_t)T*m*sizeof(double));
    double *Z2=malloc((size_t)T*Kz*sizeof(double));
    for(long t=0;t<T;t++){
        for(int j=0;j<m;j++){
            double yt = c->f->vars[vs[j]].num[rows[t+p]];
            double y1 = c->f->vars[vs[j]].num[rows[t+p-1]];
            Z0[(size_t)j*T+t]=yt-y1;
            Z1[(size_t)j*T+t]=y1;
        }
        int col=0;
        for(int l=1;l<p;l++)
            for(int j=0;j<m;j++){
                double a=c->f->vars[vs[j]].num[rows[t+p-l]];
                double b=c->f->vars[vs[j]].num[rows[t+p-l-1]];
                Z2[(size_t)(col++)*T+t]=a-b;
            }
        Z2[(size_t)(Kz-1)*T+t]=1.0;
    }
    /* residuals R0, R1 of Z0, Z1 on Z2 */
    double *R0=malloc((size_t)T*m*sizeof(double));
    double *R1=malloc((size_t)T*m*sizeof(double));
    {
        double *b=calloc(Kz,sizeof(double));
        for(int j=0;j<m;j++){
            ts_ols(Z2, Z0+(size_t)j*T, T, Kz, b, R0+(size_t)j*T, NULL);
            ts_ols(Z2, Z1+(size_t)j*T, T, Kz, b, R1+(size_t)j*T, NULL);
        }
        free(b);
    }
    /* Sij = Ri'Rj / T */
    double S00[36],S01[36],S11[36];
    cblas_dgemm(CblasColMajor,CblasTrans,CblasNoTrans,m,m,(int)T,1.0/T,R0,(int)T,R0,(int)T,0.0,S00,m);
    cblas_dgemm(CblasColMajor,CblasTrans,CblasNoTrans,m,m,(int)T,1.0/T,R0,(int)T,R1,(int)T,0.0,S01,m);
    cblas_dgemm(CblasColMajor,CblasTrans,CblasNoTrans,m,m,(int)T,1.0/T,R1,(int)T,R1,(int)T,0.0,S11,m);
    /* eigenvalues of S11^-1 S10 S00^-1 S01 via generalized symmetric problem:
       (S10 S00^-1 S01) v = lambda S11 v  — dsygv */
    double A[36], B[36], C1[36];
    memcpy(B,S11,(size_t)m*m*sizeof(double));
    /* C1 = S00^-1 S01  (solve S00 * C1 = S01) */
    memcpy(A,S00,(size_t)m*m*sizeof(double));
    memcpy(C1,S01,(size_t)m*m*sizeof(double));
    LAPACKE_dposv(LAPACK_COL_MAJOR,'U',m,m,A,m,C1,m);
    /* A = S10 * C1 = S01' * C1 */
    cblas_dgemm(CblasColMajor,CblasTrans,CblasNoTrans,m,m,m,1.0,S01,m,C1,m,0.0,A,m);
    double lam[6];
    if(LAPACKE_dsygv(LAPACK_COL_MAJOR,1,'N','U',m,A,m,B,m,lam)!=0){
        tea_err("vecrank: eigenproblem failed\n");
        free(Z0);free(Z1);free(Z2);free(R0);free(R1);free(vs);free(rows);
        tsop_drop_temps(c->f,n_temps); return 2000; }
    /* descending */
    for(int i=0;i<m/2;i++){ double t2=lam[i]; lam[i]=lam[m-1-i]; lam[m-1-i]=t2; }
    /* Osterwald-Lenum (1992) 5% trace CVs, unrestricted constant */
    static const double ol5[6] = {3.76, 15.41, 29.68, 47.21, 68.52, 94.15};
    printf("Johansen tests for cointegration\n");
    printf("Trend: constant                                 Number of obs = %8ld\n", T);
    printf("Sample: %ld obs                                 Number of lags = %7d\n", T, p);
    printf("\n---------------------------------------------------------------\n");
    printf(" Maximum                                 Trace       5%% critical\n");
    printf("   rank     Parms    Eigenvalue      statistic          value\n");
    printf("---------------------------------------------------------------\n");
    int sel = -1;
    for(int r=0;r<=m;r++){
        double tr=0;
        for(int i=r;i<m;i++) tr += log(1.0-lam[i]);
        tr *= -(double)T;
        int parms = m*(m*(p-1)+1) + r*(2*m-r);
        char ev[16];
        if(r==0) snprintf(ev,sizeof ev,"%10s","-");
        else     snprintf(ev,sizeof ev,"%10.5f",lam[r-1]);
        if(r<m){
            double cv = ol5[m-r-1];
            int star = (sel<0 && tr < cv);
            if(star) sel=r;
            printf("   %4d  %8d  %s   %12.4f%s  %12.2f\n",
                   r, parms, ev, tr, star? "*":" ", cv);
        } else {
            if(sel<0) sel=m;
            printf("   %4d  %8d  %s\n", r, parms, ev);
        }
    }
    printf("---------------------------------------------------------------\n");
    printf("* selected rank (trace test, 5%% level): %d\n", sel);
    free(Z0);free(Z1);free(Z2);free(R0);free(R1);free(vs);free(rows);
    tsop_drop_temps(c->f,n_temps);
    return 0;
}

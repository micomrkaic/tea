#define _GNU_SOURCE
/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * regress, predict, test, lincom.
 *
 * Design:
 *   - All estimators populate the same Estimates struct on the workspace
 *     so postestimation (test/predict/lincom) is uniform.
 *   - build_design() factors out y/X/weight/listwise-deletion construction
 *     so xtreg (which after within-transformation does an OLS on the same
 *     shape of inputs) can reuse it later.
 *   - OLS solve uses LAPACKE_dgels (QR-based least squares).  Rank
 *     deficiency is detected by the singularity of the resulting R; we
 *     fall back to dgelsd (SVD with column-pivoting equivalent) to drop
 *     collinear columns and mark them omitted, Stata-style.
 *   - Robust SE: HC1 sandwich (X'X)^-1 X' diag(e^2) X (X'X)^-1 with
 *     n/(n-k) finite-sample adjustment.
 *   - Cluster SE: CR1 sandwich with (G/(G-1))*((n-1)/(n-k)) adjustment.
 *
 * The variance is stored as the full K×K matrix V; omitted-row/column
 * entries are zero, leaving SE(b)=0 (Stata prints those as "(omitted)").
 */
#include "dataset.h"
#include "value.h"
#include "expr.h"
#include "cmd.h"
#include "tsop.h"
#include "interp.h"
#include "estimates.h"
#include "linalg.h"
#include "stats.h"
#include "mle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <ctype.h>

/* ---- build the design matrix ------------------------------------------- */
typedef struct {
    double *y;          /* N */
    double *X;          /* N*K column-major (LAPACK convention) */
    double *w;          /* N or NULL.  For aweight, already renormalized
                         * so sum(w)==N (Stata convention).  Otherwise raw. */
    long    N, K;
    int     wtype;      /* 0 none, 1 fw, 2 aw, 3 pw, 4 iw */
    char  (*xnames)[33];
    int     has_cons;
    size_t  nobs_full;
    char   *used;       /* nobs_full bytes: 1 if row was kept */
} Design;

static void design_free(Design *d){
    if(!d) return;
    free(d->y); free(d->X); free(d->w); free(d->xnames); free(d->used);
}

/* Save `_b[name]` and `_se[name]` macros for every coefficient in the
 * supplied estimates.  Stata exposes these alongside the e() macros so
 * users can write things like `gen yhat = _b[_cons] + _b[x]*x` or
 * `display _b[x] / _se[x]`.
 *
 * Macro names follow Stata exactly: `_b[varname]` and `_se[varname]`,
 * including the brackets in the name.  The macro_expand machinery
 * recognises this pattern out of double-quoted strings.  Omitted
 * coefficients are stored too (with the value 0 / SE 0). */
void store_coef_macros(Estimates *e, MacroKV **tbl)
{
    char key[80], val[32];
    for(int j = 0; j < e->K; j++){
        snprintf(key, sizeof key, "_b[%s]", e->xnames[j]);
        if(e->omitted[j]) snprintf(val, sizeof val, "0");
        else              snprintf(val, sizeof val, "%.10g", e->b[j]);
        mac_set(tbl, key, val);

        snprintf(key, sizeof key, "_se[%s]", e->xnames[j]);
        if(e->omitted[j]) snprintf(val, sizeof val, "0");
        else {
            double v = e->V[(size_t)j*e->K + j];
            double se = v > 0 ? sqrt(v) : 0;
            snprintf(val, sizeof val, "%.10g", se);
        }
        mac_set(tbl, key, val);
    }
}

/* build_design: depvar + regressors (already resolved to variable indices
 * by the caller — typically via tsop_expand_varlist for TS-op support),
 * with if/in/weight; performs Stata-style listwise deletion (any missing
 * in y or any X drops the row).  For aweight and pweight, weights are
 * renormalized so sum(w)==N (Stata convention). */
static int build_design(Frame *f,
                        int yi, const int *xi, int nx,
                        const char *ifexp, long in_lo, long in_hi,
                        const char *wexp, int wtype,
                        bool add_cons, Design *out, const char **err)
{
    *err = NULL;

    if(f->vars[yi].type != VT_NUM){
        static char buf[128]; snprintf(buf, sizeof buf,
            "%s is a string variable (regress requires numeric)",
            f->vars[yi].name);
        *err = buf; return 1;
    }
    for(int j=0;j<nx;j++){
        if(f->vars[xi[j]].type != VT_NUM){
            static char buf[128]; snprintf(buf, sizeof buf,
                "%s is a string variable (use encode to make it numeric, then i.%s)",
                f->vars[xi[j]].name, f->vars[xi[j]].name);
            *err = buf; return 1;
        }
    }

    const char *perr;
    Node *ifn=NULL, *wn=NULL;
    if(ifexp && ifexp[0]){ ifn=expr_parse(ifexp, f, &perr);
        if(!ifn){ *err=perr; return 1; } }
    if(wexp && wexp[0]){ wn=expr_parse(wexp, f, &perr);
        if(!wn){ *err=perr; node_free(ifn); return 1; } }

    int K = nx + (add_cons?1:0);

    /* first pass: figure out N (listwise deletion on y and any X) */
    size_t Nfull = f->nobs;
    char *used = calloc(Nfull, 1);
    long N = 0;
    EvalCtx ec={0}; ec.f=f;
    for(size_t i=0;i<Nfull;i++){
        if(in_lo>0 && (long)i+1 < in_lo) continue;
        if(in_hi>0 && (long)i+1 > in_hi) continue;
        if(ifn){ ec.i=i; ec.n=(long)i+1; ec.N=(long)Nfull; if(!expr_eval_bool(ifn,&ec)) continue; }
        double yy = f->vars[yi].num[i];
        if(sv_is_miss(yy)) continue;
        bool any_miss = false;
        for(int j=0;j<nx;j++){
            if(sv_is_miss(f->vars[xi[j]].num[i])){ any_miss=true; break; }
        }
        if(any_miss) continue;
        if(wn){ ec.i=i; EVal wv=expr_eval(wn,&ec); double w=wv.is_str?SV_MISS:wv.num; eval_free(&wv);
            if(sv_is_miss(w)) continue;
            if(w == 0.0) continue;
            if(w < 0.0){ *err="weight may not be negative";
                node_free(ifn); node_free(wn); free(used);
                tsidx_free(ec.tsidx); accs_free(&ec.accs); return 1; } }
        used[i]=1; N++;
    }
    if(N <= K){ *err="too few observations after listwise deletion";
        node_free(ifn); node_free(wn); free(used);
        tsidx_free(ec.tsidx); accs_free(&ec.accs); return 1; }

    /* allocate */
    double *y  = malloc(N * sizeof(double));
    double *X  = malloc(N * K * sizeof(double));   /* column-major */
    double *w  = wn? malloc(N*sizeof(double)) : NULL;
    char  (*xnames)[33] = malloc(K * sizeof(*xnames));

    for(int j=0;j<nx;j++) snprintf(xnames[j], 33, "%s", f->vars[xi[j]].name);
    if(add_cons) snprintf(xnames[K-1], 33, "_cons");

    /* second pass: fill arrays */
    long row=0;
    for(size_t i=0;i<Nfull;i++){
        if(!used[i]) continue;
        y[row] = f->vars[yi].num[i];
        for(int j=0;j<nx;j++) X[(size_t)j*N + row] = f->vars[xi[j]].num[i];
        if(add_cons) X[(size_t)(K-1)*N + row] = 1.0;
        if(w){ ec.i=i; EVal wv=expr_eval(wn,&ec); w[row]=wv.is_str?1.0:wv.num; eval_free(&wv); }
        row++;
    }

    /* For aweight and pweight, renormalize weights so they sum to N
     * (Stata convention).  Without this, the variance-of-residuals formula
     * needs ad-hoc rescaling; with renormalization, the standard
     * sum(weighted_resid²)/(N-K) formula gives the right answer. */
    if(w && (wtype == 2 || wtype == 3)){
        double sw = 0.0;
        for(long i=0;i<N;i++) sw += w[i];
        if(sw > 0){
            double scale = (double)N / sw;
            for(long i=0;i<N;i++) w[i] *= scale;
        }
    }

    out->y=y; out->X=X; out->w=w; out->N=N; out->K=K;
    out->wtype = wn ? wtype : 0;
    out->xnames=xnames; out->has_cons=add_cons;
    out->nobs_full=Nfull; out->used=used;

    node_free(ifn); node_free(wn);
    tsidx_free(ec.tsidx); accs_free(&ec.accs);
    return 0;
}

/* ---- helpers ----------------------------------------------------------- */
/* matrix product C = A' B  with A:m×p, B:m×q  -> C:p×q  (column-major) */
static void at_b(const double *A, long m, long p,
                 const double *B, long q, double *C){
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans,
                p, q, m, 1.0, A, m, B, m, 0.0, C, p);
}
static void mat_inv_sym(double *A, int n){
    /* Cholesky inverse for symmetric positive definite n×n column-major. */
    LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', n, A, n);
    LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', n, A, n);
    /* symmetrize */
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) A[(size_t)i*n+j] = A[(size_t)j*n+i];
}

/* ---- OLS solve with collinearity dropping ------------------------------ */
/* Returns: K-vector beta in b (zeros at omitted indices), N-vector resid,
 * K-flag array omitted[], and effective rank r in *rank.  Uses dgelsd. */
static int ols_solve_drop(double *X, double *y, long N, long K,
                          double *b, double *resid, int *omitted, int *rank){
    /* dgelsd solves min ||y-Xb||^2 returning the min-norm solution and rank.
     * We use it to detect rank deficiency, then re-solve dropping collinear
     * columns to match Stata's "omitted" reporting. */
    long lN=N, lK=K, nrhs=1;
    double *Xc = malloc((size_t)N*K*sizeof(double));
    double *yc = malloc((size_t)(N>K?N:K)*sizeof(double));
    memcpy(Xc, X, (size_t)N*K*sizeof(double));
    memcpy(yc, y, (size_t)N*sizeof(double));
    double rcond = 1e-12; int eff_rank=0;
    double *S = malloc(K*sizeof(double));
    int info = LAPACKE_dgelsd(LAPACK_COL_MAJOR, lN, lK, nrhs, Xc, lN, yc, lN>lK?lN:lK,
                              S, rcond, &eff_rank);
    free(Xc); free(yc); free(S);
    *rank = eff_rank;
    for(int j=0;j<K;j++) omitted[j]=0;
    if(info != 0) return info;

    /* If full rank: redo a clean QR solve for stability and exact residuals. */
    if(eff_rank == K){
        double *Xq = malloc((size_t)N*K*sizeof(double));
        double *yq = malloc((size_t)N*sizeof(double));
        memcpy(Xq, X, (size_t)N*K*sizeof(double));
        memcpy(yq, y, (size_t)N*sizeof(double));
        int info2 = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', N, K, 1, Xq, N, yq, N);
        if(info2){ free(Xq); free(yq); return info2; }
        for(int j=0;j<K;j++) b[j] = yq[j];
        /* residuals = y - X*b */
        for(long i=0;i<N;i++) resid[i] = y[i];
        cblas_dgemv(CblasColMajor, CblasNoTrans, N, K, -1.0, X, N, b, 1, 1.0, resid, 1);
        free(Xq); free(yq);
        return 0;
    }

    /* Rank deficient: greedily drop columns by QR with column pivoting.
     * LAPACKE_dgeqp3 returns pivots; we keep the first eff_rank pivoted
     * columns and zero out the rest.  Then re-solve on the kept subset. */
    int *jpvt = calloc(K, sizeof(int));
    double *Xp = malloc((size_t)N*K*sizeof(double));
    memcpy(Xp, X, (size_t)N*K*sizeof(double));
    double *tau = malloc(K*sizeof(double));
    int info_qp = LAPACKE_dgeqp3(LAPACK_COL_MAJOR, N, K, Xp, N, jpvt, tau);
    free(Xp); free(tau);
    if(info_qp){ free(jpvt); return info_qp; }
    /* jpvt is 1-based: kept columns are jpvt[0..eff_rank-1]-1; dropped are the rest. */
    int *keep = calloc(K, sizeof(int));
    for(int k=0;k<eff_rank;k++) keep[jpvt[k]-1] = 1;
    for(int k=0;k<K;k++) if(!keep[k]) omitted[k] = 1;
    free(jpvt); free(keep);

    /* Build reduced design and solve via dgels. */
    int Kr = eff_rank;
    double *Xr = malloc((size_t)N*Kr*sizeof(double));
    int col=0;
    for(int j=0;j<K;j++) if(!omitted[j]){
        memcpy(Xr + (size_t)col*N, X + (size_t)j*N, N*sizeof(double));
        col++;
    }
    double *yr = malloc(N*sizeof(double));
    memcpy(yr, y, N*sizeof(double));
    int info3 = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', N, Kr, 1, Xr, N, yr, N);
    if(info3){ free(Xr); free(yr); return info3; }
    int rj=0;
    for(int j=0;j<K;j++){ b[j] = omitted[j] ? 0.0 : yr[rj++]; }
    free(Xr); free(yr);
    /* residuals from full X * b (zeros at omitted slots make this equivalent) */
    for(long i=0;i<N;i++) resid[i] = y[i];
    cblas_dgemv(CblasColMajor, CblasNoTrans, N, K, -1.0, X, N, b, 1, 1.0, resid, 1);
    return 0;
}

/* ---- variance computations -------------------------------------------- */
/* Compute (X'X)^-1 for the *non-omitted* coefficients and expand back to
 * a K×K matrix with zeros at omitted rows/cols. */
static double *compute_XtXinv(const double *X, long N, int K, const int *omitted){
    int Kr=0; for(int j=0;j<K;j++) if(!omitted[j]) Kr++;
    /* calloc not malloc — silences GCC's "may be uninitialized" false positive
     * (the conditional copy below fully fills Xr, but the analyzer can't tell). */
    double *Xr = calloc((size_t)N*(Kr>0?Kr:1), sizeof(double));
    int col=0;
    for(int j=0;j<K;j++) if(!omitted[j]){
        memcpy(Xr + (size_t)col*N, X + (size_t)j*N, N*sizeof(double));
        col++;
    }
    double *XtX = calloc((size_t)Kr*Kr, sizeof(double));
    at_b(Xr, N, Kr, Xr, Kr, XtX);
    mat_inv_sym(XtX, Kr);
    double *V = calloc((size_t)K*K, sizeof(double));
    int rj=0; int *map = malloc(K*sizeof(int));
    for(int j=0;j<K;j++) map[j] = omitted[j] ? -1 : rj++;
    for(int i=0;i<K;i++) for(int j=0;j<K;j++){
        int ii=map[i], jj=map[j];
        if(ii<0 || jj<0) continue;
        V[(size_t)i*K + j] = XtX[(size_t)ii*Kr + jj];
    }
    free(Xr); free(XtX); free(map);
    return V;
}

static double *robust_V(const double *X, long N, int K, const int *omitted,
                        const double *resid){
    /* HC1: (X'X)^-1 X' diag(e^2) X (X'X)^-1 * N/(N-Kr) */
    int Kr=0; for(int j=0;j<K;j++) if(!omitted[j]) Kr++;
    double *Xr = malloc((size_t)N*Kr*sizeof(double));
    int col=0;
    for(int j=0;j<K;j++) if(!omitted[j]){
        memcpy(Xr + (size_t)col*N, X + (size_t)j*N, N*sizeof(double));
        col++;
    }
    /* meat = sum_i e_i^2 x_i x_i' */
    double *meat = calloc((size_t)Kr*Kr, sizeof(double));
    for(long i=0;i<N;i++){
        double e2 = resid[i]*resid[i];
        for(int j=0;j<Kr;j++) for(int k=0;k<Kr;k++)
            meat[(size_t)j*Kr + k] += e2 * Xr[(size_t)j*N+i] * Xr[(size_t)k*N+i];
    }
    double *XtX = calloc((size_t)Kr*Kr, sizeof(double));
    at_b(Xr, N, Kr, Xr, Kr, XtX);
    mat_inv_sym(XtX, Kr);
    /* sandwich = XtXinv * meat * XtXinv */
    double *tmp = calloc((size_t)Kr*Kr, sizeof(double));
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, Kr, Kr, Kr,
                1.0, XtX, Kr, meat, Kr, 0.0, tmp, Kr);
    double *sand = calloc((size_t)Kr*Kr, sizeof(double));
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, Kr, Kr, Kr,
                1.0, tmp, Kr, XtX, Kr, 0.0, sand, Kr);
    double adj = (double)N / (double)(N - Kr);
    for(int i=0;i<Kr*Kr;i++) sand[i] *= adj;
    /* expand to K×K */
    double *V = calloc((size_t)K*K, sizeof(double));
    int rj=0; int *map = malloc(K*sizeof(int));
    for(int j=0;j<K;j++) map[j] = omitted[j] ? -1 : rj++;
    for(int i=0;i<K;i++) for(int j=0;j<K;j++){
        int ii=map[i], jj=map[j]; if(ii<0||jj<0) continue;
        V[(size_t)i*K+j] = sand[(size_t)ii*Kr+jj];
    }
    free(Xr); free(meat); free(XtX); free(tmp); free(sand); free(map);
    return V;
}

static double *cluster_V(const double *X, long N, int K, const int *omitted,
                         const double *resid, const long *cid, long G){
    /* CR1: (X'X)^-1 [sum_g (X_g' e_g)(X_g' e_g)'] (X'X)^-1 * (G/(G-1))*((N-1)/(N-Kr)) */
    int Kr=0; for(int j=0;j<K;j++) if(!omitted[j]) Kr++;
    double *Xr = malloc((size_t)N*Kr*sizeof(double));
    int col=0;
    for(int j=0;j<K;j++) if(!omitted[j]){
        memcpy(Xr + (size_t)col*N, X + (size_t)j*N, N*sizeof(double));
        col++;
    }
    /* compute per-cluster sums u_g = sum_{i in g} e_i * x_i,  meat = sum u_g u_g' */
    double *u = calloc((size_t)G*Kr, sizeof(double));   /* G x Kr column-major: u[g + Kr-index*G] */
    for(long i=0;i<N;i++){
        long g = cid[i];
        for(int j=0;j<Kr;j++) u[(size_t)j*G + g] += resid[i] * Xr[(size_t)j*N + i];
    }
    double *meat = calloc((size_t)Kr*Kr, sizeof(double));
    at_b(u, G, Kr, u, Kr, meat);

    double *XtX = calloc((size_t)Kr*Kr, sizeof(double));
    at_b(Xr, N, Kr, Xr, Kr, XtX);
    mat_inv_sym(XtX, Kr);
    double *tmp = calloc((size_t)Kr*Kr, sizeof(double));
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, Kr, Kr, Kr,
                1.0, XtX, Kr, meat, Kr, 0.0, tmp, Kr);
    double *sand = calloc((size_t)Kr*Kr, sizeof(double));
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, Kr, Kr, Kr,
                1.0, tmp, Kr, XtX, Kr, 0.0, sand, Kr);
    double adj = ((double)G/(double)(G-1)) * ((double)(N-1)/(double)(N-Kr));
    for(int i=0;i<Kr*Kr;i++) sand[i] *= adj;
    double *V = calloc((size_t)K*K, sizeof(double));
    int rj=0; int *map = malloc(K*sizeof(int));
    for(int j=0;j<K;j++) map[j] = omitted[j] ? -1 : rj++;
    for(int i=0;i<K;i++) for(int j=0;j<K;j++){
        int ii=map[i], jj=map[j]; if(ii<0||jj<0) continue;
        V[(size_t)i*K+j] = sand[(size_t)ii*Kr+jj];
    }
    free(Xr); free(u); free(meat); free(XtX); free(tmp); free(sand); free(map);
    return V;
}

/* ---- table printer ----------------------------------------------------- */
static void print_regress_table(const Estimates *e){
    /* header summary */
    printf("\n");
    if(e->se_kind==SE_CLUSTER) printf("Linear regression                                       Number of obs = %8ld\n", e->N);
    else printf("      Source |       SS           df       MS      Number of obs   =    %6ld\n",e->N);
    if(e->se_kind==SE_CLASSICAL){
        double Fdf2 = e->df_r;
        printf("-------------+----------------------------------   F(%d, %d)        =   %8.2f\n",e->df_m,(int)Fdf2,e->F);
        printf("       Model | %11.6g %10d %11.6g    Prob > F        =   %8.4f\n",e->mss,e->df_m,e->df_m?e->mss/e->df_m:0.0,e->F_p);
        printf("    Residual | %11.6g %10d %11.6g    R-squared       =   %8.4f\n",e->rss,e->df_r,e->sigma2,e->r2);
        printf("-------------+----------------------------------   Adj R-squared   =   %8.4f\n",e->r2_a);
        printf("       Total | %11.6g %10ld %11.6g    Root MSE        =   %8.4g\n\n",e->tss,e->N-1,e->tss/(e->N-1>0?e->N-1:1),e->rmse);
    } else {
        printf("                                                    F(%d, %d)        =   %8.2f\n",e->df_m,e->df_r,e->F);
        printf("                                                    Prob > F        =   %8.4f\n",e->F_p);
        printf("                                                    R-squared       =   %8.4f\n",e->r2);
        printf("                                                    Root MSE        =   %8.4g\n",e->rmse);
        if(e->se_kind==SE_CLUSTER) printf("                                          (Std. err. adjusted for %ld clusters in %s)\n",e->n_clusters,e->cluster_var);
        printf("\n");
    }
    /* coefficient table */
    const char *selab = e->se_kind==SE_ROBUST?"Robust":e->se_kind==SE_CLUSTER?"Cluster":"Std. err.";
    printf("------------------------------------------------------------------------------\n");
    printf("%12s | Coefficient  %-10s    t    P>|t|     [95%% conf. interval]\n",e->depvar,selab);
    printf("-------------+----------------------------------------------------------------\n");
    double tcrit = tea_invt(0.975, (double)e->df_r);
    for(int i=0;i<e->K;i++){
        if(e->omitted[i]){
            printf("%12s | %10s  (omitted)\n", e->xnames[i],"0");
            continue;
        }
        double se = sqrt(e->V[(size_t)i*e->K+i]);
        double t  = se>0 ? e->b[i]/se : 0;
        double p  = se>0 ? tea_pval_t(t, (double)e->df_r) : 1.0;
        double lo = e->b[i] - tcrit*se;
        double hi = e->b[i] + tcrit*se;
        printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
               e->xnames[i], e->b[i], se, t, p, lo, hi);
    }
    printf("------------------------------------------------------------------------------\n");
}

/* ---- main entry: do_regress ------------------------------------------- */
int do_regress(Cmd *c){
    /* parse: regress y x1 x2 ... [if] [in] [weight] [, noconstant robust cluster(var)] */
    if(!c->varlist[0]){ fprintf(stderr,"regress: depvar and regressors required\n"); return 198; }
    /* split varlist into first token (depvar) and rest (xspec) */
    char vlist[2048]; snprintf(vlist, sizeof vlist, "%s", c->varlist);
    char *sp=NULL; char *dep=strtok_r(vlist," \t",&sp);
    if(!dep){ fprintf(stderr,"regress: depvar required\n"); return 198; }
    char xspec[2048]=""; char *rest=strtok_r(NULL,"",&sp);
    if(rest) snprintf(xspec, sizeof xspec, "%s", rest);

    bool noconst = opt_present(c->options,"noconstant") || opt_present(c->options,"nocons");
    SeKind se_kind = SE_CLASSICAL;
    char clvar[33]="";
    if(opt_value(c->options,"cluster",clvar,sizeof clvar)){ se_kind=SE_CLUSTER; }
    else if(opt_present(c->options,"robust")||opt_present(c->options,"vce")){
        char vce[32]=""; opt_value(c->options,"vce",vce,sizeof vce);
        if(vce[0]==0 || !strcmp(vce,"robust") || !strcmp(vce,"hc1")) se_kind=SE_ROBUST;
        else if(!strncmp(vce,"cluster",7)){ se_kind=SE_CLUSTER;
            /* vce(cluster var) — value is 'cluster var' */
            char *p=vce+7; while(*p==' ')p++; snprintf(clvar,33,"%s",p); }
        else se_kind=SE_ROBUST;
    }
    /* pweight implies robust SE unless the user already specified cluster. */
    if(c->wtype == 3 && se_kind == SE_CLASSICAL){
        se_kind = SE_ROBUST;
    }

    /* Resolve depvar and regressors through the shared TS-op expander so
     * forms like L.y, L(1/2).x, D.(x y) all work uniformly.  Temporary
     * frame columns get appended for the duration of the command and
     * dropped via tsop_drop_temps at the end. */
    int *dep_idx = NULL; int n_temps = 0; const char *vlerr = NULL;
    int n_dep = tsop_expand_varlist(c->f, dep, &dep_idx, &n_temps, &vlerr);
    if(n_dep < 0){
        fprintf(stderr,"regress: %s\n", vlerr ? vlerr : "depvar not found");
        return 198;
    }
    if(n_dep != 1){
        fprintf(stderr,"regress: depvar must resolve to one variable\n");
        free(dep_idx); tsop_drop_temps(c->f, n_temps);
        return 198;
    }
    int yi = dep_idx[0];
    free(dep_idx);

    int *xi = NULL; int nx = 0; int xtemps = 0;
    nx = tsop_expand_varlist(c->f, xspec[0]?xspec:"_all",
                             &xi, &xtemps, &vlerr);
    if(nx < 0){
        fprintf(stderr,"regress: %s\n", vlerr ? vlerr : "regressors not found");
        tsop_drop_temps(c->f, n_temps);
        return 198;
    }
    n_temps += xtemps;

    /* "_all" includes the depvar — drop it from the regressor list so we
     * don't regress y on y plus everything else. */
    if(!xspec[0]){
        int w = 0;
        for(int j=0;j<nx;j++) if(xi[j] != yi) xi[w++] = xi[j];
        nx = w;
    }

    Design D;
    const char *err;
    if(build_design(c->f, yi, xi, nx, c->ifexp[0]?c->ifexp:NULL,
                    c->in_lo, c->in_hi, c->wexp[0]?c->wexp:NULL, c->wtype,
                    !noconst, &D, &err)){
        fprintf(stderr,"regress: %s\n", err?err:"build failed");
        free(xi); tsop_drop_temps(c->f, n_temps);
        return 198;
    }
    free(xi);

    /* WLS transformation: when weights are present, the OLS solve on
     * (sqrt(w)*X, sqrt(w)*y) reproduces the WLS solution.  The resulting
     * residuals are sqrt(w)*e — exactly what classical, robust, and
     * cluster sandwich variance formulas need (since e_w² == w*e²).
     * For aweight, build_design has already renormalized so sum(w)==N. */
    double *Xfit = D.X, *yfit = D.y;
    double *Xw_alloc = NULL, *yw_alloc = NULL;
    double sw = (double)D.N;
    if(D.w){
        sw = 0.0; for(long i=0;i<D.N;i++) sw += D.w[i];
        Xw_alloc = malloc((size_t)D.N * D.K * sizeof(double));
        yw_alloc = malloc((size_t)D.N * sizeof(double));
        for(long i=0;i<D.N;i++){
            double rt = sqrt(D.w[i]);
            yw_alloc[i] = rt * D.y[i];
            for(int j=0;j<D.K;j++) Xw_alloc[(size_t)j*D.N + i] = rt * D.X[(size_t)j*D.N + i];
        }
        Xfit = Xw_alloc;
        yfit = yw_alloc;
    }

    /* solve */
    double *b = calloc(D.K, sizeof(double));
    double *resid = malloc(D.N * sizeof(double));
    int *omitted = calloc(D.K, sizeof(int));
    int rank=0;
    int rc = ols_solve_drop(Xfit, yfit, D.N, D.K, b, resid, omitted, &rank);
    if(rc){ fprintf(stderr,"regress: numerical solve failed (LAPACK info=%d)\n",rc);
        design_free(&D); free(b); free(resid); free(omitted);
        free(Xw_alloc); free(yw_alloc);
        tsop_drop_temps(c->f, n_temps);
        return 198; }

    /* variance */
    int Kr=0; for(int j=0;j<D.K;j++) if(!omitted[j]) Kr++;
    long *cid=NULL; long G=0;
    if(se_kind==SE_CLUSTER){
        int cvi = var_find(c->f, clvar);
        if(cvi < 0){ fprintf(stderr,"regress: cluster var %s not found\n",clvar);
            design_free(&D); free(b); free(resid); free(omitted);
            free(Xw_alloc); free(yw_alloc);
            tsop_drop_temps(c->f, n_temps);
            return 111; }
        cid = malloc(D.N*sizeof(long));
        /* assign each kept row a cluster id by enumerating distinct cluster values */
        Variable *cv = &c->f->vars[cvi];
        double *seen_d = NULL; char **seen_s = NULL;
        long row=0;
        for(size_t i=0;i<D.nobs_full;i++){
            if(!D.used[i]) continue;
            long g=-1;
            if(cv->type==VT_NUM){ double v=cv->num[i];
                for(long k=0;k<G;k++) if(seen_d[k]==v){g=k;break;}
                if(g<0){ seen_d=realloc(seen_d,(G+1)*sizeof(double)); seen_d[G]=v; g=G++; }
            } else { const char *v=cv->str[i];
                for(long k=0;k<G;k++) if(!strcmp(seen_s[k],v)){g=k;break;}
                if(g<0){ seen_s=realloc(seen_s,(G+1)*sizeof(char*)); seen_s[G]=strdup(v); g=G++; }
            }
            cid[row++]=g;
        }
        free(seen_d);
        if(seen_s){ for(long k=0;k<G;k++) free(seen_s[k]); free(seen_s); }
    }

    /* statistics — for weighted regression: ȳ_w = sum(wy)/sum(w),
     * TSS_w = sum(w(y-ȳ)²), and resid is already sqrt(w)-scaled so
     * resid² = w*e² and sum(resid²) is the weighted RSS. */
    double ybar=0, tss=0, rss=0;
    if(D.w){
        double swy=0;
        for(long i=0;i<D.N;i++) swy += D.w[i] * D.y[i];
        ybar = swy / sw;
        for(long i=0;i<D.N;i++){ double dy=D.y[i]-ybar; tss += D.w[i] * dy*dy; }
    } else {
        for(long i=0;i<D.N;i++) ybar += D.y[i];
        ybar /= D.N;
        for(long i=0;i<D.N;i++){ double dy=D.y[i]-ybar; tss += dy*dy; }
    }
    for(long i=0;i<D.N;i++) rss += resid[i]*resid[i];
    double mss = tss - rss;

    /* Effective N for df calculation and "Number of obs" reporting:
     *   fweight, iweight: sum of weights (rows are conceptually replicated)
     *   aweight:         unweighted N (after renormalization sw==N anyway)
     *   pweight:         unweighted N (sampling weights don't multiply rows)
     *   none:            unweighted N */
    double Neff = (D.wtype == 1 || D.wtype == 4) ? sw : (double)D.N;
    long   N_disp = (D.wtype == 1 || D.wtype == 4) ? (long)(sw + 0.5) : D.N;

    int df_r = (int)Neff - Kr;
    int df_m = Kr - (D.has_cons ? 1 : 0);
    if(df_m < 1) df_m = 1;
    double sigma2 = rss / df_r;
    double r2 = (D.has_cons && tss > 0) ? 1 - rss/tss : (tss>0? 1 - rss/tss : 0);
    double r2_a = 1 - (1-r2) * ((Neff - 1) / (double)df_r);

    double *V = NULL;
    if(se_kind==SE_CLASSICAL){
        V = compute_XtXinv(Xfit, D.N, D.K, omitted);
        for(int i=0;i<D.K*D.K;i++) V[i] *= sigma2;
    } else if(se_kind==SE_ROBUST){
        V = robust_V(Xfit, D.N, D.K, omitted, resid);
    } else {
        V = cluster_V(Xfit, D.N, D.K, omitted, resid, cid, G);
    }

    /* model F: classical = (R²/dfm) / ((1-R²)/dfr).  Wald F otherwise via
     * R b under the null all slopes = 0.  For simplicity classical formula. */
    double F = 0, F_p = 1.0;
    if(df_m>0 && df_r>0){
        if(se_kind==SE_CLASSICAL && D.has_cons){
            F = (r2/df_m) / ((1-r2)/df_r);
        } else {
            /* Wald F: b_slopes' V_slopes^-1 b_slopes / dfm  */
            int *slope_idx=malloc(D.K*sizeof(int)); int sn=0;
            for(int j=0;j<D.K;j++) if(!omitted[j]){
                if(D.has_cons && !strcmp(D.xnames[j],"_cons")) continue;
                slope_idx[sn++]=j;
            }
            if(sn>0){
                double *bs=malloc(sn*sizeof(double));
                double *Vs=malloc(sn*sn*sizeof(double));
                for(int i=0;i<sn;i++){ bs[i]=b[slope_idx[i]];
                    for(int j=0;j<sn;j++) Vs[(size_t)i*sn+j] = V[(size_t)slope_idx[i]*D.K + slope_idx[j]]; }
                mat_inv_sym(Vs, sn);
                double q=0;
                for(int i=0;i<sn;i++) for(int j=0;j<sn;j++) q += bs[i]*Vs[(size_t)i*sn+j]*bs[j];
                F = q/sn; df_m = sn;
                free(bs); free(Vs);
            }
            free(slope_idx);
        }
        F_p = tea_pval_f(F, df_m, df_r);
    }

    /* populate Estimates on the workspace */
    Estimates *e = est_new();
    snprintf(e->cmd, 16, "regress");
    snprintf(e->depvar, 33, "%s", dep);
    e->K = D.K;
    e->xnames = malloc(D.K * sizeof(*e->xnames));
    memcpy(e->xnames, D.xnames, D.K * sizeof(*e->xnames));
    e->omitted = omitted;       /* take ownership */
    e->b = b; e->V = V;
    e->N = N_disp; e->df_r = df_r; e->df_m = df_m; e->has_cons = D.has_cons;
    e->r2 = r2; e->r2_a = r2_a; e->rmse = sqrt(sigma2);
    e->F = F; e->F_p = F_p;
    e->tss = tss; e->rss = rss; e->mss = mss; e->sigma2 = sigma2;
    e->se_kind = se_kind;
    snprintf(e->cluster_var, 33, "%s", clvar);
    e->n_clusters = G;
    e->nobs_at_fit = D.nobs_full;
    e->used = D.used; D.used = NULL;   /* take ownership */
    snprintf(e->fitted_frame, 33, "%s", c->f->name);

    est_free(c->ws->last_est);
    c->ws->last_est = e;

    if(!c->quiet) print_regress_table(e);

    /* set selected r() macros to mirror Stata's e() for now */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",e->N); mac_set(&c->ip->rret,"e(N)",bb);
    snprintf(bb,sizeof bb,"%.10g",e->r2); mac_set(&c->ip->rret,"e(r2)",bb);
    snprintf(bb,sizeof bb,"%.10g",e->r2_a); mac_set(&c->ip->rret,"e(r2_a)",bb);
    snprintf(bb,sizeof bb,"%.10g",e->rmse); mac_set(&c->ip->rret,"e(rmse)",bb);
    snprintf(bb,sizeof bb,"%.10g",e->F); mac_set(&c->ip->rret,"e(F)",bb);
    snprintf(bb,sizeof bb,"%.10g",e->F_p); mac_set(&c->ip->rret,"e(p)",bb);
    snprintf(bb,sizeof bb,"%d",e->df_m); mac_set(&c->ip->rret,"e(df_m)",bb);
    snprintf(bb,sizeof bb,"%d",e->df_r); mac_set(&c->ip->rret,"e(df_r)",bb);
    store_coef_macros(e, &c->ip->rret);

    /* clean up D (used was transferred out) */
    free(D.y); free(D.X); free(D.w); free(D.xnames);
    free(resid); free(cid);
    free(Xw_alloc); free(yw_alloc);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- predict ----------------------------------------------------------- */
/* predict newvar [, option]
 *
 * After regress/logit/probit/xtreg, generate a new variable derived from
 * the most recent estimates.  Options depend on the previous command:
 *
 *   regress (OLS):
 *     xb         (default): linear prediction Xβ
 *     residuals  : y - Xβ (only for in-sample rows)
 *     stdp       : SE of the linear prediction (NOT yet implemented for
 *                  weighted regression)
 *
 *   logit / probit:
 *     pr         (default): predicted probability  Λ(Xβ) or Φ(Xβ)
 *     xb         : linear index Xβ
 *
 *   xtreg fe / re:
 *     xb         (default): linear prediction Xβ_hat (does NOT include u_i)
 *     u          : panel effect α̂_i  (= ybar_i - xbar_i' β, in-sample only)
 *     e          : idiosyncratic residual y - Xβ - α̂_i  (in-sample only)
 *     ue         : combined residual α̂_i + e_i  (= y - Xβ; in-sample only)
 *     xbu        : Xβ + α̂_i  (in-sample only — α̂_i unknown out-of-sample)
 *
 * For TS-op coefficient names like `L.growth`, predict materializes
 * the lagged column temporarily via tsop_expand_varlist (same code that
 * estimation used), evaluates Xβ, then drops the temps.
 */

/* Decode option flags by command into a single integer kind */
enum PredKind {
    PK_XB = 0,            /* default for regress, xtreg; also valid for logit/probit */
    PK_RESID,             /* regress only */
    PK_STDP,              /* regress; logit/probit ok in principle but not done */
    PK_PR,                /* logit/probit default */
    PK_U,                 /* xtreg: alpha_i */
    PK_E,                 /* xtreg: idiosyncratic resid */
    PK_UE,                /* xtreg: alpha_i + e_i = y - Xβ */
    PK_XBU,               /* xtreg: Xβ + alpha_i */
};

/* Map command + options to a prediction kind.  Returns -1 on bad option. */
static int predict_resolve_kind(const Estimates *e, const char *options)
{
    bool xb       = opt_present(options, "xb");
    bool resid    = opt_present(options, "residuals") || opt_present(options, "resid");
    bool stdp     = opt_present(options, "stdp");
    bool pr       = opt_present(options, "pr") || opt_present(options, "p");
    bool u        = opt_present(options, "u");
    bool ec       = opt_present(options, "e");
    bool ue       = opt_present(options, "ue");
    bool xbu      = opt_present(options, "xbu");

    bool is_glm   = (!strcmp(e->cmd,"logit") || !strcmp(e->cmd,"probit"));
    bool is_xtreg = !strcmp(e->cmd, "xtreg");

    /* Default depends on estimator. */
    int requested = 0;
    if(xb)    requested++;
    if(resid) requested++;
    if(stdp)  requested++;
    if(pr)    requested++;
    if(u)     requested++;
    if(ec)    requested++;
    if(ue)    requested++;
    if(xbu)   requested++;
    if(requested > 1) return -1;

    if(requested == 0){
        /* defaults */
        if(is_glm) return PK_PR;
        return PK_XB;
    }
    if(xb)    return PK_XB;
    if(resid) return is_xtreg ? -2 : PK_RESID;     /* regress only */
    if(stdp)  return PK_STDP;
    if(pr){
        if(!is_glm) return -3;                      /* pr only for logit/probit */
        return PK_PR;
    }
    if(u)   { if(!is_xtreg) return -4; return PK_U;  }
    if(ec)  { if(!is_xtreg) return -4; return PK_E;  }
    if(ue)  { if(!is_xtreg) return -4; return PK_UE; }
    if(xbu) { if(!is_xtreg) return -4; return PK_XBU;}
    return -1;
}

int do_predict(Cmd *c)
{
    Estimates *e = c->ws->last_est;
    if(!e){ fprintf(stderr,"predict: no estimates available\n"); return 301; }
    char newvar[33]=""; sscanf(c->varlist,"%32s",newvar);
    if(!newvar[0]){ fprintf(stderr,"predict: new variable name required\n"); return 198; }
    if(var_find(c->f, newvar) >= 0){
        fprintf(stderr,"predict: %s already exists\n", newvar); return 110;
    }

    int kind = predict_resolve_kind(e, c->options);
    if(kind == -1){ fprintf(stderr,"predict: at most one option may be specified\n"); return 198; }
    if(kind == -2){ fprintf(stderr,"predict: residuals not supported after xtreg; use ,e or ,ue\n"); return 198; }
    if(kind == -3){ fprintf(stderr,"predict: pr is only valid after logit or probit\n"); return 198; }
    if(kind == -4){ fprintf(stderr,"predict: u, e, ue, xbu are only valid after xtreg\n"); return 198; }

    /* Resolve each regressor name to a column index in the *current*
     * frame.  TS-op names like "L.growth" need to be materialized via
     * tsop_expand_varlist.  But we need to drop the temp columns BEFORE
     * adding the new variable (otherwise tsop_drop_temps would drop the
     * new variable instead of the temps).  So we snapshot the data from
     * temp columns into our own buffers, then drop the temps, then add
     * the new variable. */
    int *xi = malloc(e->K * sizeof(int));     /* col index in c->f, or -1 for _cons */
    /* For TS-op resolved columns, we also remember the snapshot. */
    double **snap = calloc(e->K, sizeof(double*));   /* snap[j] = NULL means use xi[j] from c->f directly */
    size_t snap_n = c->f->nobs;
    int n_temps = 0;
    for(int j=0; j<e->K; j++){
        if(!strcmp(e->xnames[j], "_cons")){ xi[j] = -1; continue; }
        int idx = var_find(c->f, e->xnames[j]);
        if(idx >= 0){ xi[j] = idx; continue; }
        int *one = NULL; int these_temps = 0; const char *vlerr = NULL;
        int got = tsop_expand_varlist(c->f, e->xnames[j], &one, &these_temps, &vlerr);
        if(got != 1 || !one){
            fprintf(stderr,"predict: %s not in current data%s%s\n",
                    e->xnames[j], vlerr?" (":"", vlerr?vlerr:"");
            if(vlerr) fprintf(stderr,")");
            tsop_drop_temps(c->f, n_temps + these_temps);
            for(int k=0;k<j;k++) free(snap[k]);
            free(snap); free(xi); free(one);
            return 111;
        }
        /* Snapshot the temp column so we own the data after dropping. */
        snap[j] = malloc(snap_n * sizeof(double));
        memcpy(snap[j], c->f->vars[one[0]].num, snap_n * sizeof(double));
        xi[j] = -2;   /* sentinel: look up in snap[j] not c->f */
        n_temps += these_temps;
        free(one);
    }
    /* Drop all temp columns BEFORE we add the new variable. */
    tsop_drop_temps(c->f, n_temps);
    n_temps = 0;

    Variable *v = var_add(c->f, newvar, VT_NUM);

    /* For predictions that need in-sample info (residuals, u, e, ue, xbu),
     * we need the per-row used[] flag from the estimation, the depvar
     * column (residuals), and panel info (xtreg).  used[] is sized to the
     * frame's nobs at fit; if rows were added since, the new rows are
     * automatically out-of-sample. */
    bool need_y = (kind == PK_RESID || kind == PK_E || kind == PK_UE
                   || kind == PK_XBU || kind == PK_U);   /* PK_U needs y to compute α̂ */
    int yi = need_y ? var_find(c->f, e->depvar) : -1;
    if(need_y && yi < 0){
        fprintf(stderr,"predict: depvar %s not in current data\n", e->depvar);
        tsop_drop_temps(c->f, n_temps); free(xi); return 111;
    }

    /* For xtreg u/e/ue/xbu: need per-panel α̂_i = ȳ_i - x̄_i'β. */
    bool need_alpha = (!strcmp(e->cmd,"xtreg") &&
                       (kind == PK_U || kind == PK_E || kind == PK_UE || kind == PK_XBU));
    long G = 0;
    double *alpha = NULL;
    long *panel_of_row = NULL;
    if(need_alpha){
        if(c->f->ts_panel < 0){
            fprintf(stderr,"predict: xtset must still be in effect for predict ,u/,e/,ue/,xbu\n");
            tsop_drop_temps(c->f, n_temps); free(xi); return 459;
        }
        /* Compute per-panel α from in-sample rows.  Identify panels by the
         * panel-variable's value at each row; build a sequence of (panel,
         * ybar, xbar' β) tuples. */
        Variable *pv = &c->f->vars[c->f->ts_panel];
        bool pv_str = (pv->type == VT_STR);

        /* In-sample rows only: rely on e->used.  If used[] is missing (some
         * legacy estimates), error. */
        if(!e->used){
            fprintf(stderr,"predict: in-sample bookkeeping missing for previous estimates\n");
            tsop_drop_temps(c->f, n_temps); free(xi); return 198;
        }
        /* Stream over in-sample rows in order; since xtset has sorted the
         * data, panels are contiguous. */
        size_t Nfull = e->nobs_at_fit;
        if(Nfull > c->f->nobs) Nfull = c->f->nobs;   /* defensive */
        /* First pass: count panels */
        long g = -1;
        size_t last_first = 0;
        for(size_t i=0; i<Nfull; i++){
            if(!e->used[i]) continue;
            bool new_panel = (g < 0);
            if(!new_panel){
                if(pv_str) new_panel = (strcmp(pv->str[i], pv->str[last_first]) != 0);
                else       new_panel = (pv->num[i] != pv->num[last_first]);
            }
            if(new_panel){ g++; last_first = i; }
        }
        G = g + 1;
        alpha = malloc((G>0?G:1) * sizeof(double));
        long *first_row = malloc((G>0?G:1) * sizeof(long));
        long *counts    = malloc((G>0?G:1) * sizeof(long));
        for(long k=0; k<G; k++){ counts[k]=0; first_row[k]=-1; alpha[k]=0; }
        /* Build first_row, counts, and accumulate ybar, xbar_β for each panel.
         * α̂_i = ȳ_i - x̄_i' β = (1/T_i) sum_t (y_it - x_it' β).  */
        g = -1; last_first = 0;
        for(size_t i=0; i<Nfull; i++){
            if(!e->used[i]) continue;
            bool new_panel = (g < 0);
            if(!new_panel){
                if(pv_str) new_panel = (strcmp(pv->str[i], pv->str[last_first]) != 0);
                else       new_panel = (pv->num[i] != pv->num[last_first]);
            }
            if(new_panel){ g++; last_first = i; first_row[g] = i; }
            counts[g]++;
            double xb = 0;
            for(int j=0; j<e->K; j++){
                if(e->omitted[j]) continue;
                if(!strcmp(e->xnames[j],"_cons")){ xb += e->b[j]; continue; }
                double xv = (xi[j] == -2) ? snap[j][i] : c->f->vars[xi[j]].num[i];
                if(sv_is_miss(xv)){ xb = SV_MISS; break; }
                xb += e->b[j] * xv;
            }
            double yi_val = c->f->vars[yi].num[i];
            if(sv_is_miss(xb) || sv_is_miss(yi_val)){
                /* row contributed nothing usable; very rare after listwise
                 * deletion in build_design, but possible if user altered
                 * the data after estimation */
                continue;
            }
            alpha[g] += (yi_val - xb);
        }
        for(long k=0; k<G; k++) if(counts[k] > 0) alpha[k] /= counts[k];

        /* Map every row of the current frame to a panel index (for the in-
         * sample rows only — out-of-sample gets panel_of_row = -1). */
        panel_of_row = malloc(c->f->nobs * sizeof(long));
        for(size_t i=0; i<c->f->nobs; i++) panel_of_row[i] = -1;
        g = -1; last_first = 0;
        for(size_t i=0; i<Nfull; i++){
            if(!e->used[i]) continue;
            bool new_panel = (g < 0);
            if(!new_panel){
                if(pv_str) new_panel = (strcmp(pv->str[i], pv->str[last_first]) != 0);
                else       new_panel = (pv->num[i] != pv->num[last_first]);
            }
            if(new_panel){ g++; last_first = i; }
            panel_of_row[i] = g;
        }
        free(first_row); free(counts);
    }

    long n_filled = 0;
    bool is_glm = (!strcmp(e->cmd,"logit") || !strcmp(e->cmd,"probit"));
    for(size_t i=0; i<c->f->nobs; i++){
        /* Compute Xβ for this row, missing if any factor missing. */
        double xb = 0; bool miss = false;
        for(int j=0; j<e->K; j++){
            if(e->omitted[j]) continue;
            double xv;
            if(xi[j] == -1)      xv = 1.0;                  /* _cons */
            else if(xi[j] == -2) xv = snap[j][i];           /* TS-op snapshot */
            else                 xv = c->f->vars[xi[j]].num[i];
            if(sv_is_miss(xv)){ miss = true; break; }
            xb += e->b[j] * xv;
        }
        if(miss){ v->num[i] = SV_MISS; continue; }

        double out = SV_MISS;
        switch(kind){
            case PK_XB:
                out = xb;
                break;
            case PK_PR:
                if(!strcmp(e->cmd,"logit")){
                    /* stable sigmoid */
                    if(xb >= 0){ double t = exp(-xb); out = 1.0/(1.0+t); }
                    else        { double t = exp(xb);  out = t/(1.0+t); }
                } else {  /* probit */
                    out = tea_normal_cdf(xb);
                }
                break;
            case PK_RESID: {
                int yi2 = var_find(c->f, e->depvar);
                if(yi2 < 0){ out = SV_MISS; break; }
                double y_val = c->f->vars[yi2].num[i];
                out = sv_is_miss(y_val) ? SV_MISS : y_val - xb;
                break;
            }
            case PK_STDP: {
                /* stdp = sqrt(x' V x) for this row.  V is K×K. */
                double s = 0;
                for(int a=0; a<e->K; a++){
                    if(e->omitted[a]) continue;
                    double xa = (xi[a]==-1) ? 1.0 :
                                (xi[a]==-2) ? snap[a][i] : c->f->vars[xi[a]].num[i];
                    for(int b=0; b<e->K; b++){
                        if(e->omitted[b]) continue;
                        double xbv = (xi[b]==-1) ? 1.0 :
                                     (xi[b]==-2) ? snap[b][i] : c->f->vars[xi[b]].num[i];
                        s += xa * e->V[(size_t)a*e->K + b] * xbv;
                    }
                }
                out = s > 0 ? sqrt(s) : 0;
                break;
            }
            case PK_U: {
                long g = (i < c->f->nobs) ? panel_of_row[i] : -1;
                out = (g >= 0) ? alpha[g] : SV_MISS;
                break;
            }
            case PK_E: {
                long g = (i < c->f->nobs) ? panel_of_row[i] : -1;
                if(g < 0){ out = SV_MISS; break; }
                double y_val = c->f->vars[yi].num[i];
                if(sv_is_miss(y_val)){ out = SV_MISS; break; }
                out = y_val - xb - alpha[g];
                break;
            }
            case PK_UE: {
                long g = (i < c->f->nobs) ? panel_of_row[i] : -1;
                if(g < 0){ out = SV_MISS; break; }
                double y_val = c->f->vars[yi].num[i];
                out = sv_is_miss(y_val) ? SV_MISS : (y_val - xb);
                break;
            }
            case PK_XBU: {
                long g = (i < c->f->nobs) ? panel_of_row[i] : -1;
                out = (g >= 0) ? xb + alpha[g] : SV_MISS;
                break;
            }
        }
        v->num[i] = out;
        if(!sv_is_miss(out)) n_filled++;
    }

    free(xi);
    if(snap){ for(int j=0; j<e->K; j++) free(snap[j]); free(snap); }
    free(alpha);
    free(panel_of_row);
    tsop_drop_temps(c->f, n_temps);

    if(!c->quiet){
        const char *kind_name =
            kind==PK_XB    ? "xb" :
            kind==PK_PR    ? (is_glm ? "Pr(y)" : "pr") :
            kind==PK_RESID ? "residuals" :
            kind==PK_STDP  ? "stdp" :
            kind==PK_U     ? "u_i" :
            kind==PK_E     ? "e_it" :
            kind==PK_UE    ? "u_i + e_it" :
            kind==PK_XBU   ? "xb + u_i" : "?";
        printf("(option %s assumed; %ld observations)\n", kind_name, n_filled);
    }
    return 0;
}

/* ---- test -------------------------------------------------------------- */
/* test ‹hypothesis›
 *
 * Supported forms:
 *   test v1 v2 ...                  H0: each β_i = 0  (Wald F joint test)
 *   test v = 0                      H0: β_v = 0
 *   test v = 0.5                    H0: β_v = 0.5
 *   test v1 = v2                    H0: β_v1 = β_v2
 *   test v1 = 0 v2 = 0              joint of two simple hypotheses
 *
 * Equivalent to constructing R, r such that R β = r under H0; the Wald
 * statistic is (Rβ̂ - r)' (R V R')^{-1} (Rβ̂ - r) / q.
 *
 * We build R one row at a time as we parse tokens, then form q×K R, q×1 r,
 * compute Rb-r, R V R', invert, and the quadratic form. */
int do_test(Cmd *c){
    Estimates *e = c->ws->last_est;
    if(!e){ fprintf(stderr,"test: no estimates available\n"); return 301; }
    /* Build R (q rows × K cols) and r (q vector) row-by-row. */
    int K = e->K;
    double *R = NULL; double *r = NULL; int q = 0, cap = 0;
    /* Per-row formatted strings for printing. */
    char (*labels)[128] = NULL;

    /* Token parser that understands TS-op-style names ("L.growth"). */
    const char *p = c->varlist;
    while(*p){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        /* read a coefficient name */
        char nm[64]; int n=0;
        if(!(isalpha((unsigned char)*p) || *p=='_')){
            fprintf(stderr,"test: parse error near '%s'\n",p);
            free(R); free(r); free(labels); return 198;
        }
        while(*p && n<63){
            if(isalnum((unsigned char)*p) || *p=='_'){ nm[n++]=*p++; }
            else if(*p=='.' && (isalnum((unsigned char)p[1])||p[1]=='_')){ nm[n++]=*p++; }
            else break;
        }
        nm[n]=0;
        int k = est_idx_of(e, nm);
        if(k<0){ fprintf(stderr,"test: %s not a coefficient\n",nm);
            free(R); free(r); free(labels); return 111; }
        if(e->omitted[k]){ fprintf(stderr,"test: %s is omitted\n",nm);
            free(R); free(r); free(labels); return 198; }

        /* Optional '= ‹rhs›' where rhs is a number or another coef name. */
        const char *save = p;
        while(*p==' '||*p=='\t') p++;
        double rhs_val = 0;
        int rhs_kind = 0;   /* 0=zero, 1=number, 2=coef k */
        int rhs_kidx = -1;
        char rhs_label[64] = "0";
        if(*p == '='){
            p++;
            while(*p==' '||*p=='\t') p++;
            if(isalpha((unsigned char)*p) || *p=='_'){
                /* coefficient name on RHS */
                char rnm[64]; int rn=0;
                while(*p && rn<63){
                    if(isalnum((unsigned char)*p) || *p=='_'){ rnm[rn++]=*p++; }
                    else if(*p=='.' && (isalnum((unsigned char)p[1])||p[1]=='_')){ rnm[rn++]=*p++; }
                    else break;
                }
                rnm[rn]=0;
                int kr = est_idx_of(e, rnm);
                if(kr<0){ fprintf(stderr,"test: %s not a coefficient\n",rnm);
                    free(R); free(r); free(labels); return 111; }
                rhs_kind = 2; rhs_kidx = kr;
                snprintf(rhs_label, sizeof rhs_label, "%s", rnm);
            } else {
                char *endp; double v = strtod(p, &endp);
                if(endp == p){ fprintf(stderr,"test: expected number or coef name after '='\n");
                    free(R); free(r); free(labels); return 198; }
                p = endp;
                rhs_val = v; rhs_kind = 1;
                snprintf(rhs_label, sizeof rhs_label, "%g", v);
            }
        } else {
            p = save;  /* no '=' here; the next iteration will reparse */
        }

        /* Append a row to R, value to r. */
        if(q == cap){ cap = cap ? cap*2 : 8;
            R = realloc(R, (size_t)cap*K*sizeof(double));
            r = realloc(r, cap*sizeof(double));
            labels = realloc(labels, cap*sizeof *labels);
        }
        for(int j=0;j<K;j++) R[(size_t)q*K + j] = 0;
        R[(size_t)q*K + k] = 1;
        if(rhs_kind == 2){
            R[(size_t)q*K + rhs_kidx] = -1;  /* β_k - β_{rhs} = 0 */
            r[q] = 0;
        } else {
            r[q] = rhs_val;
        }
        snprintf(labels[q], sizeof labels[q], "%s = %s", nm, rhs_label);
        q++;
    }
    if(q < 1){ fprintf(stderr,"test: at least one hypothesis required\n");
        free(R); free(r); free(labels); return 198; }

    /* Compute Rb - r (q×1) and R V R' (q×q). */
    double *Rb = malloc(q*sizeof(double));
    for(int i=0;i<q;i++){
        double s = 0;
        for(int j=0;j<K;j++) s += R[(size_t)i*K + j] * e->b[j];
        Rb[i] = s - r[i];
    }
    double *RV = malloc((size_t)q*K*sizeof(double));   /* q × K */
    for(int i=0;i<q;i++) for(int j=0;j<K;j++){
        double s = 0;
        for(int kk=0;kk<K;kk++) s += R[(size_t)i*K + kk] * e->V[(size_t)kk*K + j];
        RV[(size_t)i*K + j] = s;
    }
    double *RVRt = calloc((size_t)q*q, sizeof(double));
    for(int i=0;i<q;i++) for(int j=0;j<q;j++){
        double s = 0;
        for(int kk=0;kk<K;kk++) s += RV[(size_t)i*K + kk] * R[(size_t)j*K + kk];
        RVRt[(size_t)i*q + j] = s;
    }
    mat_inv_sym(RVRt, q);

    double F = 0;
    for(int i=0;i<q;i++) for(int j=0;j<q;j++) F += Rb[i] * RVRt[(size_t)i*q + j] * Rb[j];
    F /= q;
    double pv = tea_pval_f(F, q, e->df_r);

    printf("\n");
    for(int i=0;i<q;i++) printf(" ( %d)  %s\n", i+1, labels[i]);
    printf("\n");
    printf("       F(%d, %d) = %8.2f\n", q, e->df_r, F);
    printf("            Prob > F = %8.4f\n\n", pv);

    char b32[32]; snprintf(b32,sizeof b32,"%.10g",F); mac_set(&c->ip->rret,"r(F)",b32);
    snprintf(b32,sizeof b32,"%.10g",pv); mac_set(&c->ip->rret,"r(p)",b32);
    snprintf(b32,sizeof b32,"%d",q); mac_set(&c->ip->rret,"r(df)",b32);

    free(R); free(r); free(Rb); free(RV); free(RVRt); free(labels);
    return 0;
}

/* ---- lincom ------------------------------------------------------------ */
/* lincom <linear combination of coef names>, e.g.  lincom x1 - x2
 *  parsing: a sum of (signed) terms, each '[coef *] name' or '[coef]' */
int do_lincom(Cmd *c){
    Estimates *e = c->ws->last_est;
    if(!e){ fprintf(stderr,"lincom: no estimates available\n"); return 301; }
    double *L = calloc(e->K, sizeof(double));
    double offset = 0.0;
    const char *p = c->varlist;
    int sign = 1;
    while(*p){
        while(*p==' ') p++;
        if(!*p) break;
        if(*p=='+'){ sign=1; p++; continue; }
        if(*p=='-'){ sign=-1; p++; continue; }
        /* parse number? */
        char *end; double num = strtod(p, &end);
        double coef = 1.0; bool had_num = (end!=p);
        if(had_num){ coef = num; p = end; while(*p==' ')p++;
            if(*p=='*'){ p++; while(*p==' ')p++; } }
        if(isalpha((unsigned char)*p)||*p=='_'){
            /* Coefficient names can include '.' for TS-op forms like
             * "L.growth", "L2.growth", "D.x", "F.y".  Accept '.' inside
             * the name but only when it's followed by another name
             * character — so "L.growth + L2.growth" parses correctly
             * (the '+' and ' ' end the name as expected). */
            char nm[33]; int n=0;
            while(*p && n<32){
                if(isalnum((unsigned char)*p) || *p=='_'){ nm[n++]=*p++; }
                else if(*p == '.' && (isalnum((unsigned char)p[1]) || p[1]=='_')){ nm[n++]=*p++; }
                else break;
            }
            nm[n]=0;
            int k = est_idx_of(e, nm);
            if(k<0){ fprintf(stderr,"lincom: %s not a coefficient\n",nm); free(L); return 111; }
            L[k] += sign * coef;
        } else if(had_num){
            offset += sign * coef;
        } else { fprintf(stderr,"lincom: parse error near '%s'\n",p); free(L); return 198; }
        sign = 1;
    }
    /* point estimate */
    double est = offset;
    for(int j=0;j<e->K;j++) est += L[j] * e->b[j];
    /* variance: L V L' */
    double *VL = calloc(e->K, sizeof(double));
    for(int j=0;j<e->K;j++) for(int k=0;k<e->K;k++) VL[j] += e->V[(size_t)j*e->K+k] * L[k];
    double var=0; for(int j=0;j<e->K;j++) var += L[j]*VL[j];
    double se = var>0 ? sqrt(var) : 0;
    double t = se>0 ? est/se : 0;
    double pv = se>0 ? tea_pval_t(t, e->df_r) : 1.0;
    double tcrit = tea_invt(0.975, e->df_r);
    printf("\n( 1)  %s\n\n", c->varlist);
    printf("------------------------------------------------------------------------------\n");
    printf("%12s | Coefficient  Std. err.    t    P>|t|     [95%% conf. interval]\n",e->depvar);
    printf("-------------+----------------------------------------------------------------\n");
    printf("       (1)   | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
           est, se, t, pv, est-tcrit*se, est+tcrit*se);
    printf("------------------------------------------------------------------------------\n");
    free(L); free(VL);
    return 0;
}

/* ---- xtreg ------------------------------------------------------------- */
/* xtreg y x1 x2 ... [if] [in], fe [vce(robust|cluster var)]
 *
 * Fixed-effects (within) estimator.  Requires xtset to identify the panel
 * variable.  Method:
 *   1. Resolve depvar + regressors via tsop_expand_varlist (TS ops work).
 *   2. Listwise-delete missing y or X.
 *   3. Within-transform: subtract panel mean of y and each X.
 *   4. OLS on the demeaned data, no constant.
 *   5. Effective df: N - n_groups - K (lose one df per panel for FE).
 *   6. Report R-within (this regression), R-between, R-overall, plus the
 *      panel diagnostics Stata users expect.
 *
 * The intercept Stata reports as _cons is the average of the fixed
 * effects, computed as y_bar_overall - x_bar_overall' * beta_hat.  Its
 * SE in the strictly correct LSDV-equivalent sense involves variance
 * propagation through the panel means.  We compute the point estimate
 * but report SE=0 for v1 — to be revisited.  (For inference on _cons,
 * users wanting strict accuracy should run `regress y x i.panel`.)
 *
 * Standard errors:
 *   classical (default): σ²·(X_w' X_w)^{-1}, σ² = RSS/(N - n_groups - K)
 *   vce(robust):         HC1 sandwich on the within-transformed data
 *   vce(cluster v):      cluster-robust on the within-transformed data
 *
 * (Stata defaults `vce(robust)` to cluster-by-panel; we keep HC1 for v1
 * and let users explicitly cluster.) */
int do_xtreg(Cmd *c)
{
    if(!c->varlist[0]){ fprintf(stderr,"xtreg: depvar and regressors required\n"); return 198; }

    /* Require xtset with a panel variable. */
    if(c->f->ts_panel < 0){
        fprintf(stderr,"xtreg: panel variable not set; use xtset first\n");
        return 459;
    }

    /* Estimator selection: fe (default), re (random effects via FGLS),
     * be (between effects). */
    bool mode_be = opt_present(c->options,"be");
    bool mode_re = opt_present(c->options,"re");
    if(mode_be && mode_re){
        fprintf(stderr,"xtreg: cannot combine ,be and ,re\n");
        return 198;
    }
    /* fe is the default; if the user wrote ,fe it's a no-op. */

    SeKind se_kind = SE_CLASSICAL;
    char clvar[33]="";
    if(opt_value(c->options,"cluster",clvar,sizeof clvar)){ se_kind=SE_CLUSTER; }
    else if(opt_present(c->options,"robust")||opt_present(c->options,"vce")){
        char vce[64]=""; opt_value(c->options,"vce",vce,sizeof vce);
        if(vce[0]==0 || !strcmp(vce,"robust") || !strcmp(vce,"hc1")) se_kind=SE_ROBUST;
        else if(!strncmp(vce,"cluster",7)){ se_kind=SE_CLUSTER;
            char *p=vce+7; while(*p==' ')p++; snprintf(clvar,33,"%s",p); }
        else se_kind=SE_ROBUST;
    }

    /* Split varlist into depvar + regressor list (paren-aware first token). */
    char vlist[2048]; snprintf(vlist, sizeof vlist, "%s", c->varlist);
    char *sp=NULL; char *dep=strtok_r(vlist," \t",&sp);
    if(!dep){ fprintf(stderr,"xtreg: depvar required\n"); return 198; }
    char xspec[2048]=""; char *rest=strtok_r(NULL,"",&sp);
    if(rest) snprintf(xspec, sizeof xspec, "%s", rest);

    /* Resolve via the shared TS-op expander (works for L.y, L(1/2).x, etc). */
    int *dep_idx = NULL; int n_temps = 0; const char *vlerr = NULL;
    int n_dep = tsop_expand_varlist(c->f, dep, &dep_idx, &n_temps, &vlerr);
    if(n_dep < 0){
        fprintf(stderr,"xtreg: %s\n", vlerr ? vlerr : "depvar not found");
        return 198;
    }
    if(n_dep != 1){
        fprintf(stderr,"xtreg: depvar must resolve to one variable\n");
        free(dep_idx); tsop_drop_temps(c->f, n_temps);
        return 198;
    }
    int yi = dep_idx[0];
    free(dep_idx);

    int *xi = NULL; int nx = 0; int xtemps = 0;
    if(xspec[0]){
        nx = tsop_expand_varlist(c->f, xspec, &xi, &xtemps, &vlerr);
        if(nx < 0){
            fprintf(stderr,"xtreg: %s\n", vlerr ? vlerr : "regressors not found");
            tsop_drop_temps(c->f, n_temps);
            return 198;
        }
        n_temps += xtemps;
    }
    if(nx < 1){
        fprintf(stderr,"xtreg: at least one regressor required\n");
        free(xi); tsop_drop_temps(c->f, n_temps); return 198;
    }

    /* Build the design (no constant — within transform absorbs it). */
    Design D;
    const char *err = NULL;
    if(build_design(c->f, yi, xi, nx, c->ifexp[0]?c->ifexp:NULL,
                    c->in_lo, c->in_hi, c->wexp[0]?c->wexp:NULL, c->wtype,
                    /*add_cons=*/false, &D, &err)){
        fprintf(stderr,"xtreg: %s\n", err?err:"build failed");
        free(xi); tsop_drop_temps(c->f, n_temps); return 198;
    }
    free(xi);

    /* Identify the panel id of each kept row.  We hash strings into the
     * same double key the eval module uses (so behaviour is consistent
     * across xtset/L.x). */
    Variable *pv = &c->f->vars[c->f->ts_panel];
    bool pv_str = (pv->type == VT_STR);
    double *pid = malloc(D.N * sizeof(double));
    {
        long row = 0;
        for(size_t i=0; i<D.nobs_full; i++){
            if(!D.used[i]) continue;
            if(pv_str){
                const char *s = pv->str[i] ? pv->str[i] : "";
                unsigned long h = 1469598103934665603UL;
                for(; *s; s++){ h ^= (unsigned char)*s; h *= 1099511628211UL; }
                pid[row] = (double)(h & 0x1fffffffffffffUL);
            } else {
                pid[row] = pv->num[i];
            }
            row++;
        }
    }

    /* Group rows by panel id: each group is a contiguous slice in
     * row-order (xtset has already sorted by panel/time, and the survivors
     * preserve that order via D.used).  Compute panel means of y and each
     * X in one pass. */
    typedef struct { long first, last; long count;
                     double ybar; double *xbar; } Group;
    Group *grps = malloc(D.N * sizeof *grps);  /* upper bound */
    long G = 0;
    {
        long start = 0;
        while(start < D.N){
            long end = start;
            while(end+1 < D.N && pid[end+1] == pid[start]) end++;
            grps[G].first = start;
            grps[G].last  = end;
            grps[G].count = end - start + 1;
            grps[G].xbar  = calloc(D.K, sizeof(double));
            double ys = 0;
            for(long i=start; i<=end; i++) ys += D.y[i];
            grps[G].ybar = ys / grps[G].count;
            for(int j=0; j<D.K; j++){
                double xs = 0;
                for(long i=start; i<=end; i++) xs += D.X[(size_t)j*D.N + i];
                grps[G].xbar[j] = xs / grps[G].count;
            }
            G++;
            start = end + 1;
        }
    }

    if(G < 2){
        fprintf(stderr,"xtreg: need at least 2 panels\n");
        for(long g=0; g<G; g++) free(grps[g].xbar);
        free(grps); free(pid);
        free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
        tsop_drop_temps(c->f, n_temps); return 198;
    }
    long K_slopes = D.K;
    long df_r = D.N - G - K_slopes;
    if(df_r <= 0){
        fprintf(stderr,"xtreg: too few obs (%ld) for %ld panels and %ld regressors\n",
                D.N, G, K_slopes);
        for(long g=0; g<G; g++) free(grps[g].xbar);
        free(grps); free(pid);
        free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
        tsop_drop_temps(c->f, n_temps); return 198;
    }

    /* ===== between-effects branch =====
     *
     * Collapse to G panel means and run OLS on those.  Each panel
     * contributes one observation: (ȳ_i, x̄_i,1, ..., x̄_i,K).  The model
     * is:  ȳ_i = α + Σ β_k x̄_i,k + u_i  where var(u_i) = σ²_u + σ²_e/T_i.
     *
     * v1.0 uses simple OLS on the panel means without the T_i correction
     * (which would be WLS using 1/T_i weights or a Swamy-Arora-style
     * variance adjustment).  This matches Stata's xtreg, be default.
     *
     * Output: K_slopes coefficients (incl. _cons) with classical OLS SEs
     * computed on G obs and σ²_be = RSS/(G - K_slopes).
     */
    if(mode_be){
        long G_obs = G;
        int K_orig = (int)D.K;
        int K = K_orig + 1;       /* add _cons column */
        long df_be = G_obs - K;
        if(df_be <= 0){
            fprintf(stderr,"xtreg, be: too few panels (%ld) for %d regressors\n",
                    G_obs, K);
            for(long g=0; g<G; g++) free(grps[g].xbar);
            free(grps); free(pid);
            free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
            tsop_drop_temps(c->f, n_temps); return 198;
        }
        /* Build G_obs × K matrix of panel means (column-major).  Last
         * column is _cons (all 1s). */
        double *Xb = malloc((size_t)G_obs * K * sizeof(double));
        double *yb = malloc(G_obs * sizeof(double));
        for(long g = 0; g < G_obs; g++){
            yb[g] = grps[g].ybar;
            for(int j = 0; j < K_orig; j++)
                Xb[(size_t)j * G_obs + g] = grps[g].xbar[j];
            Xb[(size_t)K_orig * G_obs + g] = 1.0;
        }
        /* OLS solve via dgels. */
        double *Xc = malloc((size_t)G_obs * K * sizeof(double));
        double *yc = malloc(G_obs * sizeof(double));
        memcpy(Xc, Xb, (size_t)G_obs * K * sizeof(double));
        memcpy(yc, yb, G_obs * sizeof(double));
        int info = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', G_obs, K, 1, Xc, G_obs, yc, G_obs);
        if(info){
            fprintf(stderr,"xtreg, be: solve failed (info=%d)\n", info);
            free(Xb); free(yb); free(Xc); free(yc);
            for(long g=0; g<G; g++) free(grps[g].xbar);
            free(grps); free(pid);
            free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
            tsop_drop_temps(c->f, n_temps); return 198;
        }
        double *bvec = malloc(K * sizeof(double));
        memcpy(bvec, yc, K * sizeof(double));
        free(Xc); free(yc);

        /* RSS and σ²_be = RSS / (G_obs - K). */
        double rss_be = 0;
        for(long g = 0; g < G_obs; g++){
            double xb_g = 0;
            for(int j = 0; j < K; j++) xb_g += Xb[(size_t)j*G_obs + g] * bvec[j];
            double e = yb[g] - xb_g;
            rss_be += e * e;
        }
        double sigma2_be = rss_be / df_be;
        /* TSS for R²_be */
        double ybar_be = 0;
        for(long g = 0; g < G_obs; g++) ybar_be += yb[g];
        ybar_be /= G_obs;
        double tss_be = 0;
        for(long g = 0; g < G_obs; g++){
            double d = yb[g] - ybar_be;
            tss_be += d * d;
        }
        double r2_be = (tss_be > 0) ? 1.0 - rss_be / tss_be : 0;

        /* V = σ²_be (X̄'X̄)^{-1} via Cholesky. */
        double *XtX = malloc((size_t)K*K*sizeof(double));
        for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
            double s = 0;
            for(long n = 0; n < G_obs; n++) s += Xb[(size_t)i*G_obs + n] * Xb[(size_t)jj*G_obs + n];
            XtX[(size_t)jj*K + i] = s;
        }
        double *V_be = malloc((size_t)K*K*sizeof(double));
        memcpy(V_be, XtX, (size_t)K*K*sizeof(double));
        int inv_rc = LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', K, V_be, K);
        if(inv_rc == 0) inv_rc = LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', K, V_be, K);
        if(inv_rc){
            fprintf(stderr,"xtreg, be: (X̄'X̄) singular\n");
            free(Xb); free(yb); free(bvec); free(XtX); free(V_be);
            for(long g=0; g<G; g++) free(grps[g].xbar);
            free(grps); free(pid);
            free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
            tsop_drop_temps(c->f, n_temps); return 198;
        }
        for(int i = 0; i < K; i++) for(int jj = i+1; jj < K; jj++)
            V_be[(size_t)i*K + jj] = V_be[(size_t)jj*K + i];
        for(long k = 0; k < (long)K*K; k++) V_be[k] *= sigma2_be;

        /* Stash an Estimates and print. */
        Estimates *e = est_new();
        snprintf(e->cmd, sizeof e->cmd, "xtreg");
        snprintf(e->depvar, sizeof e->depvar, "%s", c->f->vars[yi].name);
        e->K = K;
        e->xnames = malloc(K * sizeof *e->xnames);
        for(int j = 0; j < K_orig; j++)
            memcpy(e->xnames[j], D.xnames[j], 33);
        snprintf(e->xnames[K_orig], 33, "%s", "_cons");
        e->omitted = calloc(K, sizeof(int));
        e->b = bvec;
        e->V = V_be;
        e->N = D.N;
        e->df_r = (int)df_be;
        e->df_m = K - 1;     /* assume intercept */
        e->has_cons = 1;
        e->r2_b = r2_be;     /* between R² */
        e->r2 = r2_be;
        e->rmse = sqrt(sigma2_be);
        e->sigma2 = sigma2_be;
        e->n_groups = G_obs;
        e->se_kind = SE_CLASSICAL;
        e->nobs_at_fit = D.nobs_full;
        e->used = D.used; D.used = NULL;
        snprintf(e->fitted_frame, sizeof e->fitted_frame, "%s", c->f->name);

        est_free(c->ws->last_est);
        c->ws->last_est = e;

        if(!c->quiet){
            printf("\n");
            printf("Between regression (regression on group means)  Number of obs   = %8ld\n", D.N);
            printf("Group variable: %-20sNumber of groups = %8ld\n", c->f->vars[c->f->ts_panel].name, G_obs);
            printf("                                                R-sq: between  = %10.4f\n", r2_be);
            printf("                                                Root MSE       = %10.4f\n", e->rmse);
            printf("\n");
            printf("------------------------------------------------------------------------------\n");
            printf("%12s | Coefficient  Std. err.      t    P>|t|     [95%% conf. interval]\n", e->depvar);
            printf("-------------+----------------------------------------------------------------\n");
            double tcrit = tea_invttail(df_be, 0.025);
            for(int k = 0; k < K; k++){
                double v = V_be[(size_t)k*K + k];
                double se = v > 0 ? sqrt(v) : 0;
                double t = se > 0 ? bvec[k]/se : 0;
                double pv_be = se > 0 ? tea_pval_t(t, df_be) : 1.0;
                double lo = bvec[k] - tcrit*se;
                double hi = bvec[k] + tcrit*se;
                printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
                       e->xnames[k], bvec[k], se, t, pv_be, lo, hi);
            }
            printf("------------------------------------------------------------------------------\n");
        }

        /* macros */
        char bb[32];
        snprintf(bb,sizeof bb,"%ld",D.N); mac_set(&c->ip->rret,"e(N)",bb);
        snprintf(bb,sizeof bb,"%ld",G_obs); mac_set(&c->ip->rret,"e(N_g)",bb);
        snprintf(bb,sizeof bb,"%.10g",r2_be); mac_set(&c->ip->rret,"e(r2_b)",bb);
        store_coef_macros(e, &c->ip->rret);

        free(Xb); free(yb); free(XtX);
        for(long g=0; g<G; g++) free(grps[g].xbar);
        free(grps); free(pid);
        free(D.y); free(D.X); free(D.w); free(D.xnames);
        tsop_drop_temps(c->f, n_temps);
        return 0;
    }
    /* ===== end between-effects branch ===== */

    /* Within-transform y and X.  Keep originals: we need them later for
     * R-between (regression on panel means) and R-overall (correlation
     * of fitted vs actual on un-transformed data). */
    double *yw = malloc(D.N * sizeof(double));
    double *Xw = malloc((size_t)D.N * D.K * sizeof(double));
    for(long g=0; g<G; g++){
        for(long i=grps[g].first; i<=grps[g].last; i++){
            yw[i] = D.y[i] - grps[g].ybar;
            for(int j=0; j<D.K; j++)
                Xw[(size_t)j*D.N + i] = D.X[(size_t)j*D.N + i] - grps[g].xbar[j];
        }
    }

    /* OLS on the within-transformed data. */
    double *b = calloc(D.K, sizeof(double));
    double *resid = malloc(D.N * sizeof(double));
    int *omitted = calloc(D.K, sizeof(int));
    int rank = 0;
    int rc = ols_solve_drop(Xw, yw, D.N, D.K, b, resid, omitted, &rank);
    if(rc){
        fprintf(stderr,"xtreg: numerical solve failed (LAPACK info=%d)\n", rc);
        free(yw); free(Xw); free(b); free(resid); free(omitted);
        for(long g=0; g<G; g++) free(grps[g].xbar);
        free(grps); free(pid);
        free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
        tsop_drop_temps(c->f, n_temps); return 198;
    }
    int Kr = 0; for(int j=0; j<D.K; j++) if(!omitted[j]) Kr++;

    /* RSS and TSS on the within-transformed data → R-within. */
    double rss_w = 0; for(long i=0; i<D.N; i++) rss_w += resid[i]*resid[i];
    double tss_w = 0; for(long i=0; i<D.N; i++) tss_w += yw[i]*yw[i];
    double r2_w = tss_w > 0 ? 1 - rss_w/tss_w : 0;
    double sigma_e2 = rss_w / df_r;
    double sigma_e = sqrt(sigma_e2);

    /* Variance of beta_hat.  Sandwich variants reuse the existing helpers,
     * with their finite-sample corrections.  For classical, we scale by
     * sigma_e2 — but build's df-correction was based on N-K, not
     * N-G-K, so we recompute. */
    double *V = NULL;
    long n_cluster_groups = 0;
    long *cid = NULL;
    if(se_kind == SE_CLUSTER){
        int cvi = var_find(c->f, clvar);
        if(cvi < 0){
            fprintf(stderr,"xtreg: cluster var %s not found\n", clvar);
            free(yw); free(Xw); free(b); free(resid); free(omitted);
            for(long g=0; g<G; g++) free(grps[g].xbar);
            free(grps); free(pid);
            free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
            tsop_drop_temps(c->f, n_temps); return 111;
        }
        cid = malloc(D.N * sizeof(long));
        Variable *cv = &c->f->vars[cvi];
        double *seen_d = NULL; char **seen_s = NULL;
        long row = 0;
        for(size_t i=0; i<D.nobs_full; i++){
            if(!D.used[i]) continue;
            long g=-1;
            if(cv->type == VT_NUM){ double v=cv->num[i];
                for(long k=0; k<n_cluster_groups; k++) if(seen_d[k]==v){ g=k; break; }
                if(g<0){ seen_d = realloc(seen_d,(n_cluster_groups+1)*sizeof(double));
                         seen_d[n_cluster_groups]=v; g=n_cluster_groups++; }
            } else { const char *v=cv->str[i] ? cv->str[i] : "";
                for(long k=0; k<n_cluster_groups; k++) if(!strcmp(seen_s[k],v)){ g=k; break; }
                if(g<0){ seen_s = realloc(seen_s,(n_cluster_groups+1)*sizeof(char*));
                         seen_s[n_cluster_groups]=strdup(v); g=n_cluster_groups++; }
            }
            cid[row++] = g;
        }
        free(seen_d);
        if(seen_s){ for(long k=0; k<n_cluster_groups; k++) free(seen_s[k]); free(seen_s); }
        V = cluster_V(Xw, D.N, D.K, omitted, resid, cid, n_cluster_groups);
    } else if(se_kind == SE_ROBUST){
        V = robust_V(Xw, D.N, D.K, omitted, resid);
    } else {
        V = compute_XtXinv(Xw, D.N, D.K, omitted);
        for(int k=0; k<D.K*D.K; k++) V[k] *= sigma_e2;
    }

    /* R-between: regress panel means of y on panel means of X.  Simple
     * OLS on G observations.  We add a constant in this auxiliary
     * regression. */
    double r2_b = 0;
    {
        double *yg = malloc(G * sizeof(double));
        double *Xg = malloc((size_t)G * (D.K+1) * sizeof(double));   /* +1 for constant */
        for(long g=0; g<G; g++){
            yg[g] = grps[g].ybar;
            for(int j=0; j<D.K; j++) Xg[(size_t)j*G + g] = grps[g].xbar[j];
            Xg[(size_t)D.K*G + g] = 1.0;
        }
        if(G > D.K + 1){
            double *bg = calloc(D.K+1, sizeof(double));
            double *eg = malloc(G * sizeof(double));
            int *og = calloc(D.K+1, sizeof(int));
            int rkg=0;
            if(ols_solve_drop(Xg, yg, G, D.K+1, bg, eg, og, &rkg) == 0){
                double ssg=0, tssg=0;
                double ybar_g=0; for(long g=0; g<G; g++) ybar_g += grps[g].ybar; ybar_g /= G;
                for(long g=0; g<G; g++){
                    ssg += eg[g]*eg[g];
                    double d = grps[g].ybar - ybar_g; tssg += d*d;
                }
                r2_b = tssg>0 ? 1 - ssg/tssg : 0;
            }
            free(bg); free(eg); free(og);
        }
        free(yg); free(Xg);
    }

    /* R-overall and F-test: regress y_actual on Xb_hat (correlation²).
     * Also compute corr(u_i, X_i β) — Stata reports this as
     * "corr(u_i, Xb)". */
    double r2_o = 0, corr_uXb = 0;
    {
        /* fitted xbeta on original (un-transformed) X */
        double *xb = malloc(D.N * sizeof(double));
        for(long i=0; i<D.N; i++){
            double s = 0;
            for(int j=0; j<D.K; j++) if(!omitted[j]) s += D.X[(size_t)j*D.N + i] * b[j];
            xb[i] = s;
        }
        double y_m=0, xb_m=0; for(long i=0;i<D.N;i++){y_m+=D.y[i]; xb_m+=xb[i];}
        y_m/=D.N; xb_m/=D.N;
        double sxy=0, sxx=0, syy=0;
        for(long i=0;i<D.N;i++){
            double dx=xb[i]-xb_m, dy=D.y[i]-y_m;
            sxy+=dx*dy; sxx+=dx*dx; syy+=dy*dy;
        }
        if(sxx>0 && syy>0){ double r = sxy/sqrt(sxx*syy); r2_o = r*r; }
        /* u_i estimate per panel: alpha_i = ybar_i - xbar_i' b */
        double sum_alpha=0;
        double *alpha = malloc(G * sizeof(double));
        for(long g=0; g<G; g++){
            double a = grps[g].ybar;
            for(int j=0; j<D.K; j++) if(!omitted[j]) a -= grps[g].xbar[j] * b[j];
            alpha[g] = a;
            sum_alpha += a;
        }
        double alpha_m = sum_alpha / G;
        /* xbar_i' b averaged at panel level; correlate with alpha_i */
        double xbsum=0;
        double *xbar_b = malloc(G * sizeof(double));
        for(long g=0; g<G; g++){
            double s = 0;
            for(int j=0; j<D.K; j++) if(!omitted[j]) s += grps[g].xbar[j] * b[j];
            xbar_b[g] = s; xbsum += s;
        }
        double xb_m_g = xbsum / G;
        double cuv=0, cuu=0, cvv=0;
        for(long g=0; g<G; g++){
            double du=alpha[g]-alpha_m, dv=xbar_b[g]-xb_m_g;
            cuv+=du*dv; cuu+=du*du; cvv+=dv*dv;
        }
        if(cuu>0 && cvv>0) corr_uXb = cuv / sqrt(cuu*cvv);
        free(alpha); free(xbar_b); free(xb);
    }

    /* Variance components (Stata reports sigma_u, sigma_e, rho).
     * sigma_alpha² = variance of estimated panel intercepts across panels.
     * Then sigma_u² = max(0, sigma_alpha² - sigma_e²/T_bar).  */
    double sigma_u = 0, rho = 0;
    {
        double sum_alpha = 0, sumT = 0;
        double *alpha = malloc(G * sizeof(double));
        for(long g=0; g<G; g++){
            double a = grps[g].ybar;
            for(int j=0; j<D.K; j++) if(!omitted[j]) a -= grps[g].xbar[j] * b[j];
            alpha[g] = a;
            sum_alpha += a; sumT += grps[g].count;
        }
        double abar = sum_alpha / G;
        double svar = 0;
        for(long g=0; g<G; g++){ double d = alpha[g]-abar; svar += d*d; }
        svar /= (G > 1 ? G - 1 : 1);
        double Tbar = sumT / G;
        double su2 = svar - sigma_e2 / Tbar;
        sigma_u = su2 > 0 ? sqrt(su2) : 0;
        rho = (sigma_u*sigma_u + sigma_e*sigma_e > 0)
              ? sigma_u*sigma_u / (sigma_u*sigma_u + sigma_e*sigma_e) : 0;
        free(alpha);
    }

    /* Model F: classical = (R²_w/dfm) / ((1-R²_w)/df_r) when has_cons-like
     * structure; for the within model the constant is absorbed so use the
     * within R². */
    int df_m = Kr;
    double F=0, F_p=1;
    if(se_kind == SE_CLASSICAL){
        F = (r2_w/df_m) / ((1 - r2_w)/(double)df_r);
        F_p = tea_pval_f(F, df_m, df_r);
    } else {
        /* Wald F for sandwich variants: b' V^{-1} b / dfm */
        double *bs = malloc(Kr * sizeof(double));
        double *Vs = malloc((size_t)Kr * Kr * sizeof(double));
        int idx_map[64]; int sn=0;
        for(int j=0; j<D.K; j++) if(!omitted[j]) idx_map[sn++] = j;
        for(int i=0;i<sn;i++){ bs[i]=b[idx_map[i]];
            for(int j=0;j<sn;j++) Vs[(size_t)i*sn+j] = V[(size_t)idx_map[i]*D.K + idx_map[j]]; }
        mat_inv_sym(Vs, sn);
        double q=0;
        for(int i=0;i<sn;i++) for(int j=0;j<sn;j++) q += bs[i]*Vs[(size_t)i*sn+j]*bs[j];
        F = q/sn; df_m = sn;
        F_p = tea_pval_f(F, df_m, df_r);
        free(bs); free(Vs);
    }

    /* F-test that all u_i = 0: this compares the FE within model to a
     * pooled model.  Use the standard F = (RSS_p - RSS_w)/((G-1)) /
     * (RSS_w/(N-G-K)).  RSS_p is the pooled-OLS residual sum of squares
     * on the same y,X (with constant). */
    double F_u = 0, F_u_p = 1;
    {
        /* pooled OLS with constant for RSS_p */
        long Kp = D.K + 1;
        double *Xp = malloc((size_t)D.N * Kp * sizeof(double));
        for(int j=0; j<D.K; j++)
            memcpy(Xp + (size_t)j*D.N, D.X + (size_t)j*D.N, D.N*sizeof(double));
        for(long i=0; i<D.N; i++) Xp[(size_t)D.K*D.N + i] = 1.0;
        double *bp = calloc(Kp, sizeof(double));
        double *ep = malloc(D.N * sizeof(double));
        int *op = calloc(Kp, sizeof(int)); int rkp=0;
        if(ols_solve_drop(Xp, D.y, D.N, Kp, bp, ep, op, &rkp) == 0){
            double rss_p=0; for(long i=0;i<D.N;i++) rss_p += ep[i]*ep[i];
            if(rss_w > 0 && G > 1){
                F_u = ((rss_p - rss_w) / (double)(G - 1)) / (rss_w / (double)df_r);
                if(F_u < 0) F_u = 0;
                F_u_p = tea_pval_f(F_u, G-1, df_r);
            }
        }
        free(Xp); free(bp); free(ep); free(op);
    }

    /* T_min, T_max, T_avg */
    long T_min = grps[0].count, T_max = grps[0].count;
    double Tsum = 0;
    for(long g=0; g<G; g++){
        if(grps[g].count < T_min) T_min = grps[g].count;
        if(grps[g].count > T_max) T_max = grps[g].count;
        Tsum += grps[g].count;
    }
    double T_avg = Tsum / G;

    /* --- Output variables: default to FE, overridden by RE branch below. */
    int K_out = D.K;
    double *b_out = b;
    double *V_out = V;
    int *omitted_out = omitted;
    char (*xnames_out)[33] = NULL;
    {
        xnames_out = malloc(D.K * sizeof *xnames_out);
        memcpy(xnames_out, D.xnames, D.K * sizeof *xnames_out);
    }
    int df_r_out = (int)df_r;
    int df_m_out = df_m;
    double r2_w_out = r2_w;
    double r2_b_out = r2_b;
    double r2_o_out = r2_o;
    double F_out = F, F_p_out = F_p;
    double sigma2_out = sigma_e2;
    double theta_min = 0, theta_max = 0, theta_avg = 0;
    int has_cons_out = 0;

    /* ===== Random Effects (Swamy-Arora variant) =====
     *
     * Model: y_it = α_i + x_it' β + ε_it, with α_i ~ (0, σ_u²) iid and
     *        α_i ⊥ x_it (the RE assumption).
     *
     * Feasible GLS via quasi-demeaning:
     *   y*_it  = y_it - θ_i · ȳ_i
     *   x*_it  = x_it - θ_i · x̄_i
     *   c*_i   = 1 - θ_i        (the quasi-demeaned constant column)
     *   θ_i    = 1 - σ_e / √(T_i · σ_u² + σ_e²)
     *
     * OLS on (y*, X*, c*) gives β̂_RE.  σ_e² is taken from the within
     * regression (already computed above); σ_u² is computed from the
     * variance components above (max(0, var(α_i) - σ_e²/T̄)).
     *
     * For balanced panels θ is the same across i.  For unbalanced it
     * varies, and we report min/avg/max θ in the header. */
    if(mode_re){
        int K_re = D.K + 1;

        /* θ per panel from the FE-derived σ_u, σ_e. */
        double *theta = malloc(G * sizeof(double));
        theta_min = 1; theta_max = 0; double theta_sum = 0;
        for(long g=0; g<G; g++){
            double denom2 = (double)grps[g].count * sigma_u*sigma_u + sigma_e*sigma_e;
            theta[g] = denom2 > 0 ? 1.0 - sigma_e / sqrt(denom2) : 0.0;
            if(theta[g] < theta_min) theta_min = theta[g];
            if(theta[g] > theta_max) theta_max = theta[g];
            theta_sum += theta[g];
        }
        theta_avg = theta_sum / G;

        /* Build quasi-demeaned (y*, X*) with an explicit constant column. */
        double *y_re = malloc(D.N * sizeof(double));
        double *X_re = malloc((size_t)D.N * K_re * sizeof(double));
        for(long g=0; g<G; g++){
            double th = theta[g];
            for(long i=grps[g].first; i<=grps[g].last; i++){
                y_re[i] = D.y[i] - th * grps[g].ybar;
                for(int j=0; j<D.K; j++)
                    X_re[(size_t)j*D.N + i] = D.X[(size_t)j*D.N + i] - th * grps[g].xbar[j];
                X_re[(size_t)D.K*D.N + i] = 1.0 - th;
            }
        }

        /* OLS on quasi-demeaned data. */
        double *b_re = calloc(K_re, sizeof(double));
        double *resid_re = malloc(D.N * sizeof(double));
        int *omitted_re = calloc(K_re, sizeof(int));
        int rank_re = 0;
        int rc_re = ols_solve_drop(X_re, y_re, D.N, K_re, b_re, resid_re, omitted_re, &rank_re);
        if(rc_re){
            fprintf(stderr,"xtreg, re: numerical solve failed (LAPACK info=%d)\n", rc_re);
            free(theta); free(y_re); free(X_re); free(b_re); free(resid_re); free(omitted_re);
            free(xnames_out); free(b); free(V); free(omitted);
            for(long g=0; g<G; g++) free(grps[g].xbar);
            free(grps); free(pid); free(yw); free(Xw); free(resid); free(cid);
            free(D.y); free(D.X); free(D.w); free(D.xnames); free(D.used);
            tsop_drop_temps(c->f, n_temps); return 198;
        }

        /* Residual variance for RE (df = N - K - 1, no -G correction). */
        double rss_re = 0;
        for(long i=0; i<D.N; i++) rss_re += resid_re[i]*resid_re[i];
        int df_r_re = (int)(D.N - K_re);
        double sigma_re2 = df_r_re > 0 ? rss_re / df_r_re : 0;

        /* Variance matrix according to vce selection. */
        double *V_re = NULL;
        if(se_kind == SE_CLUSTER && cid){
            V_re = cluster_V(X_re, D.N, K_re, omitted_re, resid_re, cid, n_cluster_groups);
        } else if(se_kind == SE_ROBUST){
            V_re = robust_V(X_re, D.N, K_re, omitted_re, resid_re);
        } else {
            V_re = compute_XtXinv(X_re, D.N, K_re, omitted_re);
            for(long k=0; k<(long)K_re*K_re; k++) V_re[k] *= sigma_re2;
        }

        /* RE-specific R² values (apply β̂_RE on differently-transformed data). */
        double r2_o_re = 0, r2_w_re = 0, r2_b_re = 0;
        double cons_re = b_re[D.K];
        /* R-overall */
        {
            double *xb = malloc(D.N * sizeof(double));
            for(long i=0; i<D.N; i++){
                double s = cons_re;
                for(int j=0; j<D.K; j++) if(!omitted_re[j]) s += D.X[(size_t)j*D.N + i] * b_re[j];
                xb[i] = s;
            }
            double y_m=0, xb_m=0;
            for(long i=0; i<D.N; i++){ y_m += D.y[i]; xb_m += xb[i]; }
            y_m /= D.N; xb_m /= D.N;
            double sxy=0, sxx=0, syy=0;
            for(long i=0; i<D.N; i++){
                double dx=xb[i]-xb_m, dy=D.y[i]-y_m;
                sxy += dx*dy; sxx += dx*dx; syy += dy*dy;
            }
            if(sxx>0 && syy>0){ double r = sxy/sqrt(sxx*syy); r2_o_re = r*r; }
            free(xb);
        }
        /* R-within: apply β_RE slopes on within-transformed X vs within y */
        {
            double sxy=0, sxx=0, syy=0;
            for(long g=0; g<G; g++){
                for(long i=grps[g].first; i<=grps[g].last; i++){
                    double yw_i = D.y[i] - grps[g].ybar;
                    double xb_w = 0;
                    for(int j=0; j<D.K; j++) if(!omitted_re[j])
                        xb_w += (D.X[(size_t)j*D.N + i] - grps[g].xbar[j]) * b_re[j];
                    sxy += xb_w * yw_i; sxx += xb_w*xb_w; syy += yw_i*yw_i;
                }
            }
            if(sxx>0 && syy>0){ double r = sxy/sqrt(sxx*syy); r2_w_re = r*r; }
        }
        /* R-between: apply β_RE on panel means */
        {
            double yb_m=0, xb_b_m=0;
            for(long g=0; g<G; g++) yb_m += grps[g].ybar;
            yb_m /= G;
            double *xb_b = malloc(G * sizeof(double));
            for(long g=0; g<G; g++){
                double s = cons_re;
                for(int j=0; j<D.K; j++) if(!omitted_re[j]) s += grps[g].xbar[j] * b_re[j];
                xb_b[g] = s; xb_b_m += s;
            }
            xb_b_m /= G;
            double sxy=0, sxx=0, syy=0;
            for(long g=0; g<G; g++){
                double dx = xb_b[g]-xb_b_m, dy = grps[g].ybar - yb_m;
                sxy += dx*dy; sxx += dx*dx; syy += dy*dy;
            }
            if(sxx>0 && syy>0){ double r = sxy/sqrt(sxx*syy); r2_b_re = r*r; }
            free(xb_b);
        }

        /* Wald F for the slopes (excludes _cons). */
        double F_re = 0, F_p_re = 1;
        int Kr_re_slope = 0;
        for(int j=0; j<D.K; j++) if(!omitted_re[j]) Kr_re_slope++;
        if(Kr_re_slope > 0){
            double *bs = malloc(Kr_re_slope * sizeof(double));
            double *Vs = malloc((size_t)Kr_re_slope*Kr_re_slope * sizeof(double));
            int idx_map[64]; int sn=0;
            for(int j=0; j<D.K; j++) if(!omitted_re[j]) idx_map[sn++] = j;
            for(int i=0;i<sn;i++){ bs[i]=b_re[idx_map[i]];
                for(int j=0;j<sn;j++) Vs[(size_t)i*sn+j] = V_re[(size_t)idx_map[i]*K_re + idx_map[j]]; }
            mat_inv_sym(Vs, sn);
            double q = 0;
            for(int i=0;i<sn;i++) for(int j=0;j<sn;j++) q += bs[i]*Vs[(size_t)i*sn+j]*bs[j];
            F_re = q / sn;
            F_p_re = tea_pval_f(F_re, sn, df_r_re);
            free(bs); free(Vs);
        }

        /* xnames includes _cons at index D.K. */
        char (*xnames_re)[33] = malloc(K_re * sizeof *xnames_re);
        memcpy(xnames_re, D.xnames, D.K * sizeof *xnames_re);
        snprintf(xnames_re[D.K], 33, "_cons");

        /* Free FE-specific outputs; substitute RE-specific ones. */
        free(b); free(V); free(omitted); free(xnames_out);
        b_out = b_re; V_out = V_re; omitted_out = omitted_re;
        xnames_out = xnames_re;
        K_out = K_re;
        df_r_out = df_r_re;
        df_m_out = Kr_re_slope;
        r2_w_out = r2_w_re;
        r2_b_out = r2_b_re;
        r2_o_out = r2_o_re;
        F_out = F_re; F_p_out = F_p_re;
        sigma2_out = sigma_re2;
        has_cons_out = 1;

        free(theta); free(resid_re); free(y_re); free(X_re);
    }

    /* Populate Estimates and print. */
    Estimates *e = est_new();
    snprintf(e->cmd, sizeof e->cmd, "xtreg");
    snprintf(e->depvar, sizeof e->depvar, "%s", c->f->vars[yi].name);
    e->K = K_out;
    e->xnames = xnames_out;       /* ownership transferred */
    e->omitted = omitted_out;
    e->b = b_out; e->V = V_out;
    e->N = D.N; e->df_r = df_r_out; e->df_m = df_m_out; e->has_cons = has_cons_out;
    e->r2 = r2_w_out; e->r2_a = 0; e->rmse = sqrt(sigma2_out);
    e->F = F_out; e->F_p = F_p_out;
    e->tss = tss_w; e->rss = rss_w; e->mss = tss_w - rss_w;
    e->sigma2 = sigma2_out;
    e->se_kind = se_kind;
    snprintf(e->cluster_var, sizeof e->cluster_var, "%s", clvar);
    e->n_clusters = n_cluster_groups;
    e->nobs_at_fit = D.nobs_full;
    e->used = D.used; D.used = NULL;
    snprintf(e->fitted_frame, sizeof e->fitted_frame, "%s", c->f->name);
    /* xtreg-specific */
    e->n_groups = G; e->T_min = T_min; e->T_max = T_max; e->T_avg = T_avg;
    e->r2_w = r2_w_out; e->r2_b = r2_b_out; e->r2_o = r2_o_out;
    e->sigma_u = sigma_u; e->sigma_e = sigma_e; e->rho = rho;
    e->F_u = F_u; e->F_u_p = F_u_p;
    e->corr_u_Xb = mode_re ? 0.0 : corr_uXb;  /* RE assumes corr(u,X)=0 */

    est_free(c->ws->last_est);
    c->ws->last_est = e;

    /* Also save a clone into the FE or RE slot so `hausman` can find both
     * estimates afterward (the user runs xtreg ,fe then xtreg ,re, then
     * just types `hausman`). */
    if(mode_re){
        est_free(c->ws->re_est);
        c->ws->re_est = est_clone(e);
    } else {
        est_free(c->ws->fe_est);
        c->ws->fe_est = est_clone(e);
    }

    /* Print the xtreg fe/re table.  Layout follows Stata closely. */
    if(!c->quiet){
        const char *header = mode_re ? "Random-effects GLS regression"
                                     : "Fixed-effects (within) regression";
        printf("\n");
        printf("%-48sNumber of obs   = %8ld\n", header, D.N);
        printf("Group variable: %-15s                 Number of groups = %8ld\n",
               pv->name, G);
        printf("\n");
        printf("R-sq:                                           Obs per group:\n");
        printf("     within  = %.4f                                  min = %8ld\n", r2_w_out, T_min);
        printf("     between = %.4f                                  avg = %8.1f\n", r2_b_out, T_avg);
        printf("     overall = %.4f                                  max = %8ld\n", r2_o_out, T_max);
        printf("\n");
        if(mode_re){
            /* For RE, Stata reports a Wald χ² rather than F; we report
             * F (asymptotically equivalent) for consistency. */
            double wald = F_out * df_m_out;
            if(!isfinite(wald) || wald > 1e15){
                printf("                                                Wald chi2(%d)  = %10s\n",
                       df_m_out, "inf");
            } else {
                printf("                                                Wald chi2(%d)  = %10.2f\n",
                       df_m_out, wald);
            }
            printf("corr(u_i, X)   = 0 (assumed)                    Prob > chi2   = %10.4f\n",
                   F_p_out);
        } else {
            if(!isfinite(F_out) || F_out > 1e15){
                printf("                                                F(%d, %d) = %10s\n",
                       df_m_out, df_r_out, "inf");
            } else {
                printf("                                                F(%d, %d) = %10.2f\n",
                       df_m_out, df_r_out, F_out);
            }
            printf("corr(u_i, Xb)  = %-8.4f                       Prob > F      = %10.4f\n",
                   corr_uXb, F_p_out);
        }
        printf("\n");
        const char *selab = se_kind==SE_ROBUST?"Robust":
                            se_kind==SE_CLUSTER?"Cluster":"Std. err.";
        printf("------------------------------------------------------------------------------\n");
        printf("%12s | Coefficient  %-10s    t    P>|t|     [95%% conf. interval]\n",
               e->depvar, selab);
        printf("-------------+----------------------------------------------------------------\n");
        double tcrit = tea_invt(0.975, (double)df_r_out);
        for(int i=0; i<K_out; i++){
            if(omitted_out[i]){
                printf("%12s | %10s  (omitted)\n", xnames_out[i], "0");
                continue;
            }
            double se = sqrt(V_out[(size_t)i*K_out + i]);
            double t = se>0 ? b_out[i]/se : 0;
            double p = se>0 ? tea_pval_t(t, (double)df_r_out) : 1.0;
            double lo = b_out[i] - tcrit*se;
            double hi = b_out[i] + tcrit*se;
            printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
                   xnames_out[i], b_out[i], se, t, p, lo, hi);
        }
        printf("-------------+----------------------------------------------------------------\n");
        printf("%12s | %10.6g   %s\n", "sigma_u", sigma_u, "");
        printf("%12s | %10.6g   %s\n", "sigma_e", sigma_e, "");
        printf("%12s | %10.6g   (fraction of variance due to u_i)\n", "rho", rho);
        if(mode_re){
            if(T_min == T_max){
                printf("%12s | %10.6g   (quasi-demeaning factor)\n", "theta", theta_avg);
            } else {
                printf("%12s | min=%.4f  avg=%.4f  max=%.4f\n",
                       "theta", theta_min, theta_avg, theta_max);
            }
        }
        printf("------------------------------------------------------------------------------\n");
        if(!mode_re && F_u > 0){
            printf("F test that all u_i=0:  F(%ld, %ld) = %10.2f      Prob > F = %.4f\n",
                   G-1, (long)df_r_out, F_u, F_u_p);
        }
        if(se_kind == SE_CLUSTER){
            printf("(Std. err. adjusted for %ld clusters in %s)\n", n_cluster_groups, clvar);
        }
    }

    /* r() macros for downstream commands */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",e->N); mac_set(&c->ip->rret,"e(N)",bb);
    snprintf(bb,sizeof bb,"%ld",G); mac_set(&c->ip->rret,"e(N_g)",bb);
    snprintf(bb,sizeof bb,"%.10g",r2_w_out); mac_set(&c->ip->rret,"e(r2_w)",bb);
    snprintf(bb,sizeof bb,"%.10g",r2_b_out); mac_set(&c->ip->rret,"e(r2_b)",bb);
    snprintf(bb,sizeof bb,"%.10g",r2_o_out); mac_set(&c->ip->rret,"e(r2_o)",bb);
    snprintf(bb,sizeof bb,"%.10g",sigma_u); mac_set(&c->ip->rret,"e(sigma_u)",bb);
    snprintf(bb,sizeof bb,"%.10g",sigma_e); mac_set(&c->ip->rret,"e(sigma_e)",bb);
    snprintf(bb,sizeof bb,"%.10g",rho); mac_set(&c->ip->rret,"e(rho)",bb);
    if(mode_re){
        snprintf(bb,sizeof bb,"%.10g",theta_avg); mac_set(&c->ip->rret,"e(theta)",bb);
    }
    store_coef_macros(e, &c->ip->rret);

    /* Clean up — ownership of b_out/V_out/omitted_out/xnames_out/D.used transferred
     * to Estimates above. */
    for(long g=0; g<G; g++) free(grps[g].xbar);
    free(grps); free(pid);
    free(yw); free(Xw); free(resid); free(cid);
    free(D.y); free(D.X); free(D.w); free(D.xnames);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- hausman test ------------------------------------------------------ */
/* hausman — Hausman specification test for FE vs RE in panel models.
 *
 * Workflow:
 *     xtreg y x1 x2 ..., fe
 *     xtreg y x1 x2 ..., re
 *     hausman
 *
 * The order doesn't matter; we just need both estimates saved in the
 * workspace (which happens automatically when xtreg runs).  Aborts if
 * either is missing.
 *
 * Statistic:
 *     H = (β_FE - β_RE)' [V_FE - V_RE]^{-1} (β_FE - β_RE)
 *
 * Distributed as χ²(K_common) under H₀: cov(α_i, x_it) = 0.  Under H₀,
 * both FE and RE are consistent (RE more efficient); under H₁, only FE
 * is consistent.
 *
 * Implementation details that matter:
 *   1. Only compare slope coefficients that appear in BOTH estimates.
 *      RE has _cons, FE doesn't, so _cons is dropped from the test.
 *      Other not-in-common names (e.g. a regressor omitted as collinear
 *      in one but not the other) are also dropped.
 *   2. V_FE - V_RE is the difference of variance matrices.  Under H₀ it's
 *      positive semi-definite; the inverse uses Cholesky, which can fail
 *      if the difference is not PD (we report this as a warning and
 *      attempt a pseudo-inverse via SVD, falling back to a regularization
 *      if even that fails). */
int do_hausman(Cmd *c)
{
    Estimates *fe = c->ws->fe_est;
    Estimates *re = c->ws->re_est;
    if(!fe || !re){
        fprintf(stderr,"hausman: need both xtreg ,fe and xtreg ,re results\n");
        if(!fe) fprintf(stderr,"        (fe estimates not found — run `xtreg y x, fe` first)\n");
        if(!re) fprintf(stderr,"        (re estimates not found — run `xtreg y x, re` first)\n");
        return 301;
    }
    if(strcmp(fe->depvar, re->depvar) != 0){
        fprintf(stderr,"hausman: depvar differs between fe (%s) and re (%s)\n",
                fe->depvar, re->depvar);
        return 198;
    }
    /* Find common, non-omitted slope coefficients (exclude _cons).
     * Build a mapping common-index → (fe_idx, re_idx). */
    int *fe_map = malloc(fe->K * sizeof(int));
    int *re_map = malloc(re->K * sizeof(int));
    int nc = 0;
    for(int i=0; i<fe->K; i++){
        if(fe->omitted[i]) continue;
        if(!strcmp(fe->xnames[i], "_cons")) continue;
        int j = est_idx_of(re, fe->xnames[i]);
        if(j < 0 || re->omitted[j]) continue;
        fe_map[nc] = i;
        re_map[nc] = j;
        nc++;
    }
    if(nc < 1){
        fprintf(stderr,"hausman: no common slope coefficients between fe and re\n");
        free(fe_map); free(re_map);
        return 198;
    }

    /* db = β_FE - β_RE (nc vector); dV = V_FE - V_RE (nc × nc, sym). */
    double *db = malloc(nc * sizeof(double));
    double *dV = malloc((size_t)nc*nc * sizeof(double));
    for(int i=0; i<nc; i++){
        db[i] = fe->b[fe_map[i]] - re->b[re_map[i]];
        for(int j=0; j<nc; j++){
            double vfe = fe->V[(size_t)fe_map[i]*fe->K + fe_map[j]];
            double vre = re->V[(size_t)re_map[i]*re->K + re_map[j]];
            dV[(size_t)i*nc + j] = vfe - vre;
        }
    }

    /* Try Cholesky inverse first (works when dV is PD).  If it fails,
     * fall back to a pseudo-inverse via SVD.  This handles the common
     * situation where V_FE - V_RE is not strictly PD due to sampling
     * noise (especially with cluster/robust SE). */
    double *dVinv = malloc((size_t)nc*nc * sizeof(double));
    memcpy(dVinv, dV, (size_t)nc*nc * sizeof(double));
    int chol_info = LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', nc, dVinv, nc);
    int used_pseudo = 0;
    if(chol_info == 0){
        LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', nc, dVinv, nc);
        /* symmetrize */
        for(int i=0;i<nc;i++) for(int j=i+1;j<nc;j++)
            dVinv[(size_t)i*nc + j] = dVinv[(size_t)j*nc + i];
    } else {
        /* SVD-based pseudo-inverse: dV = U S V^T, dV^+ = V S^+ U^T.
         * For symmetric dV, U = V; S is non-negative.  Threshold tiny
         * singular values for numerical stability. */
        used_pseudo = 1;
        memcpy(dVinv, dV, (size_t)nc*nc * sizeof(double));
        double *S  = malloc(nc * sizeof(double));
        double *U  = malloc((size_t)nc*nc * sizeof(double));
        double *VT = malloc((size_t)nc*nc * sizeof(double));
        double *sb = malloc(nc * sizeof(double));
        int svd_info = LAPACKE_dgesdd(LAPACK_COL_MAJOR, 'A',
                                       nc, nc, dVinv, nc, S, U, nc, VT, nc);
        if(svd_info != 0){
            fprintf(stderr,"hausman: SVD failed (LAPACK info=%d); difference matrix may be singular\n",
                    svd_info);
            free(S); free(U); free(VT); free(sb);
            free(db); free(dV); free(dVinv); free(fe_map); free(re_map);
            return 198;
        }
        double smax = S[0]; double tol = smax * 1e-10 * nc;
        for(int i=0; i<nc; i++) sb[i] = (S[i] > tol) ? 1.0/S[i] : 0.0;
        /* dV^+ = V · diag(sb) · U^T.  In column-major LAPACKE_dgesdd,
         * VT holds V^T (so V = VT^T) and U holds U.  Compute as:
         *     M[i,j] = sum_k VT[k,i] * sb[k] * U[j,k]
         * where U is column-major (entry U[j,k] = U[k*nc + j]). */
        for(int i=0;i<nc;i++) for(int j=0;j<nc;j++){
            double s = 0;
            for(int k=0;k<nc;k++){
                s += VT[(size_t)i*nc + k] * sb[k] * U[(size_t)k*nc + j];
            }
            dVinv[(size_t)i*nc + j] = s;
        }
        free(S); free(U); free(VT); free(sb);
    }

    /* H = db' · dVinv · db */
    double H = 0;
    for(int i=0;i<nc;i++) for(int j=0;j<nc;j++)
        H += db[i] * dVinv[(size_t)i*nc + j] * db[j];

    /* Sometimes the difference matrix is not PSD and H comes out
     * slightly negative.  Stata reports the absolute value with a
     * warning; we do the same. */
    bool negative = (H < 0);
    if(negative) H = -H;

    /* p-value: upper-tail chi-square. */
    double pval = tea_pval_chi2(H, (double)nc);

    /* Print Stata-style table. */
    printf("\n");
    printf("                ---- Coefficients ----\n");
    printf("             |      (b)          (B)            (b-B)     sqrt(diag(V_b-V_B))\n");
    printf("             |      fe           re          Difference     S.E.\n");
    printf("-------------+----------------------------------------------------------------\n");
    for(int i=0; i<nc; i++){
        double vd = dV[(size_t)i*nc + i];
        double sd = vd > 0 ? sqrt(vd) : 0;
        printf("%12s | %10.6g    %10.6g    %12.6g    %10.6g%s\n",
               fe->xnames[fe_map[i]], fe->b[fe_map[i]], re->b[re_map[i]],
               db[i], sd, vd < 0 ? "  (V_b-V_B not PSD)" : "");
    }
    printf("------------------------------------------------------------------------------\n");
    printf("                  b = consistent under H0 and Ha;     obtained from xtreg, fe\n");
    printf("                  B = inconsistent under Ha, efficient under H0; obtained from xtreg, re\n");
    printf("\n");
    printf("Test: Ho:  difference in coefficients not systematic\n");
    printf("\n");
    printf("              chi2(%d) = (b-B)'[(V_b-V_B)^(-1)](b-B)\n", nc);
    printf("                       = %8.2f%s\n", H, negative ? "  (sign flipped; V difference not PSD)" : "");
    printf("            Prob > chi2 = %8.4f\n", pval);
    if(used_pseudo){
        printf("            (V_b-V_B is not positive definite; used pseudoinverse)\n");
    }
    printf("\n");

    /* r() macros */
    char b32[32];
    snprintf(b32,sizeof b32,"%.10g",H); mac_set(&c->ip->rret,"r(chi2)",b32);
    snprintf(b32,sizeof b32,"%.10g",pval); mac_set(&c->ip->rret,"r(p)",b32);
    snprintf(b32,sizeof b32,"%d",nc); mac_set(&c->ip->rret,"r(df)",b32);

    free(db); free(dV); free(dVinv); free(fe_map); free(re_map);
    return 0;
}

/* ---- logit / probit (MLE) --------------------------------------------- */
/* Shared driver for logit and probit.  The only difference is the
 * MleFamily pointer, so we route both Stata commands through this. */
static int do_glm_binary(Cmd *c, const MleFamily *fam)
{
    if(!c->varlist[0]){
        fprintf(stderr,"%s: depvar and regressors required\n", fam->name);
        return 198;
    }

    /* Parse the same options regress understands.  fweight & robust/
     * cluster work; iweight not supported by MLE; pweight forces robust
     * (matches Stata's pweight semantics). */
    bool noconst = opt_present(c->options,"noconstant") || opt_present(c->options,"nocons");
    SeKind se_kind = SE_CLASSICAL;
    char clvar[33]="";
    if(opt_value(c->options,"cluster",clvar,sizeof clvar)){ se_kind = SE_CLUSTER; }
    else if(opt_present(c->options,"robust")||opt_present(c->options,"vce")){
        char vce[64]=""; opt_value(c->options,"vce",vce,sizeof vce);
        if(vce[0]==0 || !strcmp(vce,"robust") || !strcmp(vce,"hc1")) se_kind = SE_ROBUST;
        else if(!strncmp(vce,"cluster",7)){ se_kind = SE_CLUSTER;
            char *p=vce+7; while(*p==' ')p++; snprintf(clvar,33,"%s",p); }
        else se_kind = SE_ROBUST;
    }
    if(c->wtype == 3 && se_kind == SE_CLASSICAL) se_kind = SE_ROBUST;
    if(c->wtype == 4){
        fprintf(stderr,"%s: iweight not allowed\n", fam->name);
        return 101;
    }

    /* depvar + regressors via the shared TS-op expander. */
    char vlist[2048]; snprintf(vlist, sizeof vlist, "%s", c->varlist);
    char *sp = NULL; char *dep = strtok_r(vlist, " \t", &sp);
    if(!dep){ fprintf(stderr,"%s: depvar required\n", fam->name); return 198; }
    char xspec[2048] = ""; char *rest = strtok_r(NULL, "", &sp);
    if(rest) snprintf(xspec, sizeof xspec, "%s", rest);

    int *dep_idx = NULL; int n_temps = 0; const char *vlerr = NULL;
    int n_dep = tsop_expand_varlist(c->f, dep, &dep_idx, &n_temps, &vlerr);
    if(n_dep < 0){ fprintf(stderr,"%s: %s\n", fam->name, vlerr?vlerr:"depvar not found");
        return 198; }
    if(n_dep != 1){ fprintf(stderr,"%s: depvar must resolve to one variable\n", fam->name);
        free(dep_idx); tsop_drop_temps(c->f, n_temps); return 198; }
    int yi = dep_idx[0]; free(dep_idx);

    int *xi = NULL; int nx = 0; int xtemps = 0;
    nx = tsop_expand_varlist(c->f, xspec[0]?xspec:"_all", &xi, &xtemps, &vlerr);
    if(nx < 0){ fprintf(stderr,"%s: %s\n", fam->name, vlerr?vlerr:"regressors not found");
        tsop_drop_temps(c->f, n_temps); return 198; }
    n_temps += xtemps;
    if(!xspec[0]){
        int w = 0;
        for(int j=0;j<nx;j++) if(xi[j] != yi) xi[w++] = xi[j];
        nx = w;
    }

    Design D;
    const char *err;
    if(build_design(c->f, yi, xi, nx, c->ifexp[0]?c->ifexp:NULL,
                    c->in_lo, c->in_hi, c->wexp[0]?c->wexp:NULL, c->wtype,
                    !noconst, &D, &err)){
        fprintf(stderr,"%s: %s\n", fam->name, err?err:"build failed");
        free(xi); tsop_drop_temps(c->f, n_temps); return 198;
    }
    free(xi);

    /* Fit via Newton-Raphson MLE. */
    MleFit fit;
    const char *mle_err = NULL;
    int rc = mle_newton(D.X, D.y, D.N, (int)D.K, fam, 50, 1e-7, &fit, &mle_err);
    if(rc){
        fprintf(stderr,"%s: %s\n", fam->name, mle_err?mle_err:"MLE fit failed");
        mle_fit_free(&fit);
        design_free(&D);
        tsop_drop_temps(c->f, n_temps);
        return 198;
    }
    if(!fit.converged && !c->quiet){
        fprintf(stderr,"%s: convergence not achieved in %d iterations\n",
                fam->name, fit.iterations);
    }
    if(fit.perfect_pred && !c->quiet){
        fprintf(stderr,"%s: outcome perfectly predicted; reported coefficients "
                       "and standard errors are not meaningful\n", fam->name);
    }

    /* Variance: classical = (X'WX)^{-1} (already in fit.V_classical).
     * Robust / cluster: sandwich form V = A · B · A, where A = (X'WX)^{-1}
     * and B = X' diag(s_i²) X for robust, or sum of cluster outer products
     * for cluster. */
    long Kr = 0; for(int j=0;j<D.K;j++) if(!fit.omitted[j]) Kr++;
    double *V_final = malloc((size_t)D.K*D.K*sizeof(double));
    memcpy(V_final, fit.V_classical, (size_t)D.K*D.K*sizeof(double));

    /* For robust / cluster, we need the per-observation score vector
     * s_i (the dℓ/dη term) at the converged β. */
    long n_cluster_groups = 0;
    long *cid = NULL;
    if(se_kind != SE_CLASSICAL){
        /* Recompute per-obs score at converged β. */
        double *score = malloc(D.N * sizeof(double));
        for(long i=0; i<D.N; i++){
            double s_i, w_i, l_i;
            fam->per_obs(D.y[i], fit.eta[i], &s_i, &w_i, &l_i);
            score[i] = s_i;
        }
        /* B = X' diag(s²) X  (robust)  or  sum_g (X_g' s_g)(X_g' s_g)'  (cluster) */
        double *B = calloc((size_t)D.K*D.K, sizeof(double));
        if(se_kind == SE_ROBUST){
            for(int i=0;i<D.K;i++) for(int j=0;j<D.K;j++){
                if(fit.omitted[i] || fit.omitted[j]) continue;
                double s = 0;
                for(long n=0; n<D.N; n++){
                    s += D.X[(size_t)i*D.N + n] * score[n]*score[n] * D.X[(size_t)j*D.N + n];
                }
                B[(size_t)i*D.K + j] = s;
            }
            /* HC1: scale by N/(N-K) */
            long Nl = D.N;
            double hc1 = (double)Nl / (double)(Nl - Kr);
            for(long k=0; k<(long)D.K*D.K; k++) B[k] *= hc1;
        } else {
            /* cluster: build cluster ids */
            int cvi = var_find(c->f, clvar);
            if(cvi < 0){
                fprintf(stderr,"%s: cluster var %s not found\n", fam->name, clvar);
                free(score); free(B); free(V_final);
                mle_fit_free(&fit);
                design_free(&D);
                tsop_drop_temps(c->f, n_temps);
                return 111;
            }
            cid = malloc(D.N * sizeof(long));
            Variable *cv = &c->f->vars[cvi];
            double *seen_d = NULL; char **seen_s = NULL;
            long row = 0;
            for(size_t i=0; i<D.nobs_full; i++){
                if(!D.used[i]) continue;
                long g = -1;
                if(cv->type == VT_NUM){ double v=cv->num[i];
                    for(long k=0; k<n_cluster_groups; k++) if(seen_d[k]==v){ g=k; break; }
                    if(g<0){ seen_d = realloc(seen_d,(n_cluster_groups+1)*sizeof(double));
                             seen_d[n_cluster_groups]=v; g=n_cluster_groups++; }
                } else { const char *v=cv->str[i] ? cv->str[i] : "";
                    for(long k=0; k<n_cluster_groups; k++) if(!strcmp(seen_s[k],v)){ g=k; break; }
                    if(g<0){ seen_s = realloc(seen_s,(n_cluster_groups+1)*sizeof(char*));
                             seen_s[n_cluster_groups]=strdup(v); g=n_cluster_groups++; }
                }
                cid[row++] = g;
            }
            free(seen_d);
            if(seen_s){ for(long k=0;k<n_cluster_groups;k++) free(seen_s[k]); free(seen_s); }
            /* sum of (X_g' s_g)(X_g' s_g)' over clusters */
            double *u = calloc((size_t)n_cluster_groups*D.K, sizeof(double));  /* G × K */
            for(long n=0; n<D.N; n++){
                long g = cid[n];
                for(int j=0; j<D.K; j++) if(!fit.omitted[j])
                    u[(size_t)g*D.K + j] += D.X[(size_t)j*D.N + n] * score[n];
            }
            for(int i=0; i<D.K; i++) for(int j=0; j<D.K; j++){
                if(fit.omitted[i] || fit.omitted[j]) continue;
                double s = 0;
                for(long g=0; g<n_cluster_groups; g++)
                    s += u[(size_t)g*D.K + i] * u[(size_t)g*D.K + j];
                B[(size_t)i*D.K + j] = s;
            }
            free(u);
            /* CR1 finite-sample adjustment: (G/(G-1)) · ((N-1)/(N-K)) */
            long Nl = D.N;
            double adj = (double)n_cluster_groups/(n_cluster_groups-1) *
                         (double)(Nl-1)/(Nl - Kr);
            for(long k=0; k<(long)D.K*D.K; k++) B[k] *= adj;
        }
        /* V = A B A,  A = fit.V_classical */
        double *AB = calloc((size_t)D.K*D.K, sizeof(double));
        for(int i=0;i<D.K;i++) for(int j=0;j<D.K;j++){
            double s = 0;
            for(int k=0;k<D.K;k++) s += fit.V_classical[(size_t)i*D.K + k] * B[(size_t)k*D.K + j];
            AB[(size_t)i*D.K + j] = s;
        }
        for(int i=0;i<D.K;i++) for(int j=0;j<D.K;j++){
            double s = 0;
            for(int k=0;k<D.K;k++) s += AB[(size_t)i*D.K + k] * fit.V_classical[(size_t)k*D.K + j];
            V_final[(size_t)i*D.K + j] = s;
        }
        free(AB); free(B); free(score);
    }

    /* Pseudo R² (McFadden): 1 - ll/ll_0.  Likelihood ratio test for joint
     * significance of all slopes: 2(ll - ll_0) ~ χ²(K-1 if has_cons). */
    double pseudo_r2 = (fit.loglik_0 != 0) ? 1.0 - fit.loglik / fit.loglik_0 : 0;
    double LR_chi2 = 2.0 * (fit.loglik - fit.loglik_0);
    int df_m = (int)Kr - (!noconst ? 1 : 0);
    if(df_m < 0) df_m = 0;
    double LR_p = df_m > 0 ? tea_pval_chi2(LR_chi2, df_m) : 1.0;

    /* Populate Estimates. */
    Estimates *e = est_new();
    snprintf(e->cmd, sizeof e->cmd, "%s", fam->name);
    snprintf(e->depvar, sizeof e->depvar, "%s", c->f->vars[yi].name);
    e->K = (int)D.K;
    e->xnames = malloc(D.K * sizeof *e->xnames);
    memcpy(e->xnames, D.xnames, D.K * sizeof *e->xnames);
    e->omitted = malloc(D.K * sizeof(int));
    memcpy(e->omitted, fit.omitted, D.K * sizeof(int));
    e->b = malloc(D.K * sizeof(double));
    memcpy(e->b, fit.beta, D.K * sizeof(double));
    e->V = V_final;
    e->N = D.N;
    e->df_r = (int)(D.N - Kr);          /* informational; Wald uses N for asymptotic */
    e->df_m = df_m;
    e->has_cons = !noconst;
    e->r2 = pseudo_r2; e->r2_a = 0; e->rmse = 0;
    e->F = LR_chi2; e->F_p = LR_p;        /* repurpose F/F_p for LR χ² / p */
    e->sigma2 = 0;
    e->se_kind = se_kind;
    snprintf(e->cluster_var, sizeof e->cluster_var, "%s", clvar);
    e->n_clusters = n_cluster_groups;
    e->nobs_at_fit = D.nobs_full;
    e->used = D.used; D.used = NULL;
    snprintf(e->fitted_frame, sizeof e->fitted_frame, "%s", c->f->name);

    est_free(c->ws->last_est);
    c->ws->last_est = e;

    /* Print table. */
    if(!c->quiet){
        const char *header_name =
            !strcmp(fam->name,"logit")   ? "Logistic regression" :
            !strcmp(fam->name,"probit")  ? "Probit regression"   :
            !strcmp(fam->name,"poisson") ? "Poisson regression"  : fam->name;
        printf("\n");
        printf("Iteration 0:    log likelihood = %12.6f\n", fit.loglik_0);
        printf("Iteration %d:    log likelihood = %12.6f%s\n",
               fit.iterations, fit.loglik, fit.converged ? "" : "  (not converged)");
        printf("\n");
        printf("%-48sNumber of obs   = %8ld\n", header_name, D.N);
        printf("                                                LR chi2(%d)      = %10.2f\n",
               df_m, LR_chi2);
        printf("                                                Prob > chi2     = %10.4f\n", LR_p);
        printf("Log likelihood = %-12.6f                   Pseudo R2       = %10.4f\n",
               fit.loglik, pseudo_r2);
        printf("\n");
        const char *selab = se_kind==SE_ROBUST?"Robust":
                            se_kind==SE_CLUSTER?"Cluster":"Std. err.";
        printf("------------------------------------------------------------------------------\n");
        printf("%12s | Coefficient  %-10s    z    P>|z|     [95%% conf. interval]\n",
               e->depvar, selab);
        printf("-------------+----------------------------------------------------------------\n");
        double zcrit = tea_invnormal(0.975);
        for(int j=0; j<D.K; j++){
            if(fit.omitted[j]){
                printf("%12s | %10s  (omitted)\n", D.xnames[j], "0");
                continue;
            }
            double v = V_final[(size_t)j*D.K + j];
            double se = v > 0 ? sqrt(v) : 0;
            double z = se>0 ? fit.beta[j]/se : 0;
            double p = se>0 ? 2.0 * (1.0 - tea_normal_cdf(fabs(z))) : 1.0;
            double lo = fit.beta[j] - zcrit*se;
            double hi = fit.beta[j] + zcrit*se;
            printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
                   D.xnames[j], fit.beta[j], se, z, p, lo, hi);
        }
        printf("------------------------------------------------------------------------------\n");
        if(se_kind == SE_CLUSTER){
            printf("(Std. err. adjusted for %ld clusters in %s)\n", n_cluster_groups, clvar);
        }
    }

    /* r() macros */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",e->N); mac_set(&c->ip->rret,"e(N)",bb);
    snprintf(bb,sizeof bb,"%.10g",fit.loglik); mac_set(&c->ip->rret,"e(ll)",bb);
    snprintf(bb,sizeof bb,"%.10g",fit.loglik_0); mac_set(&c->ip->rret,"e(ll_0)",bb);
    snprintf(bb,sizeof bb,"%.10g",pseudo_r2); mac_set(&c->ip->rret,"e(r2_p)",bb);
    snprintf(bb,sizeof bb,"%.10g",LR_chi2); mac_set(&c->ip->rret,"e(chi2)",bb);
    snprintf(bb,sizeof bb,"%d",df_m); mac_set(&c->ip->rret,"e(df_m)",bb);
    store_coef_macros(e, &c->ip->rret);

    mle_fit_free(&fit);
    free(cid);
    free(D.y); free(D.X); free(D.w); free(D.xnames);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

int do_logit(Cmd *c)  { return do_glm_binary(c, &mle_family_logit); }
int do_probit(Cmd *c) { return do_glm_binary(c, &mle_family_probit); }
int do_poisson(Cmd *c) { return do_glm_binary(c, &mle_family_poisson); }

/* ---- margins ----------------------------------------------------------- */
/* margins [varlist] [, dydx(varlist|*) atmeans]
 *
 * Computes marginal effects (or predicted outcomes) after regress, logit,
 * probit, or xtreg.  v1 covers the most useful cases:
 *
 *   margins, dydx(*)             AME of all continuous regressors
 *   margins, dydx(x1 x2)         AME of selected regressors
 *   margins, dydx(*) atmeans     MEM (effects at sample means)
 *   margins                      predicted outcome over the sample (just E[y])
 *
 * For each requested x_k:
 *   AME:  m_k = (1/N) Σ_i ∂E[y|X_i]/∂x_k
 *         where for OLS ∂E/∂x_k = β_k (so AME = β_k exactly, SE = SE(β_k));
 *               for logit  ∂E/∂x_k = Λ(X_iβ)(1-Λ(X_iβ)) β_k;
 *               for probit ∂E/∂x_k = φ(X_iβ) β_k.
 *   MEM:  same but evaluated only at X = mean (X-bar in the sample).
 *
 * Delta-method SE:
 *   m_k = (1/N) Σ_i g(η_i) · β_k       where η_i = X_iβ, g = Λ(1-Λ) or φ
 *   ∂m_k/∂β_j = (1/N) Σ_i [ g'(η_i) X_ij β_k + g(η_i) · 1{j=k} ]
 *   V(m_k) = G_k · V(β) · G_k'
 *
 * Derivatives:
 *   logit:   g(η) = Λ(η)(1-Λ(η));    g'(η) = Λ(η)(1-Λ(η))(1-2Λ(η))
 *   probit:  g(η) = φ(η);             g'(η) = -η · φ(η)
 *   ols:     g(η) = 1;                g'(η) = 0
 */

/* Per-observation marginal multipliers for each family.  At observation i
 * and a fixed coefficient k:
 *   contribution to point estimate: g_val * β_k
 *   contribution to gradient row:   g_deriv * X_ij * β_k  (j ≠ k)
 *                                 + g_val                 (j = k)
 * (averaged across i when computing AME). */
static void margins_g(const char *cmd, double eta, double *g_val, double *g_deriv)
{
    if(!strcmp(cmd, "regress") || !strcmp(cmd, "xtreg")){
        *g_val = 1.0;
        *g_deriv = 0.0;
    } else if(!strcmp(cmd, "logit")){
        double L;
        if(eta >= 0){ double e = exp(-eta); L = 1.0/(1.0+e); }
        else        { double e = exp(eta);  L = e/(1.0+e);    }
        *g_val   = L * (1.0 - L);
        *g_deriv = L * (1.0 - L) * (1.0 - 2.0*L);
    } else { /* probit */
        double phi = tea_normal_pdf(eta);
        *g_val   = phi;
        *g_deriv = -eta * phi;
    }
}

int do_margins(Cmd *c)
{
    Estimates *e = c->ws->last_est;
    if(!e){ fprintf(stderr,"margins: no estimates available\n"); return 301; }
    bool atmeans = opt_present(c->options, "atmeans");

    /* Parse dydx() option, or default to "*" which means all non-_cons. */
    char dydx_spec[1024] = "";
    bool has_dydx = opt_value(c->options, "dydx", dydx_spec, sizeof dydx_spec);
    if(!has_dydx){
        /* If the user provided NO option at all, default is to predict E[y]
         * over the sample (a single number).  v1: just say what we'd do. */
        if(!c->options[0] && !c->varlist[0]){
            fprintf(stderr,"margins: with no dydx() option, would compute "
                           "predicted E[y] over the sample (not yet implemented).\n"
                           "        For v1, please use `margins, dydx(*)`.\n");
            return 198;
        }
        if(!has_dydx){ has_dydx = true; snprintf(dydx_spec, sizeof dydx_spec, "*"); }
    }

    /* Resolve the requested dydx coefficients to indices in e->xnames. */
    bool *want = calloc(e->K, sizeof(bool));
    if(!strcmp(dydx_spec, "*") || !strcmp(dydx_spec, "_all")){
        for(int k=0;k<e->K;k++){
            if(e->omitted[k]) continue;
            if(!strcmp(e->xnames[k], "_cons")) continue;
            want[k] = true;
        }
    } else {
        char buf[1024]; snprintf(buf, sizeof buf, "%s", dydx_spec);
        char *sp = NULL;
        for(char *t = strtok_r(buf, " ,", &sp); t; t = strtok_r(NULL, " ,", &sp)){
            int k = est_idx_of(e, t);
            if(k < 0){ fprintf(stderr,"margins: %s not a coefficient\n", t);
                free(want); return 111; }
            if(e->omitted[k]){ fprintf(stderr,"margins: %s is omitted\n", t);
                free(want); return 198; }
            if(!strcmp(e->xnames[k], "_cons")){
                fprintf(stderr,"margins: dydx(_cons) doesn't make sense\n");
                free(want); return 198; }
            want[k] = true;
        }
    }
    int n_want = 0; for(int k=0;k<e->K;k++) if(want[k]) n_want++;
    if(n_want < 1){
        fprintf(stderr,"margins: no continuous variables to compute effects for\n");
        free(want); return 198;
    }

    /* Resolve regressor names to frame columns (with TS-op snapshotting,
     * same trick as predict).  We need to read the full X matrix at the
     * in-sample rows. */
    int *xi = malloc(e->K * sizeof(int));
    double **snap = calloc(e->K, sizeof(double*));
    int n_temps = 0;
    size_t snap_n = c->f->nobs;
    for(int j=0; j<e->K; j++){
        if(!strcmp(e->xnames[j], "_cons")){ xi[j] = -1; continue; }
        int idx = var_find(c->f, e->xnames[j]);
        if(idx >= 0){ xi[j] = idx; continue; }
        int *one = NULL; int these_temps = 0; const char *vlerr = NULL;
        int got = tsop_expand_varlist(c->f, e->xnames[j], &one, &these_temps, &vlerr);
        if(got != 1 || !one){
            fprintf(stderr,"margins: %s not in current data\n", e->xnames[j]);
            tsop_drop_temps(c->f, n_temps + these_temps);
            for(int k=0;k<j;k++) free(snap[k]);
            free(snap); free(xi); free(want); free(one); return 111;
        }
        snap[j] = malloc(snap_n * sizeof(double));
        memcpy(snap[j], c->f->vars[one[0]].num, snap_n * sizeof(double));
        xi[j] = -2;
        n_temps += these_temps;
        free(one);
    }
    tsop_drop_temps(c->f, n_temps);

    /* Helper to read X[j,i] uniformly. */
    #define XJI(j, i) ( xi[j] == -1 ? 1.0 \
                      : xi[j] == -2 ? snap[j][i] \
                      : c->f->vars[xi[j]].num[i] )

    /* Determine in-sample row indices.  If e->used is missing, fall back
     * to "all rows where every X is non-missing". */
    size_t Nfull = e->nobs_at_fit;
    if(Nfull > c->f->nobs) Nfull = c->f->nobs;
    if(!e->used){
        fprintf(stderr,"margins: in-sample bookkeeping missing\n");
        for(int k=0;k<e->K;k++) free(snap[k]);
        free(snap); free(xi); free(want); return 198;
    }
    long N_used = 0;
    for(size_t i=0;i<Nfull;i++) if(e->used[i]) N_used++;
    if(N_used < 1){
        fprintf(stderr,"margins: empty estimation sample\n");
        for(int k=0;k<e->K;k++) free(snap[k]);
        free(snap); free(xi); free(want); return 198;
    }

    /* Compute either AME (average over sample) or MEM (at sample means). */
    /* mvec[k] = point estimate for coef k (only filled where want[k]). */
    double *mvec = calloc(e->K, sizeof(double));
    /* G is K × K row-major: G[k][j] = ∂m_k/∂β_j.  Stored densely (zero for
     * !want[k]) so we can multiply by the full V matrix below. */
    double *G = calloc((size_t)e->K * e->K, sizeof(double));

    if(atmeans){
        /* MEM: compute X-bar over the in-sample rows, evaluate at one
         * point. */
        double *xbar = calloc(e->K, sizeof(double));
        for(int j=0;j<e->K;j++){
            if(xi[j] == -1){ xbar[j] = 1.0; continue; }
            double s = 0; long n=0;
            for(size_t i=0;i<Nfull;i++) if(e->used[i]){
                double v = XJI(j, i);
                if(!sv_is_miss(v)){ s += v; n++; }
            }
            xbar[j] = n>0 ? s/n : 0;
        }
        double eta_bar = 0;
        for(int j=0;j<e->K;j++) if(!e->omitted[j]) eta_bar += e->b[j] * xbar[j];
        double g_val, g_deriv; margins_g(e->cmd, eta_bar, &g_val, &g_deriv);
        for(int k=0;k<e->K;k++){
            if(!want[k]) continue;
            mvec[k] = g_val * e->b[k];
            /* ∂m_k/∂β_j = g'(η̄) · x̄_j · β_k  +  g(η̄) · 1{j=k}  */
            for(int j=0;j<e->K;j++){
                if(e->omitted[j]) continue;
                G[(size_t)k*e->K + j] = g_deriv * xbar[j] * e->b[k];
                if(j == k) G[(size_t)k*e->K + j] += g_val;
            }
        }
        free(xbar);
    } else {
        /* AME: sum ∂E/∂x_k over rows, divide by N. */
        for(size_t i=0;i<Nfull;i++){
            if(!e->used[i]) continue;
            double eta = 0;
            bool miss = false;
            for(int j=0;j<e->K;j++){
                if(e->omitted[j]) continue;
                double v = XJI(j, i);
                if(sv_is_miss(v)){ miss = true; break; }
                eta += e->b[j] * v;
            }
            if(miss) continue;
            double g_val, g_deriv; margins_g(e->cmd, eta, &g_val, &g_deriv);
            for(int k=0;k<e->K;k++){
                if(!want[k]) continue;
                mvec[k] += g_val * e->b[k];
                for(int j=0;j<e->K;j++){
                    if(e->omitted[j]) continue;
                    double xij = XJI(j, i);
                    if(sv_is_miss(xij)) xij = 0;   /* shouldn't happen given outer guard */
                    G[(size_t)k*e->K + j] += g_deriv * xij * e->b[k];
                    if(j == k) G[(size_t)k*e->K + j] += g_val;
                }
            }
        }
        /* divide by N_used */
        for(int k=0;k<e->K;k++) if(want[k]) mvec[k] /= N_used;
        for(int k=0;k<e->K;k++) for(int j=0;j<e->K;j++)
            G[(size_t)k*e->K + j] /= N_used;
    }

    /* SE via delta method: V(m_k) = G_k · V(β) · G_k'.
     * Compute K × K matrix V_m = G · V · G' (only the diagonal matters
     * for the report). */
    size_t K = e->K > 0 ? (size_t)e->K : 1;
    double *se = calloc(K, sizeof(double));
    /* GV[k, *] = G_k · V */
    double *GV = calloc(K * K, sizeof(double));
    for(int k=0;k<e->K;k++) if(want[k]){
        for(int j=0;j<e->K;j++){
            double s = 0;
            for(int a=0;a<e->K;a++) s += G[(size_t)k*e->K + a] * e->V[(size_t)a*e->K + j];
            GV[(size_t)k*e->K + j] = s;
        }
    }
    for(int k=0;k<e->K;k++) if(want[k]){
        double s = 0;
        for(int j=0;j<e->K;j++) s += GV[(size_t)k*e->K + j] * G[(size_t)k*e->K + j];
        se[k] = s > 0 ? sqrt(s) : 0;
    }

    /* Print Stata-style margins table. */
    if(!c->quiet){
        printf("\n");
        const char *kind_lbl = atmeans ? "at means" : "average";
        const char *outcome_lbl =
            (!strcmp(e->cmd,"logit") || !strcmp(e->cmd,"probit")) ? "Pr(y)" :
            "E(y)";
        printf("Marginal effects                                Number of obs   = %8ld\n", N_used);
        printf("Model VCE: %s\n", e->se_kind==SE_ROBUST?"Robust":
                                   e->se_kind==SE_CLUSTER?"Cluster":"OIM");
        printf("Expression: %s\n", outcome_lbl);
        printf("dy/dx w.r.t.: %s (%s)\n", dydx_spec, kind_lbl);
        printf("\n");
        printf("------------------------------------------------------------------------------\n");
        printf("             |     dy/dx     Std. err.    z    P>|z|     [95%% conf. interval]\n");
        printf("-------------+----------------------------------------------------------------\n");
        double zcrit = tea_invnormal(0.975);
        for(int k=0;k<e->K;k++){
            if(!want[k]) continue;
            double z  = se[k]>0 ? mvec[k]/se[k] : 0;
            double p  = se[k]>0 ? 2.0*(1.0 - tea_normal_cdf(fabs(z))) : 1.0;
            double lo = mvec[k] - zcrit*se[k];
            double hi = mvec[k] + zcrit*se[k];
            printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
                   e->xnames[k], mvec[k], se[k], z, p, lo, hi);
        }
        printf("------------------------------------------------------------------------------\n");
    }

    /* r() macros */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",N_used); mac_set(&c->ip->rret,"r(N)",bb);

    #undef XJI
    free(want); free(xi);
    if(snap){ for(int j=0;j<e->K;j++) free(snap[j]); free(snap); }
    free(mvec); free(G); free(GV); free(se);
    return 0;
}

/* ---- ivregress 2sls --------------------------------------------------- */
/*
 * Syntax: ivregress 2sls y [exog_vars] (endog_vars = instruments)
 *                       [if] [in] [weight] [, vce(robust|cluster v)]
 *
 * Two-stage least squares.  Let:
 *   y       : dependent variable
 *   X_1     : included exogenous regressors (+ _cons unless noconstant)
 *   X_2     : endogenous regressors (inside the paren on the LHS of =)
 *   Z       : excluded instruments (inside the paren on the RHS of =)
 *   W = [X_1, Z]   the "instrument matrix" used in the first stage
 *
 * First stage: for each column of X_2, regress on W → X̂_2 = P_W X_2
 *   where P_W = W(W'W)^{-1}W'.
 * Second stage: β = (X̂'X̂)^{-1} X̂'y, where X̂ = [X_1, X̂_2].
 *   Equivalent to:  β = (X' P_W X)^{-1} X' P_W y.
 *
 * Residuals for SE: ε̂ = y - X β  (using *original* X, not X̂).
 * Classical SE: σ̂² (X̂'X̂)^{-1} with σ̂² = ε̂'ε̂ / (N - K).
 * Robust SE:    HC1 sandwich (X̂'X̂)^{-1} · (X̂' diag(ε̂²) X̂) · (X̂'X̂)^{-1}.
 *
 * First-stage diagnostic: for each endog var, regress on W and report
 * the joint F-statistic for the excluded instruments Z (test that
 * Z has explanatory power conditional on X_1).  We report the
 * minimum across endog vars — the weak-instrument worry case.
 */
int do_ivregress(Cmd *c)
{
    if(!c->varlist[0]){
        fprintf(stderr,"ivregress: depvar + spec required\n");
        return 198;
    }

    /* Stata syntax requires "2sls" as the first token */
    char vbuf[2048]; snprintf(vbuf, sizeof vbuf, "%s", c->varlist);
    char *p = vbuf;
    while(*p == ' ') p++;
    char first[16] = "";
    int j = 0;
    while(*p && *p != ' ' && j < 15) first[j++] = *p++;
    first[j] = 0;
    if(strcmp(first, "2sls") != 0){
        fprintf(stderr,"ivregress: only '2sls' estimator supported in v1.0\n");
        return 198;
    }
    while(*p == ' ') p++;

    /* depvar */
    char depvar[64] = "";
    j = 0;
    while(*p && *p != ' ' && j < 63) depvar[j++] = *p++;
    depvar[j] = 0;
    while(*p == ' ') p++;
    if(!depvar[0]){
        fprintf(stderr,"ivregress: depvar required\n");
        return 198;
    }

    /* The rest contains exogenous regressors and a (endog = instruments)
     * group.  Walk forward collecting exog tokens until we hit '(';
     * then split inside the paren at '='. */
    char exog[1024] = "";
    char endog[512] = "";
    char instr[512] = "";
    size_t exog_n = 0;
    while(*p && *p != '('){
        if(exog_n + 1 < sizeof exog) exog[exog_n++] = *p;
        p++;
    }
    exog[exog_n] = 0;
    if(*p != '('){
        fprintf(stderr,"ivregress: missing (endog = instruments) group\n");
        return 198;
    }
    p++; /* past '(' */
    /* split at '=' */
    const char *eq = strchr(p, '=');
    const char *rp = strchr(p, ')');
    if(!eq || !rp || eq > rp){
        fprintf(stderr,"ivregress: malformed (endog = instruments) syntax\n");
        return 198;
    }
    /* endog spec is p..eq-1 */
    int len_endog = (int)(eq - p);
    if(len_endog >= (int)sizeof endog) len_endog = (int)sizeof endog - 1;
    memcpy(endog, p, len_endog); endog[len_endog] = 0;
    /* instr spec is eq+1..rp-1 */
    int len_instr = (int)(rp - eq - 1);
    if(len_instr >= (int)sizeof instr) len_instr = (int)sizeof instr - 1;
    memcpy(instr, eq + 1, len_instr); instr[len_instr] = 0;

    bool noconst = opt_present(c->options,"noconstant") || opt_present(c->options,"nocons");
    SeKind se_kind = SE_CLASSICAL;
    char clvar[33] = "";
    if(opt_value(c->options,"cluster",clvar,sizeof clvar)){ se_kind = SE_CLUSTER; }
    else if(opt_present(c->options,"robust")||opt_present(c->options,"vce")){
        char vce[64]=""; opt_value(c->options,"vce",vce,sizeof vce);
        if(vce[0]==0 || !strcmp(vce,"robust") || !strcmp(vce,"hc1")) se_kind = SE_ROBUST;
        else if(!strncmp(vce,"cluster",7)){ se_kind = SE_CLUSTER;
            char *p2=vce+7; while(*p2==' ')p2++; snprintf(clvar,33,"%s",p2); }
        else se_kind = SE_ROBUST;
    }

    /* Resolve the four lists into variable indices.  Expand TS-ops /
     * factor-vars in each. */
    int n_temps = 0;
    int *yi = NULL, *exi = NULL, *eni = NULL, *zi = NULL;
    int n_y, n_ex = 0, n_en, n_z;
    const char *vlerr = NULL;
    n_y = tsop_expand_varlist(c->f, depvar, &yi, &n_temps, &vlerr);
    if(n_y != 1){
        fprintf(stderr,"ivregress: depvar must resolve to one variable%s%s\n",
                vlerr ? " (" : "", vlerr ? vlerr : "");
        if(vlerr) fprintf(stderr,")\n");
        free(yi); tsop_drop_temps(c->f, n_temps); return 198;
    }
    int t2 = 0;
    if(exog[0]){
        n_ex = tsop_expand_varlist(c->f, exog, &exi, &t2, &vlerr);
        if(n_ex < 0){
            fprintf(stderr,"ivregress: bad exog %s\n", vlerr?vlerr:"");
            free(yi); free(exi); tsop_drop_temps(c->f, n_temps + t2); return 198;
        }
        n_temps += t2;
    }
    n_en = tsop_expand_varlist(c->f, endog, &eni, &t2, &vlerr);
    if(n_en < 1){
        fprintf(stderr,"ivregress: at least one endog variable required\n");
        free(yi); free(exi); free(eni); tsop_drop_temps(c->f, n_temps + t2); return 198;
    }
    n_temps += t2;
    n_z = tsop_expand_varlist(c->f, instr, &zi, &t2, &vlerr);
    if(n_z < 1){
        fprintf(stderr,"ivregress: at least one instrument required\n");
        free(yi); free(exi); free(eni); free(zi);
        tsop_drop_temps(c->f, n_temps + t2); return 198;
    }
    n_temps += t2;

    /* Order condition: need at least as many instruments as endog vars. */
    if(n_z < n_en){
        fprintf(stderr,"ivregress: order condition fails — need >= %d instruments, have %d\n",
                n_en, n_z);
        free(yi); free(exi); free(eni); free(zi);
        tsop_drop_temps(c->f, n_temps); return 198;
    }

    /* Build a combined "X" matrix [exog | endog | _cons?] for the main
     * regression.  We use build_design as the workhorse; it handles
     * missing-row dropping uniformly and gives us a clean N×K matrix. */
    int *all_x = malloc((n_ex + n_en) * sizeof(int));
    for(int k = 0; k < n_ex; k++) all_x[k] = exi[k];
    for(int k = 0; k < n_en; k++) all_x[n_ex + k] = eni[k];

    Design D;
    const char *err = NULL;
    if(build_design(c->f, yi[0], all_x, n_ex + n_en,
                    c->ifexp[0]?c->ifexp:NULL, c->in_lo, c->in_hi,
                    c->wexp[0]?c->wexp:NULL, c->wtype,
                    !noconst, &D, &err)){
        fprintf(stderr,"ivregress: %s\n", err?err:"build failed");
        free(all_x); free(yi); free(exi); free(eni); free(zi);
        tsop_drop_temps(c->f, n_temps); return 198;
    }
    free(all_x); free(yi);

    /* D.X is N×K column-major:
     *   cols [0,        n_ex)         = exog
     *   cols [n_ex,     n_ex+n_en)    = endog (we'll replace these with fitted)
     *   col   K-1 if has_cons         = ones
     * D.K = n_ex + n_en + (has_cons ? 1 : 0).
     * D.xnames has the correct labels already. */
    long N = D.N;
    int K = (int)D.K;
    int K_inst = n_ex + n_z + (noconst ? 0 : 1);   /* W = [exog, Z, _cons] */

    /* Build W = [exog | Z | _cons].  Same rows as D (via D.used). */
    double *W = malloc((size_t)N * K_inst * sizeof(double));
    long row = 0;
    for(size_t i = 0; i < D.nobs_full; i++){
        if(!D.used[i]) continue;
        int col = 0;
        for(int k = 0; k < n_ex; k++) W[(size_t)col * N + row] = c->f->vars[exi[k]].num[i], col++;
        for(int k = 0; k < n_z;  k++) W[(size_t)col * N + row] = c->f->vars[zi[k]].num[i],  col++;
        if(!noconst) W[(size_t)col * N + row] = 1.0;
        row++;
    }

    /* First stage: for each endog column j (which is D.X col n_ex+j),
     * regress on W and replace it with fitted values.  Also compute
     * the first-stage F-stat for the excluded instruments Z. */
    double *X_orig = malloc((size_t)N * K * sizeof(double));
    memcpy(X_orig, D.X, (size_t)N * K * sizeof(double));

    /* Solve W' W β_w = W' x_endog  via dgels */
    double min_fstat = INFINITY;
    int min_fstat_endog = -1;
    for(int jx = 0; jx < n_en; jx++){
        int col_in_X = n_ex + jx;
        double *xj = malloc(N * sizeof(double));
        memcpy(xj, &X_orig[(size_t)col_in_X * N], N * sizeof(double));
        double *Wc = malloc((size_t)N * K_inst * sizeof(double));
        memcpy(Wc, W, (size_t)N * K_inst * sizeof(double));
        double *yc = malloc(N * sizeof(double));
        memcpy(yc, xj, N * sizeof(double));
        int info = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N',
                                 N, K_inst, 1, Wc, N, yc, N);
        if(info){
            fprintf(stderr,"ivregress: first-stage solve failed (info=%d)\n", info);
            free(xj); free(Wc); free(yc); free(X_orig); free(W);
            free(exi); free(eni); free(zi);
            design_free(&D); tsop_drop_temps(c->f, n_temps); return 198;
        }
        /* yc[0..K_inst-1] is β; compute fitted x_hat = W·β and replace */
        double *x_hat = D.X + (size_t)col_in_X * N;
        for(long i = 0; i < N; i++){
            double sum = 0;
            for(int k = 0; k < K_inst; k++) sum += W[(size_t)k*N + i] * yc[k];
            x_hat[i] = sum;
        }
        /* First-stage F-stat for excluded instruments Z (cols n_ex..n_ex+n_z-1):
         * Restricted RSS = RSS when those coefficients are zero (i.e., regress
         *   x_endog on [exog, _cons] only).
         * Unrestricted RSS = RSS using full W.
         * F = ((R - U) / q) / (U / (N - K_inst)) with q = n_z. */
        double rss_u = 0;
        for(long i = 0; i < N; i++){
            double e = xj[i] - x_hat[i];
            rss_u += e * e;
        }
        /* Restricted: regress xj on [exog, _cons].  Just do another dgels. */
        int K_restr = n_ex + (noconst ? 0 : 1);
        double rss_r;
        if(K_restr == 0){
            /* No constant either: restricted model has no regressors, RSS = sum xj². */
            rss_r = 0;
            for(long i = 0; i < N; i++) rss_r += xj[i] * xj[i];
        } else {
            double *Wr = malloc((size_t)N * K_restr * sizeof(double));
            int rcol = 0;
            for(int k = 0; k < n_ex; k++){ memcpy(Wr + (size_t)rcol*N, W + (size_t)k*N, N*sizeof(double)); rcol++; }
            if(!noconst) for(long i = 0; i < N; i++) Wr[(size_t)rcol*N + i] = 1.0;
            double *yr = malloc(N * sizeof(double));
            memcpy(yr, xj, N * sizeof(double));
            int info_r = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N',
                                       N, K_restr, 1, Wr, N, yr, N);
            if(info_r){
                rss_r = rss_u;  /* defensive */
            } else {
                /* Re-evaluate residuals using Wr × yr (only first K_restr) */
                /* Need original Wr columns for the multiplication; dgels destroyed them.
                 * Use the columns of W directly. */
                rss_r = 0;
                for(long i = 0; i < N; i++){
                    double pred = 0;
                    int rc2 = 0;
                    for(int k = 0; k < n_ex; k++){ pred += W[(size_t)k*N + i] * yr[rc2]; rc2++; }
                    if(!noconst){ pred += yr[rc2]; }
                    double e = xj[i] - pred;
                    rss_r += e * e;
                }
            }
            free(Wr); free(yr);
        }
        double q = (double)n_z;
        double df_resid = (double)(N - K_inst);
        double fstat = 0;
        if(df_resid > 0 && rss_u > 0)
            fstat = ((rss_r - rss_u) / q) / (rss_u / df_resid);
        if(fstat < min_fstat){ min_fstat = fstat; min_fstat_endog = jx; }
        free(xj); free(Wc); free(yc);
    }

    /* Second stage: β = (X̂'X̂)^{-1} X̂'y.  Use dgels. */
    double *Xfit_c = malloc((size_t)N * K * sizeof(double));
    memcpy(Xfit_c, D.X, (size_t)N * K * sizeof(double));
    double *yfit_c = malloc(N * sizeof(double));
    memcpy(yfit_c, D.y, N * sizeof(double));
    int info2 = LAPACKE_dgels(LAPACK_COL_MAJOR, 'N', N, K, 1, Xfit_c, N, yfit_c, N);
    if(info2){
        fprintf(stderr,"ivregress: second-stage solve failed (info=%d)\n", info2);
        free(Xfit_c); free(yfit_c); free(X_orig); free(W);
        free(exi); free(eni); free(zi);
        design_free(&D); tsop_drop_temps(c->f, n_temps); return 198;
    }
    double *b = malloc(K * sizeof(double));
    memcpy(b, yfit_c, K * sizeof(double));
    free(Xfit_c); free(yfit_c);

    /* Residuals using ORIGINAL X (not fitted). */
    double *resid = malloc(N * sizeof(double));
    double rss = 0;
    for(long i = 0; i < N; i++){
        double xb = 0;
        for(int k = 0; k < K; k++) xb += X_orig[(size_t)k*N + i] * b[k];
        resid[i] = D.y[i] - xb;
        rss += resid[i] * resid[i];
    }
    long df_r = N - K;
    double sigma2 = df_r > 0 ? rss / df_r : 0;

    /* SE: classical, robust, or cluster.  All use X̂ (fitted) in the
     * outer (X̂'X̂)^{-1} part, and residuals from original X in the
     * meat.  This is the standard 2SLS variance formula. */
    /* (X̂'X̂)^{-1} */
    double *XtX = malloc((size_t)K*K*sizeof(double));
    for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
        double s = 0;
        for(long n = 0; n < N; n++) s += D.X[(size_t)i*N + n] * D.X[(size_t)jj*N + n];
        XtX[(size_t)jj*K + i] = s;
    }
    double *A = malloc((size_t)K*K*sizeof(double));   /* (X̂'X̂)^{-1} */
    memcpy(A, XtX, (size_t)K*K*sizeof(double));
    int inv_info = LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', K, A, K);
    if(inv_info == 0) inv_info = LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', K, A, K);
    if(inv_info){
        fprintf(stderr,"ivregress: (X'X) singular — likely collinearity or weak instruments\n");
        free(b); free(resid); free(XtX); free(A); free(X_orig); free(W);
        free(exi); free(eni); free(zi);
        design_free(&D); tsop_drop_temps(c->f, n_temps); return 198;
    }
    for(int i = 0; i < K; i++) for(int jj = i+1; jj < K; jj++)
        A[(size_t)i*K + jj] = A[(size_t)jj*K + i];

    double *V = malloc((size_t)K*K*sizeof(double));
    long n_cluster_groups = 0;
    if(se_kind == SE_CLASSICAL){
        for(long k = 0; k < (long)K*K; k++) V[k] = A[k] * sigma2;
    } else {
        /* Sandwich: V = A · B · A where B depends on robust/cluster */
        double *B = calloc((size_t)K*K, sizeof(double));
        if(se_kind == SE_ROBUST){
            for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
                double s = 0;
                for(long n = 0; n < N; n++)
                    s += D.X[(size_t)i*N + n] * resid[n] * resid[n] * D.X[(size_t)jj*N + n];
                B[(size_t)i*K + jj] = s;
            }
            double hc1 = (double)N / (double)(N - K);
            for(long k = 0; k < (long)K*K; k++) B[k] *= hc1;
        } else {
            /* cluster */
            int cvi = var_find(c->f, clvar);
            if(cvi < 0){
                fprintf(stderr,"ivregress: cluster var %s not found\n", clvar);
                free(B); free(V); free(b); free(resid); free(XtX); free(A);
                free(X_orig); free(W); free(exi); free(eni); free(zi);
                design_free(&D); tsop_drop_temps(c->f, n_temps); return 111;
            }
            /* Build cluster id per row */
            long *cid = malloc(N * sizeof(long));
            Variable *cv = &c->f->vars[cvi];
            double *seen_d = NULL; char **seen_s = NULL;
            long row2 = 0;
            for(size_t i = 0; i < D.nobs_full; i++){
                if(!D.used[i]) continue;
                long g = -1;
                if(cv->type == VT_NUM){
                    double v = cv->num[i];
                    for(long k = 0; k < n_cluster_groups; k++)
                        if(seen_d[k] == v){ g = k; break; }
                    if(g < 0){
                        seen_d = realloc(seen_d, (n_cluster_groups+1)*sizeof(double));
                        seen_d[n_cluster_groups] = v;
                        g = n_cluster_groups++;
                    }
                } else {
                    const char *v = cv->str[i] ? cv->str[i] : "";
                    for(long k = 0; k < n_cluster_groups; k++)
                        if(!strcmp(seen_s[k], v)){ g = k; break; }
                    if(g < 0){
                        seen_s = realloc(seen_s, (n_cluster_groups+1)*sizeof(char*));
                        seen_s[n_cluster_groups] = strdup(v);
                        g = n_cluster_groups++;
                    }
                }
                cid[row2++] = g;
            }
            free(seen_d);
            if(seen_s){ for(long k=0;k<n_cluster_groups;k++) free(seen_s[k]); free(seen_s); }

            double *u = calloc((size_t)n_cluster_groups*K, sizeof(double));
            for(long n = 0; n < N; n++){
                long g = cid[n];
                for(int k = 0; k < K; k++)
                    u[(size_t)g*K + k] += D.X[(size_t)k*N + n] * resid[n];
            }
            for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
                double s = 0;
                for(long g = 0; g < n_cluster_groups; g++)
                    s += u[(size_t)g*K + i] * u[(size_t)g*K + jj];
                B[(size_t)i*K + jj] = s;
            }
            free(u);
            double adj = (double)n_cluster_groups / (n_cluster_groups - 1)
                       * (double)(N - 1) / (N - K);
            for(long k = 0; k < (long)K*K; k++) B[k] *= adj;
            free(cid);
        }
        /* V = A B A */
        double *AB = calloc((size_t)K*K, sizeof(double));
        for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
            double s = 0;
            for(int k = 0; k < K; k++) s += A[(size_t)i*K + k] * B[(size_t)k*K + jj];
            AB[(size_t)i*K + jj] = s;
        }
        for(int i = 0; i < K; i++) for(int jj = 0; jj < K; jj++){
            double s = 0;
            for(int k = 0; k < K; k++) s += AB[(size_t)i*K + k] * A[(size_t)k*K + jj];
            V[(size_t)i*K + jj] = s;
        }
        free(AB); free(B);
    }

    /* Pseudo-R² for 2SLS (since the classical R² doesn't really work
     * with endogenous regressors).  Use 1 - RSS/TSS with TSS centered
     * around y-bar.  Stata reports this — call it "uncentered" only if
     * no constant. */
    double ybar = 0;
    for(long i = 0; i < N; i++) ybar += D.y[i];
    ybar /= N;
    double tss = 0;
    for(long i = 0; i < N; i++){ double d = D.y[i] - ybar; tss += d * d; }
    double r2 = (tss > 0) ? 1.0 - rss / tss : 0;
    double rmse = (df_r > 0) ? sqrt(rss / df_r) : 0;

    /* Populate Estimates. */
    Estimates *e = est_new();
    snprintf(e->cmd, sizeof e->cmd, "ivregress");
    snprintf(e->depvar, sizeof e->depvar, "%s", depvar);
    e->K = K;
    e->xnames = malloc(K * sizeof *e->xnames);
    memcpy(e->xnames, D.xnames, K * sizeof *e->xnames);
    e->omitted = calloc(K, sizeof(int));
    e->b = malloc(K * sizeof(double));
    memcpy(e->b, b, K * sizeof(double));
    e->V = V;
    e->N = N;
    e->df_r = (int)df_r;
    e->df_m = K - (!noconst ? 1 : 0);
    e->has_cons = !noconst;
    e->r2 = r2; e->r2_a = 0; e->rmse = rmse;
    e->F = 0; e->F_p = 0;
    e->sigma2 = sigma2;
    e->se_kind = se_kind;
    snprintf(e->cluster_var, sizeof e->cluster_var, "%s", clvar);
    e->n_clusters = n_cluster_groups;
    e->nobs_at_fit = D.nobs_full;
    e->used = D.used; D.used = NULL;
    snprintf(e->fitted_frame, sizeof e->fitted_frame, "%s", c->f->name);

    est_free(c->ws->last_est);
    c->ws->last_est = e;

    /* Output */
    if(!c->quiet){
        printf("\n");
        printf("Instrumental variables 2SLS regression           Number of obs   = %8ld\n", N);
        printf("                                                 R-squared       = %10.4f\n", r2);
        printf("                                                 Root MSE        = %10.4f\n", rmse);
        printf("\n");
        const char *selab = se_kind==SE_ROBUST?"Robust":
                            se_kind==SE_CLUSTER?"Cluster":"Std. err.";
        printf("------------------------------------------------------------------------------\n");
        printf("%12s | Coefficient  %-10s     z    P>|z|     [95%% conf. interval]\n",
               e->depvar, selab);
        printf("-------------+----------------------------------------------------------------\n");
        double zcrit = tea_invnormal(0.975);
        for(int k = 0; k < K; k++){
            double v = V[(size_t)k*K + k];
            double se = v > 0 ? sqrt(v) : 0;
            double z = se > 0 ? b[k]/se : 0;
            double pv = se > 0 ? 2.0*(1.0 - tea_normal_cdf(fabs(z))) : 1.0;
            double lo = b[k] - zcrit*se;
            double hi = b[k] + zcrit*se;
            printf("%12s | %10.6g  %10.6g %7.2f %5.3f   %10.6g  %10.6g\n",
                   D.xnames[k], b[k], se, z, pv, lo, hi);
        }
        printf("------------------------------------------------------------------------------\n");
        printf("Instrumented: ");
        for(int k = 0; k < n_en; k++) printf("%s ", c->f->vars[eni[k]].name);
        printf("\n");
        printf("Instruments:  ");
        for(int k = 0; k < n_ex; k++) printf("%s ", c->f->vars[exi[k]].name);
        for(int k = 0; k < n_z;  k++) printf("%s ", c->f->vars[zi[k]].name);
        if(!noconst) printf("_cons ");
        printf("\n");
        if(min_fstat_endog >= 0 && isfinite(min_fstat)){
            printf("\nFirst-stage diagnostic (weakest endog regressor):\n");
            printf("    %s: F(%d, %ld) = %.2f%s\n",
                   c->f->vars[eni[min_fstat_endog]].name,
                   n_z, (long)(N - K_inst), min_fstat,
                   min_fstat < 10 ? "   ← below conventional 10 threshold" : "");
        }
        if(se_kind == SE_CLUSTER)
            printf("(Std. err. adjusted for %ld clusters in %s)\n", n_cluster_groups, clvar);
    }

    /* r() macros + _b[]/_se[] */
    char bb[32];
    snprintf(bb,sizeof bb,"%ld",N); mac_set(&c->ip->rret,"e(N)",bb);
    snprintf(bb,sizeof bb,"%.10g",r2); mac_set(&c->ip->rret,"e(r2)",bb);
    snprintf(bb,sizeof bb,"%.10g",rmse); mac_set(&c->ip->rret,"e(rmse)",bb);
    if(isfinite(min_fstat)){
        snprintf(bb,sizeof bb,"%.10g",min_fstat); mac_set(&c->ip->rret,"e(F_first)",bb);
    }
    store_coef_macros(e, &c->ip->rret);

    free(X_orig); free(W); free(XtX); free(A); free(b); free(resid);
    free(exi); free(eni); free(zi);
    free(D.y); free(D.X); free(D.w); free(D.xnames);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

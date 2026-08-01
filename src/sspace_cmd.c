/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sspace_cmd.c — the sspace subset (DESIGN_SSPACE.md Addendum A).
 *
 * sspace (s L.s ..., state [noerror]) ... (y s ... [, noerror noconstant]) ...
 *        [, constraints(#) covstate(identity|diagonal) covobs(diagonal)
 *           smstates(stub)]
 *
 * Time-invariant, stationary (Lyapunov + PD guard).  Identification
 * via the constraints subsystem.  Estimation/OIM/multistart: the
 * dfactor machinery.
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

#define SP_MAXS   8      /* state equations */
#define SP_MAXY   8      /* observation equations */
#define SP_MAXPAR 200

typedef struct {
    int ns, ny;
    char snames[SP_MAXS][33];
    int  serr[SP_MAXS];                 /* 1 unless noerror */
    int  ntr[SP_MAXS];                  /* # lagged states loaded */
    int  tr[SP_MAXS][SP_MAXS];          /* which state index each L.term hits */
    int  yi[SP_MAXY];
    int  yerr[SP_MAXY], ycons[SP_MAXY];
    int  nld[SP_MAXY];                  /* # states in obs eq */
    int  ld[SP_MAXY][SP_MAXS];
    int  covstate_diag;                 /* 0 identity (default), 1 diagonal */
    int  diffuse;                       /* 1: Pinf=I, no stationarity req */
    /* parameter layout (mean/constrainable first):
     * obs-eq state coefs | obs constants | state transition coefs
     * then variances: state (if diag & err) then obs (if err) */
    int Km, nvar_s, nvar_y;
    int i_ld[SP_MAXY][SP_MAXS];
    int i_c[SP_MAXY];
    int i_tr[SP_MAXS][SP_MAXS];
    int v_s[SP_MAXS], v_y[SP_MAXY];     /* psi index offsets (into var tail), -1 */
    /* constraints reparam */
    double *tp, *Nb; int nfree, ncns;
    /* data */
    const double *y;  long Tn;
} SpSpec;

static int sp_state_idx(const SpSpec *sp, const char *nm){
    for(int i = 0; i < sp->ns; i++)
        if(!strcmp(sp->snames[i], nm)) return i;
    return -1;
}

static double sp_negll(const gsl_vector *x, void *params)
{
    SpSpec *sp = params;
    int m = sp->ns, ny = sp->ny;
    double th[SP_MAXPAR];
    for(int i = 0; i < sp->Km; i++) th[i] = sp->tp[i];
    for(int j = 0; j < sp->nfree; j++){
        double pj = gsl_vector_get(x, j);
        for(int i = 0; i < sp->Km; i++) th[i] += sp->Nb[(size_t)j*sp->Km + i]*pj;
    }
    double Z[SP_MAXY*SP_MAXS] = {0}, T[SP_MAXS*SP_MAXS] = {0};
    double R[SP_MAXS*SP_MAXS] = {0}, Q[SP_MAXS*SP_MAXS] = {0};
    double Hd[SP_MAXY] = {0};
    double a1[SP_MAXS] = {0}, Pst[SP_MAXS*SP_MAXS], Pinf[SP_MAXS*SP_MAXS] = {0};
    double RQR[SP_MAXS*SP_MAXS] = {0};
    for(int e = 0; e < ny; e++)
        for(int j = 0; j < sp->nld[e]; j++)
            Z[(size_t)sp->ld[e][j]*ny + e] = th[sp->i_ld[e][j]];
    for(int s = 0; s < m; s++)
        for(int j = 0; j < sp->ntr[s]; j++)
            T[(size_t)sp->tr[s][j]*m + s] = th[sp->i_tr[s][j]];
    int r = 0;
    for(int s = 0; s < m; s++) if(sp->serr[s]){
        R[(size_t)r*m + s] = 1.0;
        double q = sp->covstate_diag
                 ? exp(2.0*gsl_vector_get(x, sp->nfree + sp->v_s[s])) : 1.0;
        Q[(size_t)r*(r+1) + 0] = 0;      /* placeholder; set below with true r */
        RQR[(size_t)s*m + s] = q;
        r++;
    }
    /* Q as r x r diagonal */
    {
        int rr = 0;
        memset(Q, 0, sizeof Q);
        for(int s = 0; s < m; s++) if(sp->serr[s]){
            double q = sp->covstate_diag
                     ? exp(2.0*gsl_vector_get(x, sp->nfree + sp->v_s[s])) : 1.0;
            Q[(size_t)rr*r + rr] = q;
            rr++;
        }
    }
    for(int e = 0; e < ny; e++)
        Hd[e] = sp->yerr[e]
              ? exp(2.0*gsl_vector_get(x, sp->nfree + sp->v_y[e])) : 0.0;
    if(sp->diffuse){
        memset(Pst, 0, (size_t)m*m*sizeof(double));
        for(int i = 0; i < m; i++) Pinf[(size_t)i*m + i] = 1.0;
    } else {
        if(ss_lyapunov(T, RQR, m, Pst)) return 1e30;
        for(int i = 0; i < m*m; i++) if(!isfinite(Pst[i]) || fabs(Pst[i]) > 1e12) return 1e30;
        double Pchk[SP_MAXS*SP_MAXS];
        memcpy(Pchk, Pst, (size_t)m*m*sizeof(double));
        for(int i = 0; i < m; i++) Pchk[(size_t)i*m + i] += 1e-10;
        if(LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', m, Pchk, m) != 0) return 1e30;
    }
    SSModel M = { m, ny, r > 0 ? r : 1, Z, Hd, T, R, Q, a1, Pst, Pinf };
    long Tn = sp->Tn;
    double *w = malloc((size_t)ny*Tn*sizeof(double));
    for(int e = 0; e < ny; e++){
        double mu = sp->ycons[e] ? th[sp->i_c[e]] : 0.0;
        for(long t = 0; t < Tn; t++){
            double v = sp->y[(size_t)e*Tn + t];
            w[(size_t)e*Tn + t] = isnan(v) ? NAN : v - mu;
        }
    }
    double ll = ss_loglik(&M, w, Tn, NULL, NULL);
    free(w);
    if(!isfinite(ll)) return 1e30;
    return -ll;
}
static void sp_negll_df(const gsl_vector *x, void *params, gsl_vector *g)
{
    double h = 1e-5;
    gsl_vector *xp = gsl_vector_alloc(x->size);
    for(size_t i = 0; i < x->size; i++){
        gsl_vector_memcpy(xp, x);
        gsl_vector_set(xp, i, gsl_vector_get(x, i)+h);
        double fp = sp_negll(xp, params);
        gsl_vector_set(xp, i, gsl_vector_get(x, i)-h);
        double fm = sp_negll(xp, params);
        gsl_vector_set(g, i, (fp-fm)/(2*h));
    }
    gsl_vector_free(xp);
}
static void sp_negll_fdf(const gsl_vector *x, void *params, double *f, gsl_vector *g)
{ *f = sp_negll(x, params); sp_negll_df(x, params, g); }

static void sp_theta_to_psi(const SpSpec *sp, const double *th, double *psi)
{
    for(int j = 0; j < sp->nfree; j++){
        double s = 0;
        for(int i = 0; i < sp->Km; i++)
            s += sp->Nb[(size_t)j*sp->Km + i]*(th[i] - sp->tp[i]);
        psi[j] = s;
    }
}

int do_sspace(Cmd *c);
int do_sspace(Cmd *c)
{
    SpSpec sp; memset(&sp, 0, sizeof sp);
    /* ---- parse parenthesized equations ---- */
    char eqs[SP_MAXS + SP_MAXY][256]; int neq = 0;
    {
        const char *p = c->varlist; const char *st = NULL; int depth = 0;
        for(; *p; p++){
            if(*p == '('){ if(!depth) st = p+1; depth++; }
            else if(*p == ')'){
                depth--;
                if(!depth && st && neq < SP_MAXS + SP_MAXY)
                    snprintf(eqs[neq++], 256, "%.*s", (int)(p-st), st);
            }
        }
        if(neq < 2){
            tea_err("sspace: need at least one state and one observation equation\n");
            return 198;
        }
    }
    /* two passes: register state names first */
    int is_state[SP_MAXS + SP_MAXY] = {0};
    for(int e = 0; e < neq; e++){
        char *comma = strchr(eqs[e], ',');
        if(comma && strstr(comma+1, "state")){
            is_state[e] = 1;
            if(sp.ns >= SP_MAXS){ tea_err("sspace: too many state equations\n"); return 198; }
            char head[256];
            snprintf(head, sizeof head, "%.*s", (int)(comma - eqs[e]), eqs[e]);
            char *tok = strtok(head, " \t");
            if(!tok){ tea_err("sspace: empty state equation\n"); return 198; }
            snprintf(sp.snames[sp.ns], 33, "%s", tok);
            sp.serr[sp.ns] = strstr(comma+1, "noerror") ? 0 : 1;
            sp.ns++;
        }
    }
    if(!sp.ns){ tea_err("sspace: no state equations (mark with `, state')\n"); return 198; }
    /* second pass: fill loadings/transitions, obs eqs */
    int si = 0;
    for(int e = 0; e < neq; e++){
        char *comma = strchr(eqs[e], ',');
        char head[256];
        if(comma) snprintf(head, sizeof head, "%.*s", (int)(comma - eqs[e]), eqs[e]);
        else snprintf(head, sizeof head, "%s", eqs[e]);
        char *tok = strtok(head, " \t");
        if(is_state[e]){
            tok = strtok(NULL, " \t");    /* skip own name */
            while(tok){
                if(strncmp(tok, "L.", 2)){
                    tea_err("sspace: state equation %s: only L.state terms allowed (got %s)\n",
                            sp.snames[si], tok);
                    return 198;
                }
                int idx = sp_state_idx(&sp, tok+2);
                if(idx < 0){ tea_err("sspace: unknown state %s\n", tok+2); return 198; }
                if(sp.ntr[si] >= SP_MAXS){ tea_err("sspace: too many terms\n"); return 198; }
                sp.tr[si][sp.ntr[si]++] = idx;
                tok = strtok(NULL, " \t");
            }
            si++;
        } else {
            if(sp.ny >= SP_MAXY){ tea_err("sspace: too many observation equations\n"); return 198; }
            int e2 = sp.ny;
            int vi = var_find(c->f, tok);
            if(vi < 0){ tea_err("sspace: variable %s not found\n", tok); return 198; }
            sp.yi[e2] = vi;
            sp.yerr[e2] = comma && strstr(comma+1, "noerror") ? 0 : 1;
            sp.ycons[e2] = comma && strstr(comma+1, "nocons") ? 0 : 1;
            tok = strtok(NULL, " \t");
            while(tok){
                int idx = sp_state_idx(&sp, tok);
                if(idx < 0){ tea_err("sspace: obs equation %s: unknown state %s\n",
                                     c->f->vars[vi].name, tok); return 198; }
                if(sp.nld[e2] >= SP_MAXS){ tea_err("sspace: too many terms\n"); return 198; }
                sp.ld[e2][sp.nld[e2]++] = idx;
                tok = strtok(NULL, " \t");
            }
            sp.ny++;
        }
    }
    if(!sp.ny){ tea_err("sspace: no observation equations\n"); return 198; }
    /* options */
    char cvs[16] = "";
    if(opt_value(c->options, "covstate", cvs, sizeof cvs)){
        if(!strcmp(cvs, "diagonal")) sp.covstate_diag = 1;
        else if(strcmp(cvs, "identity")){
            tea_err("sspace: covstate(identity|diagonal) only\n"); return 198;
        }
    }
    sp.diffuse = opt_present(c->options, "diffuse") ? 1 : 0;
        char cvo[16] = "";
    if(opt_value(c->options, "covobs", cvo, sizeof cvo) && strcmp(cvo, "diagonal")){
        tea_err("sspace: covobs(diagonal) only (H must be diagonal; DESIGN_SSPACE.md §1)\n");
        return 198;
    }
    /* ---- parameter layout ---- */
    int Km = 0;
    for(int e = 0; e < sp.ny; e++)
        for(int j = 0; j < sp.nld[e]; j++) sp.i_ld[e][j] = Km++;
    for(int e = 0; e < sp.ny; e++) sp.i_c[e] = sp.ycons[e] ? Km++ : -1;
    for(int s = 0; s < sp.ns; s++)
        for(int j = 0; j < sp.ntr[s]; j++) sp.i_tr[s][j] = Km++;
    sp.Km = Km;
    sp.nvar_s = 0;
    for(int s = 0; s < sp.ns; s++)
        sp.v_s[s] = (sp.serr[s] && sp.covstate_diag) ? sp.nvar_s++ : -1;
    sp.nvar_y = 0;
    for(int e = 0; e < sp.ny; e++)
        sp.v_y[e] = sp.yerr[e] ? sp.nvar_s + sp.nvar_y++ : -1;
    int nvar = sp.nvar_s + sp.nvar_y;
    if(Km + nvar > SP_MAXPAR){ tea_err("sspace: model too large\n"); return 198; }
    char (*mnames)[33] = calloc(Km, 33);
    for(int e = 0; e < sp.ny; e++){
        const char *yn = c->f->vars[sp.yi[e]].name;
        for(int j = 0; j < sp.nld[e]; j++)
            snprintf(mnames[sp.i_ld[e][j]], 33, "[%s]%s", yn, sp.snames[sp.ld[e][j]]);
        if(sp.ycons[e]) snprintf(mnames[sp.i_c[e]], 33, "[%s]_cons", yn);
    }
    for(int s = 0; s < sp.ns; s++)
        for(int j = 0; j < sp.ntr[s]; j++)
            snprintf(mnames[sp.i_tr[s][j]], 33, "[%s]L.%s",
                     sp.snames[s], sp.snames[sp.tr[s][j]]);
    /* ---- constraints ---- */
    char cnl[256] = "";
    sp.ncns = 0;
    if(opt_value(c->options, "constraints", cnl, sizeof cnl) && cnl[0]){
        double *R = NULL, *r = NULL; int nc = 0;
        if(cns_build(c->ws, cnl, (const char (*)[33])mnames, Km, &R, &r, &nc)){
            free(mnames); return 198;
        }
        sp.tp = calloc(Km, sizeof(double));
        int rk = cns_nullspace(R, r, nc, Km, sp.tp, &sp.Nb, &sp.nfree);
        free(R); free(r);
        if(rk < 0){ tea_err("sspace: constraint decomposition failed\n");
            free(sp.tp); free(mnames); return 198; }
        sp.ncns = rk;
    } else {
        sp.tp = calloc(Km, sizeof(double));
        sp.Nb = calloc((size_t)Km*Km, sizeof(double));
        for(int i = 0; i < Km; i++) sp.Nb[(size_t)i*Km + i] = 1.0;
        sp.nfree = Km;
    }
    int Kp = sp.nfree + nvar;
    /* ---- sample ---- */
    if(c->f->ts_time < 0){ tea_err("sspace: data not tsset\n");
        free(sp.tp); free(sp.Nb); free(mnames); return 111; }
    if(c->f->ts_panel >= 0){ tea_err("sspace: panel data not supported\n");
        free(sp.tp); free(sp.Nb); free(mnames); return 111; }
    Variable *tv = &c->f->vars[c->f->ts_time];
    long first = -1, last = -1;
    for(size_t r2 = 0; r2 < c->f->nobs; r2++){
        if(sv_is_miss(tv->num[r2])) continue;
        if(first < 0) first = (long)r2;
        last = (long)r2;
    }
    if(first < 0){ tea_err("sspace: no observations\n");
        free(sp.tp); free(sp.Nb); free(mnames); return 2000; }
    for(long r2 = first; r2 < last; r2++)
        if(tv->num[r2+1] != tv->num[r2] + c->f->ts_delta){
            tea_err("sspace: time variable has gaps\n");
            free(sp.tp); free(sp.Nb); free(mnames); return 111; }
    long Tn = last - first + 1;
    double *y = malloc((size_t)sp.ny*Tn*sizeof(double));
    long nobs_scalar = 0;
    double ymean[SP_MAXY] = {0}, ysd[SP_MAXY] = {0}; long ycnt[SP_MAXY] = {0};
    for(int e = 0; e < sp.ny; e++){
        for(long t = 0; t < Tn; t++){
            double v = c->f->vars[sp.yi[e]].num[first+t];
            if(sv_is_miss(v)) y[(size_t)e*Tn + t] = NAN;
            else { y[(size_t)e*Tn + t] = v; ymean[e] += v; ycnt[e]++; nobs_scalar++; }
        }
        ymean[e] /= ycnt[e] > 0 ? ycnt[e] : 1;
        for(long t = 0; t < Tn; t++){
            double v = y[(size_t)e*Tn + t];
            if(!isnan(v)) ysd[e] += (v-ymean[e])*(v-ymean[e]);
        }
        ysd[e] = sqrt(ysd[e]/(ycnt[e] > 1 ? ycnt[e]-1 : 1));
    }
    sp.y = y; sp.Tn = Tn;
    /* ---- deterministic starting values ---- */
    double th0[SP_MAXPAR] = {0};
    for(int e = 0; e < sp.ny; e++){
        for(int j = 0; j < sp.nld[e]; j++) th0[sp.i_ld[e][j]] = 1.0;
        if(sp.ycons[e]) th0[sp.i_c[e]] = ymean[e];
    }
    for(int s = 0; s < sp.ns; s++)
        for(int j = 0; j < sp.ntr[s]; j++)
            th0[sp.i_tr[s][j]] = sp.tr[s][j] == s ? 0.5 : 0.1;
    double lv0[SP_MAXPAR];
    for(int s = 0; s < sp.ns; s++) if(sp.v_s[s] >= 0)
        lv0[sp.v_s[s]] = log(0.7*(ysd[0] > 0 ? ysd[0] : 1.0));
    for(int e = 0; e < sp.ny; e++) if(sp.v_y[e] >= 0)
        lv0[sp.v_y[e]] = log(0.5*(ysd[e] > 0 ? ysd[e] : 1.0));
    /* ---- BFGS2 multistart ---- */
    double psi[SP_MAXPAR]; double ll = -1e300;
    {
        double s0[SP_MAXPAR];
        sp_theta_to_psi(&sp, th0, s0);
        gsl_multimin_function_fdf F =
            { sp_negll, sp_negll_df, sp_negll_fdf, (size_t)Kp, &sp };
        for(int st2 = 0; st2 < 3; st2++){
            gsl_multimin_fdfminimizer *mz =
                gsl_multimin_fdfminimizer_alloc(
                    gsl_multimin_fdfminimizer_vector_bfgs2, Kp);
            gsl_vector *x0 = gsl_vector_alloc(Kp);
            double scal = st2 == 0 ? 1.0 : (st2 == 1 ? 0.5 : 1.5);
            for(int j = 0; j < sp.nfree; j++) gsl_vector_set(x0, j, s0[j]*scal);
            for(int i = 0; i < nvar; i++)
                gsl_vector_set(x0, sp.nfree + i, lv0[i] + (st2 == 2 ? -1.0 : 0.0));
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
    double th[SP_MAXPAR];
    for(int i = 0; i < Km; i++){
        th[i] = sp.tp[i];
        for(int j = 0; j < sp.nfree; j++) th[i] += sp.Nb[(size_t)j*Km + i]*psi[j];
    }
    double var_out[SP_MAXPAR];
    for(int i = 0; i < nvar; i++) var_out[i] = exp(2.0*psi[sp.nfree + i]);
    /* ---- OIM ---- */
    int Krep = Km + nvar;
    double *V = calloc((size_t)Krep*Krep, sizeof(double));
    {
        double *Hp = malloc((size_t)Kp*Kp*sizeof(double));
        double h = 1e-4;
        for(int i = 0; i < Kp; i++) for(int j = 0; j <= i; j++){
            double tpp[SP_MAXPAR], tpm[SP_MAXPAR], tmp2[SP_MAXPAR], tmm[SP_MAXPAR];
            memcpy(tpp, psi, sizeof psi); memcpy(tpm, psi, sizeof psi);
            memcpy(tmp2, psi, sizeof psi); memcpy(tmm, psi, sizeof psi);
            tpp[i] += h; tpp[j] += h;  tpm[i] += h; tpm[j] -= h;
            tmp2[i] -= h; tmp2[j] += h; tmm[i] -= h; tmm[j] -= h;
            gsl_vector_view vv2;
            double fpp, fpm, fmp, fmm;
            vv2 = gsl_vector_view_array(tpp, Kp); fpp = sp_negll(&vv2.vector, &sp);
            vv2 = gsl_vector_view_array(tpm, Kp); fpm = sp_negll(&vv2.vector, &sp);
            vv2 = gsl_vector_view_array(tmp2, Kp); fmp = sp_negll(&vv2.vector, &sp);
            vv2 = gsl_vector_view_array(tmm, Kp); fmm = sp_negll(&vv2.vector, &sp);
            Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i] =
                (fpp - fpm - fmp + fmm)/(4*h*h);
        }
        int okH = (LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0)
               && (LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', Kp, Hp, Kp) == 0);
        if(!okH){
            tea_err("sspace: information matrix not positive definite — SEs unavailable"
                    " (is the model identified?  see constraints())\n");
        } else {
            for(int i = 0; i < Kp; i++) for(int j = i+1; j < Kp; j++)
                Hp[(size_t)i*Kp + j] = Hp[(size_t)j*Kp + i];
            double *J = calloc((size_t)Krep*Kp, sizeof(double));
            for(int i = 0; i < Km; i++)
                for(int j = 0; j < sp.nfree; j++)
                    J[(size_t)j*Krep + i] = sp.Nb[(size_t)j*Km + i];
            for(int i = 0; i < nvar; i++)
                J[(size_t)(sp.nfree + i)*Krep + (Km + i)] = 2.0*var_out[i];
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
    if(!c->quiet){
        printf("State-space model\n");
        printf("States: %d (%d stochastic)   Observation equations: %d\n",
               sp.ns, sp.nvar_s + (sp.covstate_diag ? 0 : 0), sp.ny);
        printf("\nSample: %ld periods (%ld scalar obs)          Log likelihood = %.5f\n",
               Tn, nobs_scalar, ll);
        if(sp.ncns) printf("Constraints imposed: %d (rank)\n", sp.ncns);
        printf("------------------------------------------------------------------------------\n");
        printf("             | Coefficient  Std. err.      z    P>|z|     [95%% conf. interval]\n");
        double zc = gsl_cdf_ugaussian_Pinv(0.975);
        char cureq[34] = "";
        for(int i = 0; i < Km; i++){
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
            if(sp.ncns && se < 1e-12){
                double nn = 0;
                for(int j = 0; j < sp.nfree; j++)
                    nn += fabs(sp.Nb[(size_t)j*Km + i]);
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
        if(nvar){
            printf("-------------+----------------------------------------------------------------\n");
            printf("%-12.12s |\n", "Variance");
            int vi2 = 0;
            for(int s = 0; s < sp.ns; s++) if(sp.v_s[s] >= 0){
                double se = V[(size_t)(Km+vi2)*Krep + (Km+vi2)] > 0
                          ? sqrt(V[(size_t)(Km+vi2)*Krep + (Km+vi2)]) : 0;
                char lab[40]; snprintf(lab, sizeof lab, "var(%s)", sp.snames[s]);
                printf("%12.12s | %10.6g  %10.6g\n", lab, var_out[vi2], se);
                vi2++;
            }
            for(int e = 0; e < sp.ny; e++) if(sp.v_y[e] >= 0){
                double se = V[(size_t)(Km+vi2)*Krep + (Km+vi2)] > 0
                          ? sqrt(V[(size_t)(Km+vi2)*Krep + (Km+vi2)]) : 0;
                char lab[40];
                snprintf(lab, sizeof lab, "var(e.%s)", c->f->vars[sp.yi[e]].name);
                printf("%12.12s | %10.6g  %10.6g\n", lab, var_out[vi2], se);
                vi2++;
            }
        }
        printf("------------------------------------------------------------------------------\n");
        if(!sp.covstate_diag)
            printf("Note: covstate(identity) — state disturbance variances fixed at 1.\n");
    }
    /* ---- smstates(stub) ---- */
    char stub[33] = "";
    if(opt_value(c->options, "smstates", stub, sizeof stub) && stub[0]){
        gsl_vector_view pv = gsl_vector_view_array(psi, Kp);
        (void)pv;
        /* rebuild system at optimum (reuse sp_negll's construction inline) */
        int m = sp.ns, ny = sp.ny;
        double Z[SP_MAXY*SP_MAXS] = {0}, T[SP_MAXS*SP_MAXS] = {0};
        double R[SP_MAXS*SP_MAXS] = {0}, Q[SP_MAXS*SP_MAXS] = {0};
        double Hd[SP_MAXY] = {0}, a1[SP_MAXS] = {0};
        double Pst[SP_MAXS*SP_MAXS], Pinf[SP_MAXS*SP_MAXS] = {0};
        double RQR[SP_MAXS*SP_MAXS] = {0};
        for(int e = 0; e < ny; e++)
            for(int j = 0; j < sp.nld[e]; j++)
                Z[(size_t)sp.ld[e][j]*ny + e] = th[sp.i_ld[e][j]];
        for(int s = 0; s < m; s++)
            for(int j = 0; j < sp.ntr[s]; j++)
                T[(size_t)sp.tr[s][j]*m + s] = th[sp.i_tr[s][j]];
        int r3 = 0;
        for(int s = 0; s < m; s++) if(sp.serr[s]){
            R[(size_t)r3*m + s] = 1.0;
            double q = sp.v_s[s] >= 0 ? var_out[sp.v_s[s]] : 1.0;
            RQR[(size_t)s*m + s] = q;
            r3++;
        }
        {
            int rr = 0;
            for(int s = 0; s < m; s++) if(sp.serr[s]){
                double q = sp.v_s[s] >= 0 ? var_out[sp.v_s[s]] : 1.0;
                Q[(size_t)rr*r3 + rr] = q; rr++;
            }
        }
        for(int e = 0; e < ny; e++)
            Hd[e] = sp.v_y[e] >= 0 ? var_out[sp.v_y[e]] : 0.0;
        int init_ok = 1;
        if(sp.diffuse){
            memset(Pst, 0, (size_t)m*m*sizeof(double));
            for(int i2 = 0; i2 < m; i2++) Pinf[(size_t)i2*m + i2] = 1.0;
        } else init_ok = !ss_lyapunov(T, RQR, m, Pst);
        if(init_ok){
            SSModel M = { m, ny, r3 > 0 ? r3 : 1, Z, Hd, T, R, Q, a1, Pst, Pinf };
            double *w = malloc((size_t)ny*Tn*sizeof(double));
            for(int e = 0; e < ny; e++){
                double mu = sp.ycons[e] ? th[sp.i_c[e]] : 0.0;
                for(long t = 0; t < Tn; t++){
                    double v = y[(size_t)e*Tn + t];
                    w[(size_t)e*Tn + t] = isnan(v) ? NAN : v - mu;
                }
            }
            double *ahat = malloc((size_t)Tn*m*sizeof(double));
            ss_smooth(&M, w, Tn, ahat, NULL, NULL, NULL);
            for(int s = 0; s < m; s++){
                char vn[65];
                snprintf(vn, sizeof vn, "%s_%s", stub, sp.snames[s]);
                if(var_find(c->f, vn) >= 0){
                    tea_err("sspace: %s already defined (smstates skipped)\n", vn);
                    break;
                }
                Variable *nv2 = var_add(c->f, vn, VT_NUM);
                for(size_t r4 = 0; r4 < c->f->nobs; r4++) nv2->num[r4] = SV_MISS;
                for(long t = 0; t < Tn; t++) nv2->num[first+t] = ahat[(size_t)t*m + s];
            }
            free(ahat); free(w);
            if(!c->quiet) printf("(smoothed states saved as %s_*)\n", stub);
        }
    }
    /* ---- Estimates post ---- */
    {
        Estimates *ee = est_new();
        snprintf(ee->cmd, 16, "sspace");
        snprintf(ee->depvar, 33, "%s", c->f->vars[sp.yi[0]].name);
        ee->K = Krep;
        ee->xnames = calloc(Krep, 33);
        for(int i = 0; i < Km; i++){
            char nm[40]; int o = 0;
            for(const char *p = mnames[i]; *p && o < 32; p++){
                if(*p == '[') continue;
                if(*p == ']'){ nm[o++] = '_'; continue; }
                if(*p == '.') continue;
                nm[o] = *p; o++;
            }
            nm[o] = 0;
            snprintf(ee->xnames[i], 33, "%s", nm);
        }
        {
            int vi2 = 0;
            for(int s = 0; s < sp.ns; s++) if(sp.v_s[s] >= 0)
                snprintf(ee->xnames[Km + vi2++], 33, "var_%s", sp.snames[s]);
            for(int e = 0; e < sp.ny; e++) if(sp.v_y[e] >= 0)
                snprintf(ee->xnames[Km + vi2++], 33, "var_e%s",
                         c->f->vars[sp.yi[e]].name);
        }
        ee->omitted = calloc(Krep, sizeof(int));
        ee->b = malloc((size_t)Krep*sizeof(double));
        memcpy(ee->b, th, (size_t)Km*sizeof(double));
        for(int i = 0; i < nvar; i++) ee->b[Km + i] = var_out[i];
        ee->V = malloc((size_t)Krep*Krep*sizeof(double));
        memcpy(ee->V, V, (size_t)Krep*Krep*sizeof(double));
        ee->N = nobs_scalar;
        ee->df_r = (int)(nobs_scalar - Krep);
        ee->df_m = Krep;
        ee->has_cons = 0;
        ee->se_kind = SE_CLASSICAL;
        ee->nobs_at_fit = c->f->nobs;
        ee->used = calloc(c->f->nobs, 1);
        for(long t = 0; t < Tn; t++){
            int any = 0;
            for(int e = 0; e < sp.ny; e++)
                if(!isnan(y[(size_t)e*Tn + t])) any = 1;
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
    free(V); free(mnames); free(sp.tp); free(sp.Nb); free(y);
    return 0;
}

/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sspace.c — state-space front-ends (DESIGN_SSPACE.md §8).
 * Release one: ucm with models ntrend/llevel/lltrend/rwalk/rwdrift and
 * an optional dummy seasonal; ML by BFGS on log-sigma parameters
 * (Decision 6/7); OIM standard errors; smoothed level extraction via
 * smstate(NEWVAR).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_cdf.h>
#include "cmd.h"
#include "dataset.h"
#include "value.h"
#include "kalman.h"
#include <lapacke.h>
#include "tsop.h"
#include "estimates.h"
#include "interp.h"
extern void store_coef_macros(Estimates *e, MacroKV **tbl);

static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

/* ---- model catalogue -------------------------------------------------- */
typedef enum { UCM_NTREND, UCM_LLEVEL, UCM_LLTREND, UCM_RWALK, UCM_RWDRIFT } UcmModel;

typedef struct {
    UcmModel model;
    int      s;             /* seasonal period, 0 = none */
    int      cyc;           /* 1: one first-order stochastic cycle */
    /* dimensions */
    int m, r, ntheta, cyc0; /* cyc0: index of first cycle state */
    /* parameter slots: index into theta or -1 */
    int i_eps, i_level, i_slope, i_seas;
    int i_crho, i_clam, i_cvar;      /* cycle: damping, frequency, variance */
    /* workspace matrices (owned) */
    double *Z, *T, *R, *Q, *a1, *Pstar, *Pinf, Hdiag[1];
} UcmSpec;

/* Build structure (everything except the Q/H values, which come from
 * theta at each evaluation). */
static int ucm_build(UcmSpec *u){
    int m_trend = 0, r_trend = 0;
    int has_eps = 1, has_level_var = 0, has_slope_var = 0;
    switch(u->model){
        case UCM_NTREND:  m_trend=0; r_trend=0; break;
        case UCM_LLEVEL:  m_trend=1; r_trend=1; has_level_var=1; break;
        case UCM_LLTREND: m_trend=2; r_trend=2; has_level_var=1; has_slope_var=1; break;
        case UCM_RWALK:   m_trend=1; r_trend=1; has_level_var=1; has_eps=0; break;
        case UCM_RWDRIFT: m_trend=2; r_trend=1; has_level_var=1; has_eps=0; break;
    }
    int m_seas = u->s>1 ? u->s-1 : 0;
    int r_seas = m_seas ? 1 : 0;
    int m_cyc = u->cyc ? 2 : 0;
    u->m = m_trend + m_seas + m_cyc;
    u->r = r_trend + r_seas + (u->cyc ? 2 : 0);
    if(u->m == 0 && !has_eps){ return 1; }
    if(u->m == 0){
        /* pure noise: give it a degenerate 1-state zero system so the
         * engine has something to iterate; Z=0 */
        u->m = 1; u->r = 1;
    }
    int m=u->m, r=u->r? u->r:1; u->r=r;
    u->Z = calloc((size_t)1*m,sizeof(double));
    u->T = calloc((size_t)m*m,sizeof(double));
    u->R = calloc((size_t)m*r,sizeof(double));
    u->Q = calloc((size_t)r*r,sizeof(double));
    u->a1= calloc((size_t)m,sizeof(double));
    u->Pstar = calloc((size_t)m*m,sizeof(double));
    u->Pinf  = calloc((size_t)m*m,sizeof(double));
    int st = 0, rq = 0;
    /* trend block */
    if(m_trend >= 1){
        u->Z[st] = 1.0;
        u->T[(size_t)st*m + st] = 1.0;
        u->Pinf[(size_t)st*m + st] = 1.0;
        if(m_trend == 2){
            u->T[(size_t)(st+1)*m + st] = 1.0;   /* mu <- mu + beta */
            u->T[(size_t)(st+1)*m + st+1] = 1.0;
            u->Pinf[(size_t)(st+1)*m + st+1] = 1.0;
        }
        if(has_level_var){ u->R[(size_t)rq*m + st] = 1.0; rq++; }
        if(has_slope_var){ u->R[(size_t)rq*m + st+1] = 1.0; rq++; }
        st += m_trend;
    }
    /* seasonal block: gamma_{t+1} = -(gamma_t + ... + gamma_{t-s+2}) + w */
    if(m_seas){
        u->Z[st] = 1.0;
        for(int j=0;j<m_seas;j++)
            u->T[(size_t)(st+j)*m + st] = -1.0;
        for(int j=1;j<m_seas;j++)
            u->T[(size_t)(st+j-1)*m + st+j] = 1.0;
        for(int j=0;j<m_seas;j++)
            u->Pinf[(size_t)(st+j)*m + st+j] = 1.0;
        u->R[(size_t)rq*m + st] = 1.0; rq++;
        st += m_seas;
    }
    /* cycle block: Z picks first state; R selects both; T and Pstar
     * are parameter-dependent and filled in ucm_model */
    if(u->cyc){
        u->cyc0 = st;
        u->Z[st] = 1.0;
        u->R[(size_t)rq*m + st] = 1.0; rq++;
        u->R[(size_t)rq*m + st+1] = 1.0; rq++;
        st += 2;
    }
    /* theta layout */
    int k=0;
    u->i_eps   = has_eps ? k++ : -1;
    u->i_level = has_level_var ? k++ : -1;
    u->i_slope = has_slope_var ? k++ : -1;
    u->i_seas  = m_seas ? k++ : -1;
    u->i_crho = u->i_clam = u->i_cvar = -1;
    if(u->cyc){ u->i_crho = k++; u->i_clam = k++; u->i_cvar = k++; }
    u->ntheta = k;
    return 0;
}

static void ucm_free(UcmSpec *u){
    free(u->Z); free(u->T); free(u->R); free(u->Q);
    free(u->a1); free(u->Pstar); free(u->Pinf);
}

/* theta (log-sigma) -> model; returns SSModel view over u's buffers */
static SSModel ucm_model(UcmSpec *u, const double *theta){
    memset(u->Q, 0, (size_t)u->r*u->r*sizeof(double));
    int rq=0;
    if(u->i_level>=0) u->Q[(size_t)rq*u->r + rq] = exp(2.0*theta[u->i_level]), rq++;
    if(u->i_slope>=0) u->Q[(size_t)rq*u->r + rq] = exp(2.0*theta[u->i_slope]), rq++;
    if(u->i_seas>=0)  u->Q[(size_t)rq*u->r + rq] = exp(2.0*theta[u->i_seas]),  rq++;
    if(u->cyc){
        int m = u->m, c0 = u->cyc0;
        double rho = 1.0/(1.0 + exp(-theta[u->i_crho]));
        double lam = M_PI/(1.0 + exp(-theta[u->i_clam]));
        double s2c = exp(2.0*theta[u->i_cvar]);
        u->T[(size_t)c0*m + c0]       =  rho*cos(lam);
        u->T[(size_t)(c0+1)*m + c0]   =  rho*sin(lam);
        u->T[(size_t)c0*m + c0+1]     = -rho*sin(lam);
        u->T[(size_t)(c0+1)*m + c0+1] =  rho*cos(lam);
        u->Q[(size_t)rq*u->r + rq] = s2c; rq++;
        u->Q[(size_t)rq*u->r + rq] = s2c; rq++;
        /* stationary cycle block: unconditional var s2c/(1-rho^2) I2 */
        double pv = s2c/(1.0 - rho*rho);
        u->Pstar[(size_t)c0*m + c0]     = pv;
        u->Pstar[(size_t)(c0+1)*m + c0+1] = pv;
    }
    u->Hdiag[0] = u->i_eps>=0 ? exp(2.0*theta[u->i_eps]) : 0.0;
    SSModel M = { u->m, 1, u->r, u->Z, u->Hdiag, u->T, u->R, u->Q,
                  u->a1, u->Pstar, u->Pinf };
    return M;
}

/* ---- MLE driver ------------------------------------------------------- */
typedef struct { UcmSpec *u; const double *y; long Tn; } MleCtx;

static double mle_f(const gsl_vector *x, void *params){
    MleCtx *c = params;
    double th[8];
    for(int i=0;i<c->u->ntheta;i++) th[i]=gsl_vector_get(x,i);
    SSModel M = ucm_model(c->u, th);
    double ll = ss_loglik(&M, c->y, c->Tn, NULL, NULL);
    if(!isfinite(ll)) return 1e30;
    return -ll;
}
static void mle_df(const gsl_vector *x, void *params, gsl_vector *g){
    double h = 1e-5;
    gsl_vector *xp = gsl_vector_alloc(x->size);
    for(size_t i=0;i<x->size;i++){
        gsl_vector_memcpy(xp,x);
        gsl_vector_set(xp,i,gsl_vector_get(x,i)+h);
        double fp = mle_f(xp,params);
        gsl_vector_set(xp,i,gsl_vector_get(x,i)-h);
        double fm = mle_f(xp,params);
        gsl_vector_set(g,i,(fp-fm)/(2*h));
    }
    gsl_vector_free(xp);
}
static void mle_fdf(const gsl_vector *x, void *params, double *f, gsl_vector *g){
    *f = mle_f(x,params); mle_df(x,params,g);
}

int do_ucm(Cmd *c);
int do_ucm(Cmd *c){
    /* parse */
    int *vv=NULL, nvv, ntp=0; const char *ve=NULL;
    nvv = tsop_expand_varlist(c->f, c->varlist, &vv, &ntp, &ve);
    if(nvv!=1){ tea_err("ucm: one depvar required\n"); free(vv); return 111; }
    int yi=vv[0]; free(vv);
    char mdl[24]="";
    if(!opt_value(c->options,"model",mdl,sizeof mdl) || !mdl[0]){
        tea_err("ucm: model(ntrend|llevel|lltrend|rwalk|rwdrift) required\n");
        tsop_drop_temps(c->f,ntp); return 198;
    }
    UcmSpec u = {0};
    if(!strcmp(mdl,"ntrend")) u.model=UCM_NTREND;
    else if(!strcmp(mdl,"llevel")) u.model=UCM_LLEVEL;
    else if(!strcmp(mdl,"lltrend")) u.model=UCM_LLTREND;
    else if(!strcmp(mdl,"rwalk")) u.model=UCM_RWALK;
    else if(!strcmp(mdl,"rwdrift")) u.model=UCM_RWDRIFT;
    else { tea_err("ucm: unknown model %s\n", mdl); tsop_drop_temps(c->f,ntp); return 198; }
    u.cyc = opt_present(c->options,"cycle") ? 1 : 0;
    char sb[16]="";
    u.s = opt_value(c->options,"seasonal",sb,sizeof sb)? atoi(sb) : 0;
    if(u.s==1 || u.s<0 || u.s>24){ tea_err("ucm: seasonal(#) out of range\n");
        tsop_drop_temps(c->f,ntp); return 198; }
    if(u.model==UCM_NTREND && u.s<2 && !u.cyc){
        tea_err("ucm: model(ntrend) needs seasonal(#) to have any component\n");
        tsop_drop_temps(c->f,ntp); return 198; }
    if(ucm_build(&u)){ tea_err("ucm: empty model\n"); tsop_drop_temps(c->f,ntp); return 198; }
    /* sample: tsset, gaps in time are an error, missing VALUES are legal */
    if(c->f->ts_time < 0){ tea_err("ucm: data not tsset\n"); ucm_free(&u);
        tsop_drop_temps(c->f,ntp); return 111; }
    if(c->f->ts_panel >= 0){ tea_err("ucm: panel data not supported\n"); ucm_free(&u);
        tsop_drop_temps(c->f,ntp); return 111; }
    Variable *tv=&c->f->vars[c->f->ts_time], *yv=&c->f->vars[yi];
    long first=-1,last=-1;
    for(size_t r=0;r<c->f->nobs;r++){
        if(sv_is_miss(tv->num[r])) continue;
        if(first<0) first=(long)r;
        last=(long)r;
    }
    if(first<0){ tea_err("ucm: no observations\n"); ucm_free(&u);
        tsop_drop_temps(c->f,ntp); return 2000; }
    for(long r=first;r<last;r++)
        if(tv->num[r+1] != tv->num[r]+c->f->ts_delta){
            tea_err("ucm: time variable has gaps\n"); ucm_free(&u);
            tsop_drop_temps(c->f,ntp); return 111; }
    long Tn = last-first+1;
    double *y = malloc((size_t)Tn*sizeof(double));
    long nobs=0;
    for(long t=0;t<Tn;t++){
        double v = yv->num[first+t];
        if(sv_is_miss(v)) y[t]=NAN; else { y[t]=v; nobs++; }
    }
    if(nobs < u.ntheta+2){ tea_err("ucm: insufficient observations\n");
        free(y); ucm_free(&u); tsop_drop_temps(c->f,ntp); return 2000; }
    /* starting values: log(sd/2) heuristics on levels and differences */
    double sy=0,syy=0; long nn=0;
    double sd1=0; long nd=0; double prev=NAN;
    for(long t=0;t<Tn;t++){
        if(isnan(y[t])) { prev=NAN; continue; }
        sy+=y[t]; syy+=y[t]*y[t]; nn++;
        if(!isnan(prev)) { sd1 += (y[t]-prev)*(y[t]-prev); nd++; }
        prev=y[t];
    }
    double sdy = sqrt((syy-sy*sy/nn)/(nn-1));
    double sdd = nd>1 ? sqrt(sd1/nd) : sdy;
    double th0[8];
    for(int i=0;i<u.ntheta;i++) th0[i]=log(sdy>0? sdy*0.5 : 1.0);
    if(u.i_level>=0) th0[u.i_level]=log(sdd>0? sdd*0.5 : 1.0);
    if(u.i_slope>=0) th0[u.i_slope]=log(sdd>0? sdd*0.1 : 1.0);
    if(u.i_seas>=0)  th0[u.i_seas]=log(sdd>0? sdd*0.1 : 1.0);
    if(u.cyc){
        th0[u.i_crho]=2.0;                       /* rho ~ .88 */
        th0[u.i_clam]=-1.5;                      /* lam ~ .58 (period ~11) */
        th0[u.i_cvar]=log(sdd>0? sdd*0.3 : 1.0);
    }
    /* BFGS2 with central-difference gradients (Decision 6), from a
     * fixed set of deterministic starting points — flat UC likelihoods
     * (airline!) have boundary-swapped local optima, and multiple
     * starts are the deterministic cure.  Best final ll wins. */
    MleCtx ctx = {&u, y, Tn};
    gsl_multimin_function_fdf F = { mle_f, mle_df, mle_fdf, (size_t)u.ntheta, &ctx };
    double th[8]; double ll = -1e300;
    double starts[3][8];
    int nstarts = u.ntheta>1 ? 3 : 1;
    memcpy(starts[0],th0,sizeof th0);
    for(int i=0;i<u.ntheta;i++){ starts[1][i]=th0[i]-2.0; starts[2][i]=th0[i]; }
    if(u.i_eps>=0){ starts[1][u.i_eps]=th0[u.i_eps];       /* small states, big eps */
                    starts[2][u.i_eps]=th0[u.i_eps]-3.0; } /* small eps, big states */
    for(int s0=0;s0<nstarts;s0++){
        gsl_multimin_fdfminimizer *s =
            gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_vector_bfgs2,
                                            u.ntheta);
        gsl_vector *x0 = gsl_vector_alloc(u.ntheta);
        for(int i=0;i<u.ntheta;i++) gsl_vector_set(x0,i,starts[s0][i]);
        gsl_multimin_fdfminimizer_set(s,&F,x0,0.1,1e-4);
        int iter=0, status=GSL_CONTINUE;
        double f_prev=1e300;
        while(status==GSL_CONTINUE && iter<300){
            iter++;
            status = gsl_multimin_fdfminimizer_iterate(s);
            if(status) break;
            double f_now = s->f;
            if(fabs(f_prev-f_now) < 1e-10*(1.0+fabs(f_now))) break;
            f_prev = f_now;
            status = gsl_multimin_test_gradient(s->gradient, 1e-7);
        }
        if(-s->f > ll){
            ll = -s->f;
            for(int i=0;i<u.ntheta;i++) th[i]=gsl_vector_get(s->x,i);
        }
        gsl_multimin_fdfminimizer_free(s);
        gsl_vector_free(x0);
    }
    /* OIM on theta by central-difference Hessian, delta method to sigma2 */
    double H[64];
    {
        double h=1e-4;
        int k=u.ntheta;
        for(int i=0;i<k;i++) for(int j=0;j<=i;j++){
            double tpp[8],tpm[8],tmp2[8],tmm[8];
            memcpy(tpp,th,sizeof th); memcpy(tpm,th,sizeof th);
            memcpy(tmp2,th,sizeof th); memcpy(tmm,th,sizeof th);
            tpp[i]+=h; tpp[j]+=h;  tpm[i]+=h; tpm[j]-=h;
            tmp2[i]-=h; tmp2[j]+=h; tmm[i]-=h; tmm[j]-=h;
            gsl_vector_view vp;
            double fpp,fpm,fmp,fmm;
            vp = gsl_vector_view_array(tpp,k); fpp = mle_f(&vp.vector,&ctx);
            vp = gsl_vector_view_array(tpm,k); fpm = mle_f(&vp.vector,&ctx);
            vp = gsl_vector_view_array(tmp2,k); fmp = mle_f(&vp.vector,&ctx);
            vp = gsl_vector_view_array(tmm,k); fmm = mle_f(&vp.vector,&ctx);
            double hij = (fpp-fpm-fmp+fmm)/(4*h*h);
            H[(size_t)i*k+j]=H[(size_t)j*k+i]=hij;
        }
        LAPACKE_dpotrf(LAPACK_COL_MAJOR,'U',k,H,k);
        LAPACKE_dpotri(LAPACK_COL_MAJOR,'U',k,H,k);
        for(int i=0;i<k;i++) for(int j=i+1;j<k;j++) H[(size_t)i*k+j]=H[(size_t)j*k+i];
    }
    long d=0, nsteps=0;
    SSModel M = ucm_model(&u,th);
    ll = ss_loglik(&M,y,Tn,&d,&nsteps);
    /* ---- output ---- */
    static const char *mnames[] = {"No trend","Local level","Local linear trend",
                                   "Random walk","Random walk with drift"};
    if(!c->quiet){
        printf("Unobserved-components model\n");
        printf("Components: %s%s%s\n", mnames[u.model],
               u.s>1? " + seasonal (dummy)":"",
               u.cyc? " + cycle (stochastic, order 1)":"");
        printf("\nSample: %ld obs (%ld missing inside range)      Number of obs   = %8ld\n",
               Tn, Tn-nobs, nobs);
        printf("Log likelihood = %.5f                     Diffuse steps   = %8ld\n", ll, d);
        printf("------------------------------------------------------------------------------\n");
        printf("%12.12s | Coefficient  Std. err.      z    P>|z|     [95%% conf. interval]\n",
               yv->name);
        printf("-------------+----------------------------------------------------------------\n");
    }
    double zc = gsl_cdf_ugaussian_Pinv(0.975);
    /* table labels are Stata's; posted _b names are paren-free.
     * kind 0 = variance (exp(2 psi)), 1 = logit(0,1), 2 = logit(0,pi) */
    const char *labs[8]; const char *enames[8]; int idxs[8], kind[8]; int nrow=0;
    if(u.cyc){
        labs[nrow]="damping";   enames[nrow]="cycle_rho";  kind[nrow]=1; idxs[nrow++]=u.i_crho;
        labs[nrow]="frequency"; enames[nrow]="cycle_freq"; kind[nrow]=2; idxs[nrow++]=u.i_clam;
    }
    if(u.i_level>=0){ labs[nrow]="var(level)"; enames[nrow]="var_level"; kind[nrow]=0; idxs[nrow++]=u.i_level; }
    if(u.i_slope>=0){ labs[nrow]="var(slope)"; enames[nrow]="var_slope"; kind[nrow]=0; idxs[nrow++]=u.i_slope; }
    if(u.i_seas>=0){  labs[nrow]="var(seas)";  enames[nrow]="var_seas";  kind[nrow]=0; idxs[nrow++]=u.i_seas; }
    if(u.cyc){        labs[nrow]="var(cycle)"; enames[nrow]="var_cycle"; kind[nrow]=0; idxs[nrow++]=u.i_cvar; }
    if(u.i_eps>=0){   labs[nrow]="var(e)";     enames[nrow]="var_e";     kind[nrow]=0; idxs[nrow++]=u.i_eps; }
    double bpost[8], Vpost[64]; memset(Vpost,0,sizeof Vpost);
    char xn[8][33];
    for(int rw=0;rw<nrow;rw++){
        int i0=idxs[rw];
        double val, dder;
        if(kind[rw]==0){ val = exp(2.0*th[i0]); dder = 2.0*val; }
        else if(kind[rw]==1){ val = 1.0/(1.0+exp(-th[i0])); dder = val*(1.0-val); }
        else { val = M_PI/(1.0+exp(-th[i0])); dder = val*(1.0-val/M_PI); }
        double se = fabs(dder)*sqrt(H[(size_t)i0*u.ntheta+i0]);
        double z = se>0 ? val/se : 0;
        if(!c->quiet)
            printf("%12.12s | %10.6g  %10.6g %7.2f %6.3f    %10.6g  %10.6g\n",
                   labs[rw], val, se, z, se>0?2.0*(1.0-gsl_cdf_ugaussian_P(fabs(z))):1.0,
                   val-zc*se, val+zc*se);
        bpost[rw]=val; Vpost[(size_t)rw*nrow+rw]=se*se;
        snprintf(xn[rw],33,"%s",enames[rw]);
    }
    if(!c->quiet){
        printf("------------------------------------------------------------------------------\n");
        printf("Note: variance tests are one-sided; values on the boundary print z against 0.\n");
    }
    /* smstate(NEW): smoothed level (first state) */
    char smv[65]="";
    if(opt_value(c->options,"smstate",smv,sizeof smv) && smv[0]){
        if(var_find(c->f,smv)>=0) tea_err("ucm: %s already defined (smstate skipped)\n",smv);
        else if(u.model==UCM_NTREND) tea_err("ucm: no level state in model(ntrend)\n");
        else {
            double *ahat = malloc((size_t)Tn*u.m*sizeof(double));
            ss_smooth(&M,y,Tn,ahat,NULL,NULL,NULL);
            Variable *nv2 = var_add(c->f,smv,VT_NUM);
            for(size_t r=0;r<c->f->nobs;r++) nv2->num[r]=SV_MISS;
            for(long t=0;t<Tn;t++) nv2->num[first+t]=ahat[(size_t)t*u.m + 0];
            free(ahat);
            if(!c->quiet) printf("(smoothed level saved as %s)\n", smv);
        }
    }
    /* estimates post */
    {
        Estimates *ee = est_new();
        snprintf(ee->cmd,16,"ucm");
        snprintf(ee->depvar,33,"%s",yv->name);
        ee->K=nrow;
        ee->xnames=calloc(nrow,33);
        for(int rw2=0;rw2<nrow;rw2++) snprintf(ee->xnames[rw2],33,"%s",xn[rw2]);
        ee->omitted=calloc(nrow,sizeof(int));
        ee->b=malloc((size_t)nrow*sizeof(double));
        memcpy(ee->b,bpost,(size_t)nrow*sizeof(double));
        ee->V=malloc((size_t)nrow*nrow*sizeof(double));
        memcpy(ee->V,Vpost,(size_t)nrow*nrow*sizeof(double));
        ee->N=nobs; ee->df_r=(int)(nobs-nrow); ee->df_m=nrow; ee->has_cons=0;
        ee->se_kind=SE_CLASSICAL;
        ee->nobs_at_fit=c->f->nobs;
        ee->used=calloc(c->f->nobs,1);
        for(long t=0;t<Tn;t++) if(!isnan(y[t])) ee->used[first+t]=1;
        snprintf(ee->fitted_frame,33,"%s",c->f->name);
        est_free(c->ws->last_est);
        c->ws->last_est=ee;
        store_coef_macros(ee, &c->ip->rret);
        char bb[32];
        snprintf(bb, sizeof bb, "%.10g", ll);
        mac_set(&c->ip->rret, "e(ll)", bb);
    }
    free(y); ucm_free(&u); tsop_drop_temps(c->f,ntp);
    return 0;
}

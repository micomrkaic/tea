/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kalman.c — the state-space engine (DESIGN_SSPACE.md).
 * DK form: y_t = Z a_t + eps, a_{t+1} = T a_t + R eta.
 * Univariate (sequential) filtering with exact diffuse initialization
 * (Koopman 1997; Koopman & Durbin 2000); DK backward smoother for the
 * smoothed state mean including the diffuse-period (r0, r1) extension.
 * Smoothed variances: standard-phase N recursion (release-one scope;
 * verified against statsmodels on the shipped models — see the design
 * note's addendum commitment).
 * No Stata parsing, no printing — front-ends live in sspace.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kalman.h"
#include "linalg.h"

#define SS_TOL_INF 1e-8
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* P <- T P T' + add (m x m); tmp: m x m workspace */
static void quad_update(double *P, const double *T, int m, const double *add,
                        double *tmp){
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, m, m, m,
                1.0, T, m, P, m, 0.0, tmp, m);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, m, m, m,
                1.0, tmp, m, T, m, 0.0, P, m);
    if(add) for(int i=0;i<m*m;i++) P[i] += add[i];
}

int ss_lyapunov(const double *T, const double *RQR, int m, double *P){
    int mm = m*m;
    double *A = calloc((size_t)mm*mm, sizeof(double));
    if(!A) return 1;
    for(int c1=0;c1<m;c1++) for(int c2=0;c2<m;c2++)
        for(int r1=0;r1<m;r1++) for(int r2=0;r2<m;r2++){
            int row = r1 + m*r2, col = c1 + m*c2;
            A[(size_t)col*mm + row] = ((row==col)?1.0:0.0)
                - T[(size_t)c1*m + r1]*T[(size_t)c2*m + r2];
        }
    memcpy(P, RQR, (size_t)mm*sizeof(double));
    int *ipiv = malloc((size_t)mm*sizeof(int));
    int info = LAPACKE_dgesv(LAPACK_COL_MAJOR, mm, 1, A, mm, ipiv, P, mm);
    free(A); free(ipiv);
    return info != 0;
}

static void build_RQR(const SSModel *M, double *RQR){
    double *RQ = malloc((size_t)M->m*M->r*sizeof(double));
    cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,M->m,M->r,M->r,
                1.0,M->R,M->m,M->Q,M->r,0.0,RQ,M->m);
    cblas_dgemm(CblasColMajor,CblasNoTrans,CblasTrans,M->m,M->m,M->r,
                1.0,RQ,M->m,M->R,M->m,0.0,RQR,M->m);
    free(RQ);
}

int ss_filter(const SSModel *M, const double *y, long Tn, SSFilterOut *out){
    int m = M->m, p = M->p;
    double *a  = malloc((size_t)m*sizeof(double));
    double *Ps = malloc((size_t)m*m*sizeof(double));
    double *Pi = malloc((size_t)m*m*sizeof(double));
    double *tmp= malloc((size_t)m*m*sizeof(double));
    double *Mv = malloc((size_t)m*sizeof(double));
    double *Mi = malloc((size_t)m*sizeof(double));
    double *tv = malloc((size_t)m*sizeof(double));
    double *RQR= calloc((size_t)m*m,sizeof(double));
    if(!a||!Ps||!Pi||!tmp||!Mv||!Mi||!tv||!RQR){
        free(a);free(Ps);free(Pi);free(tmp);free(Mv);free(Mi);free(tv);free(RQR);
        return 1;
    }
    build_RQR(M, RQR);
    memcpy(a,  M->a1,     (size_t)m*sizeof(double));
    memcpy(Ps, M->Pstar1, (size_t)m*m*sizeof(double));
    memcpy(Pi, M->Pinf1,  (size_t)m*m*sizeof(double));
    double pinf_scale = 0;
    for(int i=0;i<m*m;i++) if(fabs(Pi[i])>pinf_scale) pinf_scale=fabs(Pi[i]);
    int diffuse = pinf_scale > 0;
    double ll = 0; long d = 0, nsteps = 0;
    for(long t=0;t<Tn;t++){
        for(int i=0;i<p;i++){
            double yti = y[(size_t)i*Tn + t];
            if(isnan(yti)) continue;
            const double *Zi = M->Z + i;              /* row i, stride p */
            double v = yti;
            for(int j=0;j<m;j++) v -= Zi[(size_t)j*p]*a[j];
            for(int r2=0;r2<m;r2++){
                double s=0, si=0;
                for(int j=0;j<m;j++){
                    s  += Ps[(size_t)j*m + r2]*Zi[(size_t)j*p];
                    si += Pi[(size_t)j*m + r2]*Zi[(size_t)j*p];
                }
                Mv[r2]=s; Mi[r2]=si;
            }
            double Fst = M->Hdiag[i];
            for(int j=0;j<m;j++) Fst += Zi[(size_t)j*p]*Mv[j];
            if(diffuse){
                double Finf = 0;
                for(int j=0;j<m;j++) Finf += Zi[(size_t)j*p]*Mi[j];
                if(Finf > SS_TOL_INF*pinf_scale){
                    for(int r2=0;r2<m;r2++) a[r2] += Mi[r2]/Finf*v;
                    for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++){
                        double K0r = Mi[r2]/Finf, K0c = Mi[c2]/Finf;
                        Ps[(size_t)c2*m+r2] += K0r*K0c*Fst
                                             - K0r*Mv[c2] - Mv[r2]*K0c;
                        Pi[(size_t)c2*m+r2] -= K0r*Mi[c2];
                    }
                    ll += -0.5*(log(2*M_PI) + log(Finf));
                    d++; nsteps++;
                    double mx=0;
                    for(int q2=0;q2<m*m;q2++) if(fabs(Pi[q2])>mx) mx=fabs(Pi[q2]);
                    if(mx <= SS_TOL_INF*pinf_scale){
                        diffuse=0;
                        memset(Pi,0,(size_t)m*m*sizeof(double));
                    }
                    continue;
                }
            }
            if(Fst <= 0) continue;
            for(int r2=0;r2<m;r2++) a[r2] += Mv[r2]/Fst*v;
            for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++)
                Ps[(size_t)c2*m+r2] -= Mv[r2]*Mv[c2]/Fst;
            ll += -0.5*(log(2*M_PI) + log(Fst) + v*v/Fst);
            nsteps++;
        }
        cblas_dgemv(CblasColMajor,CblasNoTrans,m,m,1.0,M->T,m,a,1,0.0,tv,1);
        memcpy(a, tv, (size_t)m*sizeof(double));
        quad_update(Ps, M->T, m, RQR, tmp);
        if(diffuse) quad_update(Pi, M->T, m, NULL, tmp);
        if(out && out->at){
            memcpy(out->at + (size_t)t*m, a, (size_t)m*sizeof(double));
            if(out->Pt)
                memcpy(out->Pt + (size_t)t*m*m, Ps, (size_t)m*m*sizeof(double));
        }
    }
    if(out){ out->loglik = ll; out->d = d; out->nsteps = nsteps; }
    free(a);free(Ps);free(Pi);free(tmp);free(Mv);free(Mi);free(tv);free(RQR);
    return 0;
}

int ss_smooth(const SSModel *M, const double *y, long Tn,
              double *ahat, double *Vt, double *loglik_out, long *d_out){
    int m = M->m, p = M->p;
    double *A0 = malloc((size_t)Tn*m*sizeof(double));
    double *PS = malloc((size_t)Tn*m*m*sizeof(double));
    double *PI = malloc((size_t)Tn*m*m*sizeof(double));
    double *vv = malloc((size_t)Tn*p*sizeof(double));
    double *FF = malloc((size_t)Tn*p*sizeof(double));
    double *FI = malloc((size_t)Tn*p*sizeof(double));
    double *KK = malloc((size_t)Tn*p*m*sizeof(double));
    double *KI = malloc((size_t)Tn*p*m*sizeof(double));
    unsigned char *OB = calloc((size_t)Tn*p,1);
    double *a  = malloc((size_t)m*sizeof(double));
    double *Ps = malloc((size_t)m*m*sizeof(double));
    double *Pi = malloc((size_t)m*m*sizeof(double));
    double *tmp= malloc((size_t)m*m*sizeof(double));
    double *RQR= calloc((size_t)m*m,sizeof(double));
    build_RQR(M, RQR);
    memcpy(a,M->a1,(size_t)m*sizeof(double));
    memcpy(Ps,M->Pstar1,(size_t)m*m*sizeof(double));
    memcpy(Pi,M->Pinf1,(size_t)m*m*sizeof(double));
    double pinf_scale=0;
    for(int i=0;i<m*m;i++) if(fabs(Pi[i])>pinf_scale) pinf_scale=fabs(Pi[i]);
    int diffuse = pinf_scale>0;
    double ll=0; long d=0;
    for(long t=0;t<Tn;t++){
        memcpy(A0+(size_t)t*m, a, (size_t)m*sizeof(double));
        memcpy(PS+(size_t)t*m*m, Ps, (size_t)m*m*sizeof(double));
        memcpy(PI+(size_t)t*m*m, Pi, (size_t)m*m*sizeof(double));
        for(int i=0;i<p;i++){
            double yti = y[(size_t)i*Tn + t];
            if(isnan(yti)) continue;
            const double *Zi = M->Z + i;
            double v=yti; for(int j=0;j<m;j++) v -= Zi[(size_t)j*p]*a[j];
            double *Mv = KK + ((size_t)t*p+i)*m;
            double *Mi = KI + ((size_t)t*p+i)*m;
            for(int r2=0;r2<m;r2++){
                double s=0,si=0;
                for(int j=0;j<m;j++){
                    s  += Ps[(size_t)j*m+r2]*Zi[(size_t)j*p];
                    si += Pi[(size_t)j*m+r2]*Zi[(size_t)j*p];
                }
                Mv[r2]=s; Mi[r2]=si;
            }
            double Fst=M->Hdiag[i];
            for(int j=0;j<m;j++) Fst += Zi[(size_t)j*p]*Mv[j];
            vv[(size_t)t*p+i]=v; FF[(size_t)t*p+i]=Fst; FI[(size_t)t*p+i]=0;
            if(diffuse){
                double Finf=0;
                for(int j=0;j<m;j++) Finf += Zi[(size_t)j*p]*Mi[j];
                FI[(size_t)t*p+i]=Finf;
                if(Finf > SS_TOL_INF*pinf_scale){
                    OB[(size_t)t*p+i]=2;
                    for(int r2=0;r2<m;r2++) a[r2] += Mi[r2]/Finf*v;
                    for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++){
                        double K0r=Mi[r2]/Finf, K0c=Mi[c2]/Finf;
                        Ps[(size_t)c2*m+r2] += K0r*K0c*Fst - K0r*Mv[c2] - Mv[r2]*K0c;
                        Pi[(size_t)c2*m+r2] -= K0r*Mi[c2];
                    }
                    ll += -0.5*(log(2*M_PI)+log(Finf));
                    d++;
                    double mx=0;
                    for(int q2=0;q2<m*m;q2++) if(fabs(Pi[q2])>mx) mx=fabs(Pi[q2]);
                    if(mx<=SS_TOL_INF*pinf_scale){ diffuse=0; memset(Pi,0,(size_t)m*m*sizeof(double)); }
                    continue;
                }
            }
            if(Fst<=0) continue;
            OB[(size_t)t*p+i]=1;
            for(int r2=0;r2<m;r2++) a[r2] += Mv[r2]/Fst*v;
            for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++)
                Ps[(size_t)c2*m+r2] -= Mv[r2]*Mv[c2]/Fst;
            ll += -0.5*(log(2*M_PI)+log(Fst)+v*v/Fst);
        }
        double *tv = tmp;   /* reuse first column as temp vector */
        cblas_dgemv(CblasColMajor,CblasNoTrans,m,m,1.0,M->T,m,a,1,0.0,tv,1);
        memcpy(a,tv,(size_t)m*sizeof(double));
        quad_update(Ps,M->T,m,RQR,tmp);
        if(diffuse) quad_update(Pi,M->T,m,NULL,tmp);
    }
    /* -------- backward pass -------- */
    double *r0=calloc(m,sizeof(double)), *r1=calloc(m,sizeof(double));
    double *N0=calloc((size_t)m*m,sizeof(double));
    double *w = malloc((size_t)m*sizeof(double));
    double *w2= malloc((size_t)m*sizeof(double));
    double *Lw= malloc((size_t)m*m*sizeof(double));
    for(long t=Tn-1;t>=0;t--){
        if(t<Tn-1){
            cblas_dgemv(CblasColMajor,CblasTrans,m,m,1.0,M->T,m,r0,1,0.0,w,1);
            memcpy(r0,w,(size_t)m*sizeof(double));
            cblas_dgemv(CblasColMajor,CblasTrans,m,m,1.0,M->T,m,r1,1,0.0,w,1);
            memcpy(r1,w,(size_t)m*sizeof(double));
            cblas_dgemm(CblasColMajor,CblasTrans,CblasNoTrans,m,m,m,
                        1.0,M->T,m,N0,m,0.0,Lw,m);
            cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,m,m,m,
                        1.0,Lw,m,M->T,m,0.0,N0,m);
        }
        for(int i=p-1;i>=0;i--){
            unsigned char ob = OB[(size_t)t*p+i];
            if(!ob) continue;
            const double *Zi = M->Z + i;
            double v = vv[(size_t)t*p+i];
            double Fst = FF[(size_t)t*p+i];
            const double *Mv = KK + ((size_t)t*p+i)*m;
            const double *Mi = KI + ((size_t)t*p+i)*m;
            if(ob==1){
                double mr=0, mr1=0;
                for(int j=0;j<m;j++){ mr += Mv[j]*r0[j]; mr1 += Mv[j]*r1[j]; }
                for(int j=0;j<m;j++){
                    w[j]  = r0[j] + Zi[(size_t)j*p]*((v - mr)/Fst);
                    w2[j] = r1[j] - Zi[(size_t)j*p]*(mr1/Fst);
                }
                memcpy(r0,w,(size_t)m*sizeof(double));
                memcpy(r1,w2,(size_t)m*sizeof(double));
                if(Vt){
                    double *K = w; for(int j=0;j<m;j++) K[j]=Mv[j]/Fst;
                    double *u = w2;
                    cblas_dgemv(CblasColMajor,CblasNoTrans,m,m,1.0,N0,m,K,1,0.0,u,1);
                    double kNk=0; for(int j=0;j<m;j++) kNk += K[j]*u[j];
                    for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++){
                        double zr2=Zi[(size_t)r2*p], zc=Zi[(size_t)c2*p];
                        N0[(size_t)c2*m+r2] += -zr2*u[c2] - u[r2]*zc
                                             + zr2*zc*kNk + zr2*zc/Fst;
                    }
                }
            } else {
                double Finf = FI[(size_t)t*p+i];
                double mir0=0, mir1=0;
                for(int j=0;j<m;j++){ mir0 += Mi[j]*r0[j]; mir1 += Mi[j]*r1[j]; }
                double k1r0 = 0;
                for(int j=0;j<m;j++)
                    k1r0 += (Mv[j]/Finf - Mi[j]*Fst/(Finf*Finf))*r0[j];
                for(int j=0;j<m;j++){
                    w2[j] = r1[j] - Zi[(size_t)j*p]*(mir1/Finf)
                          + Zi[(size_t)j*p]*(v/Finf) - Zi[(size_t)j*p]*k1r0;
                    w[j]  = r0[j] - Zi[(size_t)j*p]*(mir0/Finf);
                }
                memcpy(r1,w2,(size_t)m*sizeof(double));
                memcpy(r0,w,(size_t)m*sizeof(double));
                if(Vt){
                    double *K = w; for(int j=0;j<m;j++) K[j]=Mi[j]/Finf;
                    double *u = w2;
                    cblas_dgemv(CblasColMajor,CblasNoTrans,m,m,1.0,N0,m,K,1,0.0,u,1);
                    double kNk=0; for(int j=0;j<m;j++) kNk += K[j]*u[j];
                    for(int r2=0;r2<m;r2++) for(int c2=0;c2<m;c2++){
                        double zr2=Zi[(size_t)r2*p], zc=Zi[(size_t)c2*p];
                        N0[(size_t)c2*m+r2] += -zr2*u[c2] - u[r2]*zc + zr2*zc*kNk;
                    }
                }
            }
        }
        const double *a0 = A0 + (size_t)t*m;
        const double *ps = PS + (size_t)t*m*m;
        const double *pi = PI + (size_t)t*m*m;
        for(int j=0;j<m;j++){
            double s=a0[j];
            for(int k2=0;k2<m;k2++)
                s += ps[(size_t)k2*m+j]*r0[k2] + pi[(size_t)k2*m+j]*r1[k2];
            ahat[(size_t)t*m + j] = s;
        }
        if(Vt){
            double *PN = Lw;
            cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,m,m,m,
                        1.0,(const double*)ps,m,N0,m,0.0,PN,m);
            double *V = Vt + (size_t)t*m*m;
            cblas_dgemm(CblasColMajor,CblasNoTrans,CblasNoTrans,m,m,m,
                        -1.0,PN,m,(const double*)ps,m,0.0,V,m);
            for(int q2=0;q2<m*m;q2++) V[q2] += ps[q2];
        }
    }
    if(loglik_out) *loglik_out = ll;
    if(d_out) *d_out = d;
    free(A0);free(PS);free(PI);free(vv);free(FF);free(FI);free(KK);free(KI);
    free(OB);free(a);free(Ps);free(Pi);free(tmp);free(RQR);
    free(r0);free(r1);free(N0);free(w);free(w2);free(Lw);
    return 0;
}

double ss_loglik(const SSModel *M, const double *y, long Tn, long *d, long *nsteps){
    SSFilterOut out = {0};
    if(ss_filter(M, y, Tn, &out)) return NAN;
    if(d) *d = out.d;
    if(nsteps) *nsteps = out.nsteps;
    return out.loglik;
}

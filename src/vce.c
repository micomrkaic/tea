/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unified sandwich-VCE module.  Every robust/clustered VCE in tea is
 * V = c * B M B with estimator-supplied bread B and score rows s_i;
 * this file owns the meat, the finite-sample policy, the option
 * parsing, and the output furniture.  DESIGN_VCE.md is the contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vce.h"
#include "linalg.h"

int  opt_present(const char *opts, const char *name);
int  opt_value(const char *opts, const char *name, char *out, size_t out_sz);

int vce_parse(const char *opts, VceSpec *spec){
    memset(spec, 0, sizeof *spec);
    /* bare forms first (classic Stata) */
    if(opt_value(opts, "cluster", spec->cluster_var, sizeof spec->cluster_var)){
        spec->kind = VCE_CLUSTER;
        if(!spec->cluster_var[0]){ fprintf(stderr,"cluster(): variable required\n"); return 198; }
        return 0;
    }
    if(opt_present(opts, "robust")){ spec->kind = VCE_ROBUST; return 0; }
    /* vce(...) form */
    char v[80] = "";
    if(opt_value(opts, "vce", v, sizeof v)){
        if(!strcmp(v,"robust") || !strcmp(v,"hc1")){ spec->kind = VCE_ROBUST; return 0; }
        if(!strcmp(v,"ols") || !strcmp(v,"")){ spec->kind = VCE_DEFAULT; return 0; }
        if(!strncmp(v,"cluster",7)){
            const char *p = v + 7; while(*p==' ') p++;
            if(!*p){ fprintf(stderr,"vce(cluster VARNAME): variable required\n"); return 198; }
            snprintf(spec->cluster_var, sizeof spec->cluster_var, "%s", p);
            spec->kind = VCE_CLUSTER; return 0;
        }
        fprintf(stderr,"vce(%s): not supported (robust, cluster VARNAME)\n", v);
        return 198;
    }
    return 0;   /* VCE_DEFAULT */
}

double *vce_sandwich(const double *bread, const double *scores,
                     long N, int K, const long *cid, long G, double c){
    double *meat = calloc((size_t)K*K, sizeof(double));
    if(!meat) return NULL;
    if(!cid){
        /* robust: M = S'S */
        cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)N,
                    1.0, scores, (int)N, scores, (int)N, 0.0, meat, K);
    } else {
        /* cluster: M = U'U,  u_g = sum_{i in g} s_i */
        double *U = calloc((size_t)G*K, sizeof(double));
        if(!U){ free(meat); return NULL; }
        for(long i = 0; i < N; i++){
            long g = cid[i];
            for(int j = 0; j < K; j++)
                U[(size_t)j*G + g] += scores[(size_t)j*N + i];
        }
        cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)G,
                    1.0, U, (int)G, U, (int)G, 0.0, meat, K);
        free(U);
    }
    /* V = c * B M B */
    double *tmp = calloc((size_t)K*K, sizeof(double));
    double *V   = calloc((size_t)K*K, sizeof(double));
    if(!tmp || !V){ free(meat); free(tmp); free(V); return NULL; }
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                1.0, bread, K, meat, K, 0.0, tmp, K);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                1.0, tmp, K, bread, K, 0.0, V, K);
    for(size_t i = 0; i < (size_t)K*K; i++) V[i] *= c;
    free(meat); free(tmp);
    return V;
}

/* HAC (Newey-West/Bartlett) sandwich: V = c * B M B with
 * M = Omega_0 + sum_{l=1..L} w_l (Omega_l + Omega_l'),
 * w_l = 1 - l/(L+1), Omega_l = sum_t s_t s_{t-l}'.
 * scores must be in TIME ORDER with no gaps (caller enforces tsset). */
double *vce_hac(const double *bread, const double *scores,
                long N, int K, int L, double c){
    double *M = calloc((size_t)K*K, sizeof(double));
    if(!M) return NULL;
    /* Omega_0 = S'S */
    cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, K, K, (int)N,
                1.0, scores, (int)N, scores, (int)N, 0.0, M, K);
    for(int l = 1; l <= L && l < N; l++){
        double w = 1.0 - (double)l / (double)(L + 1);
        /* Omega_l[j][k] = sum_{t=l..N-1} s_t[j] s_{t-l}[k] */
        for(int j = 0; j < K; j++) for(int k2 = 0; k2 < K; k2++){
            double o = 0.0;
            const double *sj = scores + (size_t)j*N;
            const double *sk = scores + (size_t)k2*N;
            for(long t = l; t < N; t++) o += sj[t] * sk[t - l];
            M[(size_t)k2*K + j] += w * o;      /* Omega_l   */
            M[(size_t)j*K + k2] += w * o;      /* Omega_l'  */
        }
    }
    double *tmp = calloc((size_t)K*K, sizeof(double));
    double *V   = calloc((size_t)K*K, sizeof(double));
    if(!tmp || !V){ free(M); free(tmp); free(V); return NULL; }
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                1.0, bread, K, M, K, 0.0, tmp, K);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, K, K, K,
                1.0, tmp, K, bread, K, 0.0, V, K);
    for(size_t i = 0; i < (size_t)K*K; i++) V[i] *= c;
    free(M); free(tmp);
    return V;
}

/* Bartlett long-run variance of a single series (for pperron):
 * lrv = gamma_0 + 2 sum_{l=1..L} w_l gamma_l, gammas uncentered. */
double vce_lrvar(const double *e, long N, int L){
    double g0 = 0.0;
    for(long t = 0; t < N; t++) g0 += e[t]*e[t];
    g0 /= N;
    double lrv = g0;
    for(int l = 1; l <= L && l < N; l++){
        double w = 1.0 - (double)l / (double)(L + 1);
        double g = 0.0;
        for(long t = l; t < N; t++) g += e[t]*e[t-l];
        lrv += 2.0 * w * g / N;
    }
    return lrv;
}

double vce_adj_ls_robust(long N, int k){ return (double)N / (double)(N - k); }
double vce_adj_ls_cluster(long N, int k, long G){
    return ((double)G/(double)(G-1)) * ((double)(N-1)/(double)(N-k));
}
double vce_adj_ml_robust(long N){ return (double)N / (double)(N - 1); }
double vce_adj_ml_cluster(long G){ return (double)G / (double)(G - 1); }

const char *vce_se_header(const VceSpec *s){
    return s->kind == VCE_DEFAULT ? "Std. err." : "Robust\nstd. err.";
}
void vce_print_cluster_note(const char *cluster_var, long G){
    printf("                                          (Std. err. adjusted for %ld clusters in %s)\n",
           G, cluster_var);
}

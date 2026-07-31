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

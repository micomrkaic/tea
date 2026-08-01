/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * constraint.c — the constraints subsystem (DESIGN_DFACTOR.md §7).
 *
 * `constraint [define] # <lhs> = <rhs>` stores TEXT in the workspace;
 * `constraint list` / `constraint drop #|_all` manage it.  Parsing
 * against parameter names happens at ESTIMATION time (cns_build),
 * Stata's semantics: a constraint referencing parameters the model
 * doesn't have is an estimation-time error, not a definition error.
 *
 * cns_build turns a numlist of constraint ids plus the estimator's
 * parameter-name table into R theta = r.  cns_nullspace then produces
 * a minimum-norm particular solution theta_p (dgelsd) and an
 * orthonormal null-space basis N (dgesdd), so estimators can run
 * their unconstrained machinery on psi where theta = theta_p + N psi.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include "cmd.h"
#include "dataset.h"
#include <lapacke.h>

static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

/* ---- the command ------------------------------------------------------ */
static struct CnsDef **cns_find(Workspace *ws, int num){
    struct CnsDef **p = &ws->cns;
    while(*p && (*p)->num != num) p = &(*p)->next;
    return p;
}

int do_constraint(Cmd *c);
int do_constraint(Cmd *c)
{
    const char *s = c->varlist;
    while(isspace((unsigned char)*s)) s++;
    if(!strncmp(s,"define",6) && (isspace((unsigned char)s[6])||!s[6])) s += 6;
    while(isspace((unsigned char)*s)) s++;
    if(!strncmp(s,"list",4) && !s[4]){
        for(struct CnsDef *d = c->ws->cns; d; d = d->next)
            printf("%6d:  %s\n", d->num, d->text);
        return 0;
    }
    if(!strncmp(s,"drop",4)){
        s += 4;
        while(isspace((unsigned char)*s)) s++;
        if(!strcmp(s,"_all")){
            struct CnsDef *d = c->ws->cns;
            while(d){ struct CnsDef *n = d->next; free(d->text); free(d); d = n; }
            c->ws->cns = NULL;
            return 0;
        }
        int num = atoi(s);
        if(num <= 0){ tea_err("constraint drop: bad number\n"); return 198; }
        struct CnsDef **p = cns_find(c->ws, num);
        if(*p){ struct CnsDef *d = *p; *p = d->next; free(d->text); free(d); }
        return 0;
    }
    /* define: # text... */
    char *end;
    long num = strtol(s, &end, 10);
    if(end == s || num <= 0 || num > 1999){
        tea_err("constraint: expected `constraint [define] # expr = expr', `list', or `drop'\n");
        return 198;
    }
    s = end;
    while(isspace((unsigned char)*s)) s++;
    if(!*s || !strchr(s,'=')){
        tea_err("constraint %ld: definition must contain `='\n", num);
        return 198;
    }
    struct CnsDef **p = cns_find(c->ws, (int)num);
    if(*p){ free((*p)->text); (*p)->text = strdup(s); if(!(*p)->text) return 111; }
    else {
        struct CnsDef *d = malloc(sizeof *d);
        d->num = (int)num; d->text = strdup(s); d->next = NULL;
        /* keep list sorted by number for tidy `list` output */
        struct CnsDef **q = &c->ws->cns;
        while(*q && (*q)->num < d->num) q = &(*q)->next;
        d->next = *q; *q = d;
    }
    return 0;
}

/* ---- estimation-time parsing ------------------------------------------ */
/* Parameter names offered by the estimator: flat table of K strings,
 * matched either bare ("f1") or bracketed ("[y2]f1" is the name the
 * estimator itself registers, brackets included).  Matching is exact
 * on the registered spelling; bare spellings are also tried when the
 * constraint text omits brackets and the bare name is unambiguous. */
static int cns_name_idx(const char (*names)[33], int K, const char *tok){
    for(int i = 0; i < K; i++) if(!strcmp(names[i], tok)) return i;
    /* bare fallback: unique suffix match "...]tok" */
    int hit = -1;
    for(int i = 0; i < K; i++){
        const char *br = strchr(names[i], ']');
        if(br && !strcmp(br+1, tok)){
            if(hit >= 0) return -2;      /* ambiguous */
            hit = i;
        }
    }
    return hit;
}

/* Parse one side of a constraint into a row of coefficients + const.
 * Grammar: [+|-] term { (+|-) term } ; term = number [* nameref]
 *          | nameref ; nameref = [eqname]name | name                */
static int cns_side(const char *s, const char (*names)[33], int K,
                    double *row, double *cst, int sign0, int cnum){
    const char *p = s;
    int sign = sign0;
    int first = 1;
    while(1){
        while(isspace((unsigned char)*p)) p++;
        if(!*p) break;
        if(*p=='+'){ sign = sign0; p++; continue; }
        if(*p=='-'){ sign = -sign0; p++; continue; }
        double coef = 1.0;
        int have_num = 0;
        if(isdigit((unsigned char)*p) || *p=='.'){
            char *e; coef = strtod(p,&e); p = e; have_num = 1;
            while(isspace((unsigned char)*p)) p++;
            if(*p=='*'){ p++; while(isspace((unsigned char)*p)) p++; }
            else if(*p!='[' && !isalpha((unsigned char)*p) && *p!='_'){
                *cst += sign*coef;      /* bare number term */
                sign = sign0; first = 0; continue;
            }
        }
        /* nameref */
        char tok[80]; int ti = 0;
        if(*p=='['){
            while(*p && *p!=']' && ti < 78) tok[ti++] = *p++;
            if(*p==']') tok[ti++] = *p++;
        }
        while((isalnum((unsigned char)*p)||*p=='_'||*p=='.') && ti < 78)
            tok[ti++] = *p++;
        tok[ti] = 0;
        if(!ti){
            tea_err("constraint %d: cannot parse near `%s'\n", cnum, p);
            return 1;
        }
        int idx = cns_name_idx(names, K, tok);
        if(idx == -2){ tea_err("constraint %d: `%s' is ambiguous\n", cnum, tok); return 1; }
        if(idx < 0){
            tea_err("constraint %d: `%s' is not a parameter of this model\n", cnum, tok);
            return 1;
        }
        row[idx] += sign*coef;
        (void)have_num;
        sign = sign0; first = 0;
    }
    (void)first;
    return 0;
}

/* Build R (c x K, col-major) and r from the requested constraint
 * numbers.  numlist like 1 2 5 or 1/3 or mixtures.  Returns count in
 * nc; caller frees Rout/rout. */
int cns_build(Workspace *ws, const char *numlist, const char (*names)[33],
              int K, double **Rout, double **rout, int *nc);
int cns_build(Workspace *ws, const char *numlist, const char (*names)[33],
              int K, double **Rout, double **rout, int *nc)
{
    int nums[64]; int n = 0;
    const char *p = numlist;
    while(*p && n < 64){
        while(isspace((unsigned char)*p) || *p==',') p++;
        if(!*p) break;
        char *e; long a = strtol(p,&e,10);
        if(e==p){ tea_err("constraints(): bad numlist near `%s'\n", p); return 1; }
        p = e;
        if(*p=='/'){
            p++; long b = strtol(p,&e,10);
            if(e==p){ tea_err("constraints(): bad range\n"); return 1; }
            p = e;
            for(long v = a; v <= b && n < 64; v++) nums[n++] = (int)v;
        } else nums[n++] = (int)a;
    }
    if(!n){ tea_err("constraints(): empty\n"); return 1; }
    double *R = calloc((size_t)n*K, sizeof(double));
    double *r = calloc((size_t)n, sizeof(double));
    for(int i = 0; i < n; i++){
        struct CnsDef *d = *cns_find(ws, nums[i]);
        if(!d){ tea_err("constraint %d not defined\n", nums[i]);
                free(R); free(r); return 1; }
        char *eq = strchr(d->text, '=');
        char lhs[256], rhs[256];
        snprintf(lhs, sizeof lhs, "%.*s", (int)(eq - d->text), d->text);
        snprintf(rhs, sizeof rhs, "%s", eq+1);
        double *row = calloc((size_t)K, sizeof(double));
        double cst = 0;
        if(cns_side(lhs, names, K, row, &cst, +1, nums[i]) ||
           cns_side(rhs, names, K, row, &cst, -1, nums[i])){
            free(row); free(R); free(r); return 1;
        }
        /* row·theta + cst = 0  ->  row·theta = -cst */
        for(int k2 = 0; k2 < K; k2++) R[(size_t)k2*n + i] = row[k2];
        r[i] = -cst;
        free(row);
    }
    *Rout = R; *rout = r; *nc = n;
    return 0;
}

/* theta_p (min-norm solution of R theta = r) and orthonormal null
 * basis N (K x (K-rank)).  Returns rank or -1.  Caller frees *Nout. */
int cns_nullspace(const double *R, const double *r, int nc, int K,
                  double *theta_p, double **Nout, int *nfree);
int cns_nullspace(const double *R, const double *r, int nc, int K,
                  double *theta_p, double **Nout, int *nfree)
{
    /* theta_p via dgelsd on a copy */
    double *A = malloc((size_t)nc*K*sizeof(double));
    memcpy(A, R, (size_t)nc*K*sizeof(double));
    int ldb = K > nc ? K : nc;
    double *b = calloc((size_t)ldb, sizeof(double));
    memcpy(b, r, (size_t)nc*sizeof(double));
    double *sv = malloc((size_t)(nc<K?nc:K)*sizeof(double));
    int rank = 0;
    int info = LAPACKE_dgelsd(LAPACK_COL_MAJOR, nc, K, 1, A, nc, b, ldb,
                              sv, 1e-10, &rank);
    free(A);
    if(info){ free(b); free(sv); return -1; }
    memcpy(theta_p, b, (size_t)K*sizeof(double));
    free(b);
    /* null basis from full SVD of R: rows of V' beyond the rank */
    double *A2 = malloc((size_t)nc*K*sizeof(double));
    memcpy(A2, R, (size_t)nc*K*sizeof(double));
    int mn = nc < K ? nc : K;
    double *S = malloc((size_t)mn*sizeof(double));
    double *U = malloc((size_t)nc*nc*sizeof(double));
    double *VT = malloc((size_t)K*K*sizeof(double));
    info = LAPACKE_dgesdd(LAPACK_COL_MAJOR, 'A', nc, K, A2, nc,
                          S, U, nc, VT, K);
    free(A2); free(U); free(sv);
    if(info){ free(S); free(VT); return -1; }
    double tol = 1e-10 * (S[0] > 1 ? S[0] : 1);
    int rk = 0;
    for(int i = 0; i < mn; i++) if(S[i] > tol) rk++;
    int nf = K - rk;
    double *N = malloc((size_t)K*nf*sizeof(double));
    /* V' is K x K col-major; null vectors are rows rk..K-1 of V',
     * i.e. columns rk..K-1 of V: N[:,j] = VT[rk+j, :]' */
    for(int j = 0; j < nf; j++)
        for(int i2 = 0; i2 < K; i2++)
            N[(size_t)j*K + i2] = VT[(size_t)i2*K + (rk + j)];
    free(S); free(VT);
    *Nout = N; *nfree = nf;
    return rk;
}

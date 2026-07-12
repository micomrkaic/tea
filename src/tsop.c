/* tsop.c — shared TS-op varlist resolution.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) Mico Mrkaic.
 *
 * See tsop.h for the contract.  Internally this re-uses the expression
 * evaluator (N_TSOP) to compute lag values row-by-row.  Each TS-op
 * token in a varlist becomes one or more temporary frame columns whose
 * display name is the canonical "L.x" / "D2.y" / etc.
 */

#define _GNU_SOURCE
#include "tsop.h"
#include "factor.h"
#include "expr.h"
#include "value.h"
#include "cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ---- numlist & paren utilities ---------------------------------------- */

static const char *find_matching_paren(const char *p)
{
    if(*p != '(') return NULL;
    int depth = 0;
    for(; *p; p++){
        if(*p == '(') depth++;
        else if(*p == ')'){
            if(--depth == 0) return p;
        }
    }
    return NULL;
}

/* Parse Stata numlist: N | N1/N2 | N1(step)N2, whitespace-separated.
 * Appends to lags[], advancing *nl.  Returns 0 ok, -1 malformed/overflow. */
static int parse_numlist(char *body, int *lags, int *nl, int maxlags)
{
    char *sv = NULL;
    for(char *t = strtok_r(body, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv)){
        if(!isdigit((unsigned char)*t) && *t != '-') return -1;
        char *lp = strchr(t, '(');
        char *slash = strchr(t, '/');
        if(lp && (!slash || lp < slash)){
            char *rp = strchr(lp, ')');
            if(!rp || !rp[1]) return -1;
            *lp = 0; *rp = 0;
            int lo = atoi(t), step = atoi(lp + 1), hi = atoi(rp + 1);
            if(step == 0) return -1;
            if(step > 0){
                for(int v = lo; v <= hi; v += step){
                    if(*nl >= maxlags) return -1;
                    lags[(*nl)++] = v;
                }
            } else {
                for(int v = lo; v >= hi; v += step){
                    if(*nl >= maxlags) return -1;
                    lags[(*nl)++] = v;
                }
            }
            continue;
        }
        if(slash){
            *slash = 0;
            int lo = atoi(t), hi = atoi(slash+1);
            if(lo > hi){ int tmp = lo; lo = hi; hi = tmp; }
            for(int v = lo; v <= hi; v++){
                if(*nl >= maxlags) return -1;
                lags[(*nl)++] = v;
            }
            continue;
        }
        if(*nl >= maxlags) return -1;
        lags[(*nl)++] = atoi(t);
    }
    return 0;
}

/* Parse one varlist token as a TS-op form.  Fills lags[] and vars[]
 * (each var is a pointer into varbuf).  Returns:
 *    1 — matched as TS-op, filled lags/vars/op
 *    0 — not a TS-op token (caller falls through)
 *   -1 — error (*err set)
 *
 * varbuf must be at least 1024 bytes; the parsed variable names are
 * stored there and pointed at via vars[]. */
static int tsop_parse_token(Frame *f, const char *tok,
                            char *op_out, int *lags, int *nl,
                            const char **vars, int *nv,
                            char *varbuf, size_t varbuf_sz,
                            const char **err)
{
    *nl = 0; *nv = 0;
    if(!tok[0]) return 0;
    char k = (char)toupper((unsigned char)tok[0]);
    if(k != 'L' && k != 'F' && k != 'D' && k != 'S') return 0;
    char one[2] = {tok[0], 0};
    if(var_find(f, one) >= 0) return 0;

    const char *p = tok + 1;

    /* op spec */
    if(*p == '('){
        const char *rp = find_matching_paren(p);
        if(!rp) return 0;
        size_t blen = (size_t)(rp - p - 1);
        if(blen >= 128){ *err = "operator list too long"; return -1; }
        char body[128]; memcpy(body, p+1, blen); body[blen] = 0;
        if(parse_numlist(body, lags, nl, 64) < 0){
            *err = "bad numlist in operator"; return -1;
        }
        p = rp + 1;
    } else if(isdigit((unsigned char)*p)){
        int num = 0;
        while(isdigit((unsigned char)*p)){ num = num*10 + (*p - '0'); p++; }
        lags[(*nl)++] = num;
    } else {
        lags[(*nl)++] = 1;
    }
    if(*nl == 0){ *err = "empty operator list"; return -1; }

    /* '.' */
    if(*p != '.') return 0;
    p++;
    if(!*p) return 0;

    /* var spec */
    if(*p == '('){
        const char *rp = find_matching_paren(p);
        if(!rp) return 0;
        size_t vlen = (size_t)(rp - p - 1);
        if(vlen >= varbuf_sz){ *err = "variable list too long"; return -1; }
        memcpy(varbuf, p+1, vlen); varbuf[vlen] = 0;
        char *sv = NULL;
        for(char *t = strtok_r(varbuf, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv)){
            if(*nv < 64) vars[(*nv)++] = t;
        }
    } else {
        for(const char *q = p; *q; q++){
            if(!isalnum((unsigned char)*q) && *q != '_') return 0;
        }
        snprintf(varbuf, varbuf_sz, "%s", p);
        vars[0] = varbuf; *nv = 1;
    }
    if(*nv == 0){ *err = "empty variable list"; return -1; }
    *op_out = k;
    return 1;
}

/* ---- frame growth helpers --------------------------------------------- */

/* Append a new numeric column to the frame with the given display name.
 * Returns the new column index.  The column's num[] is allocated but
 * not initialized — caller fills it. */
static int append_temp_column(Frame *f, const char *name)
{
    if(f->nvar == f->cap_var){
        f->cap_var = f->cap_var ? f->cap_var * 2 : 8;
        f->vars = realloc(f->vars, f->cap_var * sizeof *f->vars);
    }
    Variable *v = &f->vars[f->nvar];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = VT_NUM;
    snprintf(v->format, sizeof v->format, "%%9.0g");
    v->num = malloc(f->nobs * sizeof(double));
    for(size_t i = 0; i < f->nobs; i++) v->num[i] = SV_MISS;
    v->cap = f->nobs;
    f->nvar++;
    return f->nvar - 1;
}

/* Build the canonical display name for an op+lag+var:
 *   lag=0           -> "varname"
 *   lag=1 or -1     -> "L.varname"
 *   else            -> "L#.varname" */
static void canonical_name(char op, int lag, const char *varname,
                           char *out, size_t out_sz)
{
    if(lag == 0){
        snprintf(out, out_sz, "%s", varname);
        return;
    }
    if(lag == 1 || lag == -1){
        snprintf(out, out_sz, "%c.%s", op, varname);
    } else {
        snprintf(out, out_sz, "%c%d.%s", op, abs(lag), varname);
    }
}

/* Materialize one TS-op as a temporary column.  Returns the new column's
 * index, or -1 on error (with *err set).  Uses expr_parse + expr_eval to
 * compute the values for each row. */
static int materialize_tsop(Frame *f, char op, int lag, const char *varname,
                            const char **err)
{
    char dispname[64];
    canonical_name(op, lag, varname, dispname, sizeof dispname);

    /* lag=0 → just reference the variable directly (no temp needed).
     * The column with that name already exists in the frame. */
    if(lag == 0){
        int vi = var_find_abbrev(f, varname);
        if(vi == -2){ *err = "ambiguous abbreviation"; return -1; }
        if(vi < 0){ *err = "variable not found"; return -1; }
        return vi;
    }

    if(f->ts_time < 0){
        *err = "time variable not set; use xtset or tsset first";
        return -1;
    }

    /* Build a tiny expression "L.x" / "L2.x" / etc and parse it. */
    char src[256];
    if(lag == 1 || lag == -1) snprintf(src, sizeof src, "%c.%s", op, varname);
    else                       snprintf(src, sizeof src, "%c%d.%s", op, abs(lag), varname);
    const char *perr = NULL;
    Node *e = expr_parse(src, f, &perr);
    if(!e || e->kind != N_TSOP){
        if(e) node_free(e);
        *err = "variable not found";
        return -1;
    }

    int idx = append_temp_column(f, dispname);

    EvalCtx ec = {0}; ec.f = f;
    for(size_t i = 0; i < f->nobs; i++){
        ec.i = i; ec.n = (long)i + 1; ec.N = (long)f->nobs;
        EVal ev = expr_eval(e, &ec);
        f->vars[idx].num[i] = ev.is_str ? SV_MISS : ev.num;
        eval_free(&ev);
    }
    tsidx_free(ec.tsidx); accs_free(&ec.accs);
    node_free(e);
    return idx;
}

/* ---- public API ------------------------------------------------------- */

int tsop_expand_varlist(Frame *f, const char *varlist,
                        int **out_indices, int *n_temps, const char **err)
{
    *err = NULL;
    *out_indices = NULL;
    *n_temps = 0;

    int original_nvar = f->nvar;
    int *indices = NULL; int n = 0, cap = 0;

    /* Paren-aware tokenizer (same as in regress's parse_regressors). */
    char buf[2048]; snprintf(buf, sizeof buf, "%s", varlist ? varlist : "");
    char *p = buf;
    while(*p){
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;
        char *tok = p;
        int depth = 0;
        while(*p && (depth > 0 || (*p != ' ' && *p != '\t'))){
            if(*p == '(') depth++;
            else if(*p == ')' && depth > 0) depth--;
            p++;
        }
        if(*p){ *p = 0; p++; }

        /* Try TS-op form. */
        char op = 0;
        int lags[64]; int nl = 0;
        const char *vars[64]; int nv = 0;
        char vbuf[1024];
        int rc = tsop_parse_token(f, tok, &op, lags, &nl, vars, &nv,
                                  vbuf, sizeof vbuf, err);
        if(rc < 0){
            free(indices);
            tsop_drop_temps(f, f->nvar - original_nvar);
            return -1;
        }
        if(rc > 0){
            /* Cross product: emit one column per (lag, var). */
            for(int i = 0; i < nl; i++){
                for(int j = 0; j < nv; j++){
                    int idx = materialize_tsop(f, op, lags[i], vars[j], err);
                    if(idx < 0){
                        free(indices);
                        tsop_drop_temps(f, f->nvar - original_nvar);
                        return -1;
                    }
                    if(n == cap){ cap = cap ? cap*2 : 16;
                                  indices = realloc(indices, cap*sizeof *indices); }
                    indices[n++] = idx;
                }
            }
            continue;
        }

        /* Factor-variable token (i.var, c.var, ib<n>.var, or any token
         * containing '#').  Dispatched here so all commands that go
         * through tsop_expand_varlist get factor support uniformly. */
        if(factor_is_factor_token(tok)){
            int *fi = NULL; int fn_temps = 0; const char *ferr = NULL;
            int got = factor_expand_token(f, tok, &fi, &fn_temps, &ferr);
            if(got < 0){
                *err = ferr ? ferr : "factor expansion failed";
                free(indices);
                tsop_drop_temps(f, f->nvar - original_nvar);
                return -1;
            }
            for(int k = 0; k < got; k++){
                if(n == cap){ cap = cap ? cap*2 : 16;
                              indices = realloc(indices, cap*sizeof *indices); }
                indices[n++] = fi[k];
            }
            free(fi);
            continue;
        }

        /* Plain token: use varlist_expand. */
        int *raw = NULL;
        int got = varlist_expand(f, tok, &raw);
        if(got < 0){
            *err = "variable not found";
            free(raw); free(indices);
            tsop_drop_temps(f, f->nvar - original_nvar);
            return -1;
        }
        for(int k = 0; k < got; k++){
            if(n == cap){ cap = cap ? cap*2 : 16;
                          indices = realloc(indices, cap*sizeof *indices); }
            indices[n++] = raw[k];
        }
        free(raw);
    }

    *out_indices = indices;
    *n_temps = f->nvar - original_nvar;
    return n;
}

void tsop_drop_temps(Frame *f, int n_temps)
{
    if(n_temps <= 0) return;
    for(int j = 0; j < n_temps; j++){
        int idx = f->nvar - 1 - j;
        if(idx < 0) break;
        Variable *v = &f->vars[idx];
        if(v->type == VT_NUM){
            free(v->num);
        } else if(v->str){
            for(size_t r = 0; r < f->nobs; r++) free(v->str[r]);
            free(v->str);
        }
        memset(v, 0, sizeof *v);
    }
    f->nvar -= n_temps;
}

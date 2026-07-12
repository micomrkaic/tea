/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic.  GPLv3.
 *
 * outreg2 emulation — the community regression-table exporter, as used
 * by ~90% of applied papers.  Supported grammar (the empirically common
 * subset):
 *
 *   outreg2 using FILE [, replace|append ctitle(txt) dec(#) bdec(#) se
 *            label symbol(s1, s2, s3) alpha(a1, a2, a3) excel
 *            addstat("Name", expr, ...) addtext(Key, Val, ...)
 *            addnote(txt) ]
 *
 * Each call appends one column (the current estimates) to an in-memory
 * table keyed by FILE and rewrites FILE as a tab-separated table (opens
 * cleanly in Excel; the `excel` flag is accepted as a no-op).  `replace`
 * resets the table; `append` (or nothing) adds a column.  Stars use the
 * t distribution with the model's residual df against the alpha() levels.
 */
#include "dataset.h"
#include "estimates.h"
#include "cmd.h"
#include "expr.h"
#include "value.h"


#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gsl/gsl_cdf.h>

/* local error printer (tea_err is static to commands.c) */
static void tea_err(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "outreg2: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define ORMAXCOL 64
#define ORMAXROW 256
#define ORMAXTXT 32

typedef struct {
    char name[64];               /* coefficient (canonical xname) */
} ORRow;

typedef struct {
    char   ctitle[128];
    char   depvar[33];
    int    K;
    char (*xnames)[33];
    double *b, *se;
    int    df_r;
    long   N;
    double r2;
    int    nstat; char statname[ORMAXTXT][64]; double statval[ORMAXTXT];
    int    ntext; char textkey[ORMAXTXT][64];  char textval[ORMAXTXT][128];
} ORCol;

typedef struct {
    char  fname[512];
    int   ncol;  ORCol col[ORMAXCOL];
    int   nrow;  ORRow row[ORMAXROW];
    char  note[512];
    int   dec, bdec, show_se, use_label;
    char  sym[3][8];
    double alev[3];
} ORTable;

static ORTable *g_tabs = NULL;
static int g_ntabs = 0, g_captabs = 0;

static ORTable *or_get(const char *fname, int replace){
    for(int i = 0; i < g_ntabs; i++)
        if(!strcmp(g_tabs[i].fname, fname)){
            if(replace){
                for(int c = 0; c < g_tabs[i].ncol; c++){
                    free(g_tabs[i].col[c].xnames);
                    free(g_tabs[i].col[c].b); free(g_tabs[i].col[c].se);
                }
                memset(&g_tabs[i], 0, sizeof(ORTable));
                snprintf(g_tabs[i].fname, sizeof g_tabs[i].fname, "%s", fname);
            }
            return &g_tabs[i];
        }
    if(g_ntabs == g_captabs){
        g_captabs = g_captabs ? g_captabs*2 : 4;
        g_tabs = realloc(g_tabs, (size_t)g_captabs * sizeof(ORTable));
    }
    ORTable *t = &g_tabs[g_ntabs++];
    memset(t, 0, sizeof *t);
    snprintf(t->fname, sizeof t->fname, "%s", fname);
    return t;
}

static int or_rowidx(ORTable *t, const char *name){
    for(int i = 0; i < t->nrow; i++)
        if(!strcmp(t->row[i].name, name)) return i;
    if(t->nrow >= ORMAXROW) return -1;
    snprintf(t->row[t->nrow].name, sizeof t->row[0].name, "%s", name);
    return t->nrow++;
}

/* pull a (...) option value out of the raw option string; handles nested
 * parens and quoted commas.  Returns 1 and fills out if present. */
static int or_opt(const char *opts, const char *key, char *out, size_t sz){
    size_t kl = strlen(key);
    const char *p = opts;
    while((p = strstr(p, key)) != NULL){
        if((p == opts || !isalnum((unsigned char)p[-1])) && p[kl] == '('){
            const char *q = p + kl + 1; int depth = 1; size_t w = 0;
            while(*q && depth){
                if(*q == '(') depth++;
                else if(*q == ')'){ if(--depth == 0) break; }
                if(w < sz-1) out[w++] = *q;
                q++;
            }
            out[w] = 0;
            return 1;
        }
        p += kl;
    }
    return 0;
}

static int or_flag(const char *opts, const char *key){
    size_t kl = strlen(key);
    const char *p = opts;
    while((p = strstr(p, key)) != NULL){
        int lb = (p == opts || !isalnum((unsigned char)p[-1]));
        int rb = !isalnum((unsigned char)p[kl]) && p[kl] != '(';
        if(lb && rb) return 1;
        p += kl;
    }
    return 0;
}

/* split "a, b, c" respecting quotes; strips quotes and blanks */
static int or_split(const char *s, char items[][128], int max){
    int n = 0; const char *p = s;
    while(*p && n < max){
        while(*p==' ')p++;
        char buf[128]; size_t w = 0; int inq = 0;
        while(*p && (inq || *p != ',')){
            if(*p=='"'){ inq = !inq; p++; continue; }
            if(w < sizeof buf - 1) buf[w++] = *p;
            p++;
        }
        while(w && buf[w-1]==' ') w--;
        buf[w] = 0;
        snprintf(items[n++], 128, "%s", buf);
        if(*p == ',') p++;
    }
    return n;
}

static void or_stars(char *dst, size_t sz, double b, double se, int df,
                     double a1, double a2, double a3,
                     const char *s1, const char *s2, const char *s3){
    dst[0] = 0;
    if(se <= 0 || df <= 0) return;
    double t = fabs(b / se);
    double p = 2.0 * gsl_cdf_tdist_Q(t, (double)df);
    if(p < a1)      snprintf(dst, sz, "%s", s1);
    else if(p < a2) snprintf(dst, sz, "%s", s2);
    else if(p < a3) snprintf(dst, sz, "%s", s3);
}

static const char *or_label_for(Frame *f, const char *xname, int use_label){
    if(use_label && f){
        int vi = var_find(f, xname);
        if(vi >= 0 && f->vars[vi].vlabel[0]) return f->vars[vi].vlabel;
    }
    if(!strcmp(xname, "_cons")) return "Constant";
    return xname;
}

static void or_render(ORTable *t, Frame *f){
    FILE *o = fopen(t->fname, "w");
    if(!o){ tea_err("cannot write %s\n", t->fname); return; }
    fprintf(o, "%s", t->use_label ? "VARIABLES" : "VARIABLES");
    for(int c = 0; c < t->ncol; c++)
        fprintf(o, "\t(%d) %s", c+1,
                t->col[c].ctitle[0] ? t->col[c].ctitle : t->col[c].depvar);
    fprintf(o, "\n");
    char stars[8];
    for(int r = 0; r < t->nrow; r++){
        fprintf(o, "%s", or_label_for(f, t->row[r].name, t->use_label));
        for(int c = 0; c < t->ncol; c++){
            ORCol *C = &t->col[c];
            int k = -1;
            for(int j = 0; j < C->K; j++)
                if(!strcmp(C->xnames[j], t->row[r].name)){ k = j; break; }
            if(k < 0){ fprintf(o, "\t"); continue; }
            or_stars(stars, sizeof stars, C->b[k], C->se[k], C->df_r,
                     t->alev[0], t->alev[1], t->alev[2],
                     t->sym[0], t->sym[1], t->sym[2]);
            fprintf(o, "\t%.*f%s", t->bdec, C->b[k], stars);
        }
        fprintf(o, "\n");
        if(t->show_se){
            fprintf(o, "%s", "");
            for(int c = 0; c < t->ncol; c++){
                ORCol *C = &t->col[c];
                int k = -1;
                for(int j = 0; j < C->K; j++)
                    if(!strcmp(C->xnames[j], t->row[r].name)){ k = j; break; }
                if(k < 0) fprintf(o, "\t");
                else fprintf(o, "\t(%.*f)", t->dec, C->se[k]);
            }
            fprintf(o, "\n");
        }
    }
    fprintf(o, "Observations");
    for(int c = 0; c < t->ncol; c++) fprintf(o, "\t%ld", t->col[c].N);
    fprintf(o, "\n");
    /* addstat rows: union across columns in first-seen order */
    char seen[ORMAXTXT][64]; int nseen = 0;
    for(int c = 0; c < t->ncol; c++)
        for(int s = 0; s < t->col[c].nstat; s++){
            int dup = 0;
            for(int m = 0; m < nseen; m++)
                if(!strcmp(seen[m], t->col[c].statname[s])) dup = 1;
            if(!dup && nseen < ORMAXTXT)
                snprintf(seen[nseen++], 64, "%s", t->col[c].statname[s]);
        }
    for(int m = 0; m < nseen; m++){
        fprintf(o, "%s", seen[m]);
        for(int c = 0; c < t->ncol; c++){
            double v = SV_MISS; ORCol *C = &t->col[c];
            for(int s = 0; s < C->nstat; s++)
                if(!strcmp(C->statname[s], seen[m])) v = C->statval[s];
            if(sv_is_miss(v)) fprintf(o, "\t");
            else fprintf(o, "\t%.*f", t->dec, v);
        }
        fprintf(o, "\n");
    }
    /* addtext rows, same union scheme */
    nseen = 0;
    for(int c = 0; c < t->ncol; c++)
        for(int s = 0; s < t->col[c].ntext; s++){
            int dup = 0;
            for(int m = 0; m < nseen; m++)
                if(!strcmp(seen[m], t->col[c].textkey[s])) dup = 1;
            if(!dup && nseen < ORMAXTXT)
                snprintf(seen[nseen++], 64, "%s", t->col[c].textkey[s]);
        }
    for(int m = 0; m < nseen; m++){
        fprintf(o, "%s", seen[m]);
        for(int c = 0; c < t->ncol; c++){
            const char *v = ""; ORCol *C = &t->col[c];
            for(int s = 0; s < C->ntext; s++)
                if(!strcmp(C->textkey[s], seen[m])) v = C->textval[s];
            fprintf(o, "\t%s", v);
        }
        fprintf(o, "\n");
    }
    if(t->note[0]) fprintf(o, "%s\n", t->note);
    fclose(o);
}

int do_outreg2(Cmd *c);
int do_outreg2(Cmd *c){
    Estimates *E = tea_last_estimates();
    if(!E){ tea_err("no estimation results\n"); return 301; }

    /* `using FILE` from the varlist part */
    char fname[512] = "";
    { const char *u = strstr(c->varlist, "using");
      if(!u){ tea_err("using FILE required\n"); return 198; }
      u += 5; while(*u==' ')u++;
      size_t w = 0; int inq = (*u=='"'); if(inq)u++;
      while(*u && w < sizeof fname - 1 && (inq ? *u!='"' : *u!=' ')) fname[w++]=*u++;
      fname[w]=0;
    }
    if(!fname[0]){ tea_err("using FILE required\n"); return 198; }

    const char *opts = c->options;
    int replace = or_flag(opts, "replace");
    ORTable *t = or_get(fname, replace);
    if(t->ncol >= ORMAXCOL){ tea_err("too many columns\n"); return 198; }

    /* table-level presentation options (last call wins, matching outreg2) */
    char v[512];
    t->dec  = or_opt(opts,"dec",v,sizeof v)  ? atoi(v) : 3;
    t->bdec = or_opt(opts,"bdec",v,sizeof v) ? atoi(v) : t->dec;
    t->show_se  = or_flag(opts,"se") ? 1 : t->show_se;
    t->use_label = or_flag(opts,"label") ? 1 : t->use_label;
    snprintf(t->sym[0],8,"***"); snprintf(t->sym[1],8,"**"); snprintf(t->sym[2],8,"*");
    if(or_opt(opts,"symbol",v,sizeof v)){
        char it[3][128]; int n = or_split(v,it,3);
        for(int i=0;i<n;i++) snprintf(t->sym[i],8,"%s",it[i]);
    }
    t->alev[0]=0.01; t->alev[1]=0.05; t->alev[2]=0.10;
    if(or_opt(opts,"alpha",v,sizeof v)){
        char it[3][128]; int n = or_split(v,it,3);
        for(int i=0;i<n;i++) t->alev[i]=atof(it[i]);
    }
    if(or_opt(opts,"addnote",v,sizeof v)){
        char it[1][128]; or_split(v,it,1);
        snprintf(t->note,sizeof t->note,"%s",it[0]);
    }

    /* the new column, from the current estimates */
    ORCol *C = &t->col[t->ncol++];
    memset(C, 0, sizeof *C);
    if(or_opt(opts,"ctitle",v,sizeof v)){
        char it[1][128]; or_split(v,it,1);
        snprintf(C->ctitle,sizeof C->ctitle,"%s",it[0]);
    }
    snprintf(C->depvar,33,"%s",E->depvar);
    C->K = E->K; C->df_r = E->df_r; C->N = E->N; C->r2 = E->r2;
    C->xnames = malloc((size_t)E->K * 33);
    C->b  = malloc((size_t)E->K * sizeof(double));
    C->se = malloc((size_t)E->K * sizeof(double));
    for(int k = 0; k < E->K; k++){
        memcpy(C->xnames[k], E->xnames[k], 33);
        C->b[k]  = E->b[k];
        C->se[k] = sqrt(E->V[k*E->K + k]);
        or_rowidx(t, E->xnames[k]);      /* register row order */
    }

    /* addstat("Name", expr, ...) — values are expressions (e() works) */
    if(or_opt(opts,"addstat",v,sizeof v)){
        char it[2*ORMAXTXT][128]; int n = or_split(v,it,2*ORMAXTXT);
        for(int i = 0; i + 1 < n && C->nstat < ORMAXTXT; i += 2){
            const char *pe; Node *a = expr_parse(it[i+1], c->f, &pe);
            double val = SV_MISS;
            if(a){ EvalCtx ec={0}; ec.f=c->f; ec.n=1; ec.N=(int)c->f->nobs;
                   EVal r = expr_eval(a,&ec);
                   if(!r.is_str) val = r.num;
                   eval_free(&r); node_free(a); }
            snprintf(C->statname[C->nstat],64,"%s",it[i]);
            C->statval[C->nstat++] = val;
        }
    }
    if(or_opt(opts,"addtext",v,sizeof v)){
        char it[2*ORMAXTXT][128]; int n = or_split(v,it,2*ORMAXTXT);
        for(int i = 0; i + 1 < n && C->ntext < ORMAXTXT; i += 2){
            snprintf(C->textkey[C->ntext],64,"%s",it[i]);
            snprintf(C->textval[C->ntext],128,"%s",it[i+1]);
            C->ntext++;
        }
    }

    or_render(t, c->f);
    if(!or_flag(opts,"noout"))
        printf("(outreg2: column %d written to %s)\n", t->ncol, fname);
    return 0;
}

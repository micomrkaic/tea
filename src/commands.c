#define _GNU_SOURCE
/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "cmd.h"
#include "interp.h"
#include "value.h"
#include "expr.h"
#include "dta.h"
#include "tsop.h"
#include "plot.h"
#include "sysdata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <stdarg.h>

/* ============================================================ MODULE
 *
 * commands.c — implementations of most tea commands + the dispatch
 * table that maps command names to functions.
 *
 * Layout (top to bottom):
 *
 *   1. Output infrastructure: g_logfp tee for `log using`, tea_printf
 *      and tea_err that route to both stdout/stderr AND the log file.
 *   2. Small parsing helpers: filename scanning, glob matching,
 *      option-list manipulation.
 *   3. Per-command implementations.  Each looks like:
 *         static int do_NAME(Cmd *c) { ... }
 *      They read c->varlist, c->options, c->ifexp, etc., do their work,
 *      and return a Stata-style return code (0 = success, others = error).
 *   4. The dispatch table at the bottom: an array of {name, fn, needs_data,
 *      help} structs that the interp.c command loop searches.
 *
 * When adding a new command:
 *   - Write the do_NAME function here (or in its own file if it's big,
 *     like ivregress / arima / estout)
 *   - Declare its prototype in cmd.h
 *   - Add it to the dispatch table near the bottom of this file
 *   - Add a regression test in tests/regression/
 *
 * Larger commands have been moved to their own files:
 *   regress.c   — regress, xtreg, hausman, logit, probit, poisson,
 *                 ivregress, predict, margins, test, lincom
 *   arima.c     — arima
 *   estcmd.c    — estimates store/restore/dir/drop/table
 *   estout.c    — estout
 *   factor.c    — factor-variable expansion (used internally by tsop.c)
 *   tsop.c      — TS-op + factor-var-aware varlist expander
 *   mle.c       — Newton-Raphson driver shared by logit/probit/poisson
 *
 * ==================================================================== */

/* ---- output capture: 'log using FILE' tees printf output to FILE -------- */
FILE *g_logfp = NULL;

/* Called from run_stream when a command is about to be executed.
 * In Stata, the log captures both commands (with '. ' prefix) and output. */
void tea_log_command(const char *line){
    if(!g_logfp) return;
    fprintf(g_logfp, ". %s\n", line);
    fflush(g_logfp);
}

__attribute__((format(__printf__,1,2)))
static int tea_printf(const char *fmt, ...){
    va_list ap;
    /* render once into a buffer; write to stdout and log */
    char small[2048]; char *buf = small; size_t sz = sizeof small;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    if(n >= (int)sz){
        sz = (size_t)n + 1; buf = malloc(sz);
        va_start(ap, fmt);
        vsnprintf(buf, sz, fmt, ap);
        va_end(ap);
    }
    fputs(buf, stdout);
    if(g_logfp){ fputs(buf, g_logfp); fflush(g_logfp); }
    if(buf != small) free(buf);
    return n;
}
__attribute__((format(__printf__,2,3)))
static int tea_fprintf(FILE *fp, const char *fmt, ...){
    /* fprintf to non-stdout streams (e.g. stderr) is NOT teed */
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    va_end(ap);
    if(fp == stdout && g_logfp){
        va_start(ap, fmt);
        char buf[4096]; vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fputs(buf, g_logfp); fflush(g_logfp);
    }
    return n;
}

/* Route every printf/fprintf(stdout,...) in this TU through the tee.
 * Defined AFTER stdio.h so the system prototypes are intact. */
#define printf  tea_printf
#define fprintf tea_fprintf

/* forward decl: load_csv_into is defined further down; used by import excel */
static int load_csv_into(Frame *f,const char *fn,char delim);

/* Display width of a UTF-8 string = number of codepoints (continuation
 * bytes 10xxxxxx don't advance the column).  Used to pad table columns
 * correctly for names like "C\u00f4te d'Ivoire"; printf's %*s pads by
 * BYTES, so we widen the field by (bytes - codepoints) to compensate. */
static int u8width(const char *s){
    int w = 0;
    for(const unsigned char *p=(const unsigned char*)s; *p; p++)
        if((*p & 0xC0) != 0x80) w++;
    return w;
}
static int u8pad(const char *s, int want){   /* field width for %*s */
    return want + (int)strlen(s) - u8width(s);
}


/* scan_filename: extract a filename from `*ps` into `out`, handling
 * Stata-style quoted paths.  Advances `*ps` past the filename and any
 * trailing whitespace.  Returns the number of chars consumed; 0 if no
 * filename was found.
 *
 *  - "foo bar.csv"     -> foo bar.csv          (quoted: spaces allowed)
 *  - 'foo bar.csv'     -> foo bar.csv          (single-quoted, Stata supports)
 *  - foo.csv           -> foo.csv              (bare: terminates on space/comma)
 *
 * The quote characters are NOT included in the output. */
static int scan_filename(const char **ps, char *out, size_t outsz){
    const char *s = *ps;
    while(*s == ' ' || *s == '\t') s++;
    if(!*s){ out[0] = 0; return 0; }
    size_t w = 0;
    if(*s == '"' || *s == '\''){
        char q = *s++;
        while(*s && *s != q && w < outsz-1) out[w++] = *s++;
        if(*s == q) s++;
    } else {
        while(*s && *s != ' ' && *s != '\t' && *s != ',' && *s != '\n' && w < outsz-1)
            out[w++] = *s++;
    }
    out[w] = 0;
    while(*s == ' ' || *s == '\t') s++;
    *ps = s;
    return (int)w;
}

/* estimation/postestimation handlers live in regress.c */
extern int do_regress(Cmd *c);
extern int do_predict(Cmd *c);
extern int do_test(Cmd *c);
extern int do_lincom(Cmd *c);

/* error helper: prefixes 'line N: ' in do-file mode, plain in REPL. */
extern int g_current_line;
#include <stdarg.h>
__attribute__((format(__printf__,1,2)))
static void tea_err(const char *fmt, ...){
    if(g_current_line) fprintf(stderr,"line %d: ", g_current_line);
    va_list ap; va_start(ap,fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* validate_wtype: returns 0 if the command's weight clause is OK, or a
 * Stata-style return code if the weight type isn't allowed here.
 *
 *   mask — bitmask of accepted weight types.  Bit (1<<wtype) is set for
 *          each accepted type.  Use the WTM_* macros below.
 *   cmdname — used only in the error message.
 *
 * No clause (c->wtype==0) is always OK. */
#define WTM_FW  (1u<<1)
#define WTM_AW  (1u<<2)
#define WTM_PW  (1u<<3)
#define WTM_IW  (1u<<4)
static int validate_wtype(Cmd *c, unsigned mask, const char *cmdname){
    if(c->wtype == 0) return 0;
    static const char *names[] = {NULL, "fweight", "aweight", "pweight", "iweight"};
    if(c->wtype < 1 || c->wtype > 4) return 0;  /* unknown — shouldn't happen */
    if(!(mask & (1u << c->wtype))){
        tea_err("%s: %s not allowed\n", cmdname, names[c->wtype]);
        return 101;
    }
    return 0;
}

/* ---- value formatting (honours %t* date formats) ----------------------- */
static const char *MON[]={"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
extern long  tea_civ_from(long sd,long*y,unsigned*m,unsigned*d); /* in eval.c? */
/* local copies of civil conversion (kept self-contained) */
static void civ_from(long sd,long*y,unsigned*m,unsigned*d){ long z=sd-3653+719468; long e=(z>=0?z:z-146096)/146097; unsigned doe=(unsigned)(z-e*146097); unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365; long yy=(long)yoe+e*400; unsigned doy=doe-(365*yoe+yoe/4-yoe/100); unsigned mp=(5*doy+2)/153; *d=doy-(153*mp+2)/5+1; *m=mp+(mp<10?3:-9); *y=yy+(*m<=2); }

static Workspace *g_ws;   /* set by run_command, used for value-label lookup */

static void fmt_cell(Variable *v,size_t i,char *out,size_t n){
    if(v->type==VT_STR){ snprintf(out,n,"%s",v->str[i]); return; }
    double x=v->num[i];
    if(sv_is_miss(x)){ int c=sv_miss_code(x); if(c==0)snprintf(out,n,"."); else snprintf(out,n,".%c",'a'+c-1); return; }
    if(v->vallab[0] && g_ws){
        const char *t=vlabel_lookup(g_ws,v->vallab,x);
        if(t){ snprintf(out,n,"%s",t); return; }
    }
    const char *f=v->format;
    if(!strncmp(f,"%td",3)){ long y;unsigned m,d;civ_from((long)x,&y,&m,&d); snprintf(out,n,"%02u%s%ld",d,MON[m-1],y); return; }
    if(!strncmp(f,"%tm",3)){ long mm=(long)x; long y=1960+mm/12; long m=mm%12; if(m<0){m+=12;y--;} snprintf(out,n,"%ldm%ld",y,m+1); return; }
    if(!strncmp(f,"%tq",3)){ long qq=(long)x; long y=1960+qq/4; long q=qq%4; if(q<0){q+=4;y--;} snprintf(out,n,"%ldq%ld",y,q+1); return; }
    if(!strncmp(f,"%tw",3)){ long ww=(long)x; long y=1960+ww/52; long w=ww%52; if(w<0){w+=52;y--;} snprintf(out,n,"%ldw%ld",y,w+1); return; }
    if(!strncmp(f,"%th",3)){ long hh=(long)x; long y=1960+hh/2; long h=hh%2; if(h<0){h+=2;y--;} snprintf(out,n,"%ldh%ld",y,h+1); return; }
    if(!strncmp(f,"%ty",3)){ snprintf(out,n,"%ld",(long)x); return; }
    /* Custom Stata-compatible numeric formats (%w.dF / %w.dG / %w.de etc.)
     * are printf-compatible.  If the variable has an explicit format and
     * it's not the default "%9.0g", use it directly.  Otherwise fall
     * back to the default heuristic (integers as %.0f, others as %.4g). */
    if(f[0] == '%' && strcmp(f, "%9.0g") != 0
       && (strchr(f,'f') || strchr(f,'g') || strchr(f,'e') || strchr(f,'G') || strchr(f,'E') || strchr(f,'F'))){
        snprintf(out, n, f, x);
        return;
    }
    if(x==floor(x)&&fabs(x)<1e15) snprintf(out,n,"%.0f",x);
    else snprintf(out,n,"%.4g",x);
}

/* ---- selection predicate ----------------------------------------------- */
typedef struct { Node *ifn; long lo,hi; } Sel;
static bool sel_ok(Sel *s,EvalCtx *ec,size_t row,size_t pos1){
    if(s->lo>0 && (long)pos1<s->lo) return false;
    if(s->hi>0 && (long)pos1>s->hi) return false;
    if(s->ifn){ ec->i=row; ec->n=(long)pos1; ec->N=(long)ec->f->nobs;
        if(!expr_eval_bool(s->ifn,ec)) return false; }
    return true;
}

/* ---- generate / replace ------------------------------------------------ */
static int do_genrep(Cmd *c,int is_replace){
    char *eq=strchr(c->varlist,'=');
    /* careful: '=' may be inside varlist only (no ifexp here) */
    if(!eq){ tea_err("%s: '=' required\n",c->cmd); return 198; }
    *eq=0; char lhs[256]; snprintf(lhs,sizeof lhs,"%s",c->varlist);
    char rhs[2048]; snprintf(rhs,sizeof rhs,"%s",eq+1);
    /* trim */
    char *p=lhs; while(*p==' ')p++; char nm[64];
    /* optional type token in generate */
    char typ[16]=""; if(!is_replace){
        char a[64],b[64]; if(sscanf(p,"%63s %63s",a,b)==2 && var_find(c->f,a)<0
            && (!strcmp(a,"double")||!strcmp(a,"float")||!strcmp(a,"long")
              ||!strcmp(a,"int")||!strcmp(a,"byte")||!strncmp(a,"str",3))){
            snprintf(typ,sizeof typ,"%s",a); snprintf(nm,sizeof nm,"%s",b);
        } else sscanf(p,"%63s",nm);
    } else sscanf(p,"%63s",nm);
    for(char *q=rhs;*q;q++){} char *r=rhs; while(*r==' ')r++;
    for(int i=(int)strlen(r)-1;i>=0&&(r[i]==' ');i--)r[i]=0;

    const char *perr;
    Node *ast=expr_parse(r,c->f,&perr);
    if(!ast){ tea_err("expression error: %s\n",perr); return 198; }
    Node *ifn=NULL;
    if(c->ifexp[0]){ ifn=expr_parse(c->ifexp,c->f,&perr);
        if(!ifn){ tea_err("if error: %s\n",perr); node_free(ast); return 198; } }

    EvalCtx ec={0}; ec.f=c->f;
    int vi=var_find(c->f,nm);
    if(is_replace){ if(vi<0){ tea_err("replace: %s not found\n",nm); node_free(ast);node_free(ifn);return 111; } }
    else if(vi>=0){ tea_err("generate: %s already defined\n",nm); node_free(ast);node_free(ifn);return 110; }

    /* decide type by probing first obs */
    VarType vt=VT_NUM;
    if(typ[0]&&!strncmp(typ,"str",3)) vt=VT_STR;
    else if(!typ[0]||!is_replace){
        if(c->f->nobs){ ec.i=0; ec.n=1; ec.N=(long)c->f->nobs;
            EVal pv=expr_eval(ast,&ec); if(pv.is_str)vt=VT_STR; eval_free(&pv); }
    }
    Variable *v;
    if(is_replace) v=&c->f->vars[vi];
    else { v=var_add(c->f,nm,vt); }

    Sel sel={ifn,c->in_lo,c->in_hi};
    size_t nch=0;
    /* group iteration for by: */
    size_t *lo=NULL,*hi=NULL; int ng=1; bool by=(c->nby>0);
    if(by){ ng=by_groups(c->f,c->byvars,c->nby,&lo,&hi); }
    for(int g=0; g<ng; g++){
        size_t a = by? lo[g]:0, b = by? hi[g]:(c->f->nobs?c->f->nobs-1:0);
        long gN = by? (long)(b-a+1) : (long)c->f->nobs;
        accs_free(&ec.accs);
        for(size_t row=a; c->f->nobs && row<=b; row++){
            long pos1 = by? (long)(row-a+1) : (long)(row+1);
            ec.i=row; ec.n=pos1; ec.N=gN;
            if(!sel_ok(&sel,&ec,row, (size_t)(row+1))){ continue; }
            ec.i=row; ec.n=pos1; ec.N=gN;
            EVal val=expr_eval(ast,&ec);
            if(ec.err){ tea_err("%s: %s\n",c->cmd,ec.err); }
            if(v->type==VT_STR){ str_set(v,row,val.is_str?val.str:""); }
            else v->num[row]= val.is_str? SV_MISS : val.num;
            eval_free(&val); nch++;
        }
        if(c->f->nobs==0) break;
    }
    free(lo);free(hi);
    tsidx_free(ec.tsidx); accs_free(&ec.accs);
    node_free(ast); node_free(ifn);
    if(!c->quiet) printf("(%zu real change%s made)\n",nch,nch==1?"":"s");
    /* a destructive replace on a sort key invalidates the sort */
    if(is_replace) for(int k=0;k<c->f->nsort;k++) if(c->f->sortvars[k]==vi){ frame_unsort(c->f); break; }
    return 0;
}

/* ---- egen -------------------------------------------------------------- */
static int do_egen(Cmd *c){
    char *eq=strchr(c->varlist,'='); if(!eq){tea_err("egen: '=' required\n");return 198;}
    *eq=0; char nm[64]; sscanf(c->varlist,"%63s",nm);
    char *rhs=eq+1; while(*rhs==' ')rhs++;
    char fn[32]; char inside[1024]; 
    if(sscanf(rhs,"%31[^ (](%1023[^)])",fn,inside)!=2){ tea_err("egen: bad syntax\n"); return 198; }

    /* Trim trailing whitespace from inside */
    {char *q=inside+strlen(inside);while(q>inside&&(q[-1]==' '||q[-1]=='\t'))*--q=0;}

    /* Detect aggregator family: row* operate over multiple variables of
     * the same row; the rest operate over rows within (optional) by(). */
    bool is_row = !strncmp(fn,"row",3);
    bool is_group = !strcmp(fn,"group");
    bool is_tag   = !strcmp(fn,"tag");
    bool is_rank  = !strcmp(fn,"rank");

    int *bv=NULL,nbv=0; char byspec[256]="";
    if(opt_value(c->options,"by",byspec,sizeof byspec)) nbv=varlist_expand(c->f,byspec,&bv);

    /* `group(v1 v2)` and `tag(v1 v2)` use the inside-paren list as their
     * group definition; `by()` is not used (and shouldn't be). */
    if((is_group || is_tag) && nbv == 0){
        nbv = varlist_expand(c->f, inside, &bv);
    }

    /* For row* functions, expand the inside as a varlist of multiple cols. */
    int *rv = NULL; int nrv = 0;
    if(is_row){
        nrv = varlist_expand(c->f, inside, &rv);
        if(nrv < 1){ tea_err("egen: %s() needs a varlist\n", fn); free(bv); return 198; }
    }

    /* Determine if we need an expression node (for non-row, non-group/tag) */
    Node *arg = NULL; const char *perr;
    if(!is_row && !is_group && !is_tag){
        arg = expr_parse(inside, c->f, &perr);
        if(!arg){ tea_err("egen: %s\n", perr); free(bv); free(rv); return 198; }
    }
    Node *ifn=NULL; if(c->ifexp[0]) ifn=expr_parse(c->ifexp,c->f,&perr);

    /* Probe type at row 0 to catch string-arg-to-numeric-fn errors early.  */
    if(arg && c->f->nobs > 0){
        EvalCtx pec={0}; pec.f=c->f; pec.i=0; pec.n=1; pec.N=(long)c->f->nobs;
        EVal pv = expr_eval(arg, &pec);
        bool is_string = pv.is_str;
        eval_free(&pv);
        if(is_string){
            tea_err("egen: type mismatch — %s() requires a numeric argument, but '%s' is a string\n",
                    fn, inside);
            node_free(arg); node_free(ifn); free(bv); free(rv); return 109;
        }
    }

    if(var_find(c->f,nm)>=0){ tea_err("egen: %s exists\n",nm); node_free(arg);node_free(ifn);free(bv);free(rv);return 110; }
    Variable *v=var_add(c->f,nm,VT_NUM);

    EvalCtx ec={0}; ec.f=c->f; Sel sel={ifn,c->in_lo,c->in_hi};

    /* --- row* functions: per-row aggregation across columns --- */
    if(is_row){
        for(size_t row=0; row<c->f->nobs; row++){
            ec.i=row; ec.n=(long)row+1; ec.N=(long)c->f->nobs;
            if(!sel_ok(&sel,&ec,row,row+1)){ v->num[row]=SV_MISS; continue; }
            double sm=0, ssq=0; double mn=INFINITY, mx=-INFINITY; long cnt=0;
            for(int j=0; j<nrv; j++){
                if(c->f->vars[rv[j]].type != VT_NUM) continue;
                double x = c->f->vars[rv[j]].num[row];
                if(sv_is_miss(x)) continue;
                sm += x; ssq += x*x;
                if(x < mn) mn = x;
                if(x > mx) mx = x;
                cnt++;
            }
            double res;
            if(!strcmp(fn,"rowtotal")) res = sm;
            else if(!strcmp(fn,"rowmean")) res = cnt>0 ? sm/cnt : SV_MISS;
            else if(!strcmp(fn,"rowmin"))  res = cnt>0 ? mn : SV_MISS;
            else if(!strcmp(fn,"rowmax"))  res = cnt>0 ? mx : SV_MISS;
            else if(!strcmp(fn,"rowsd"))   res = cnt>1 ? sqrt((ssq - sm*sm/cnt)/(cnt-1)) : SV_MISS;
            else if(!strcmp(fn,"rowmiss")) res = (double)(nrv - cnt);
            else if(!strcmp(fn,"rownonmiss")) res = (double)cnt;
            else { tea_err("egen: unknown row function %s\n", fn);
                   node_free(arg); node_free(ifn); free(bv); free(rv); return 198; }
            v->num[row] = res;
        }
        goto done;
    }

    /* For non-row functions, we may sort+group by `by(...)`. */
    size_t *lo=NULL,*hi=NULL; int ng=1; bool by=(nbv>0);
    if(by){ frame_physical_sort(c->f,bv,nbv,NULL); ng=by_groups(c->f,bv,nbv,&lo,&hi); }

    /* --- group/tag: assign group ID or first-in-group marker --- */
    if(is_group){
        long gid = 0;
        for(int g=0; g<ng && c->f->nobs; g++){
            size_t a=by?lo[g]:0, b=by?hi[g]:c->f->nobs-1;
            gid++;
            for(size_t row=a; row<=b; row++) v->num[row] = (double)gid;
        }
        free(lo); free(hi);
        goto done;
    }
    if(is_tag){
        for(int g=0; g<ng && c->f->nobs; g++){
            size_t a=by?lo[g]:0, b=by?hi[g]:c->f->nobs-1;
            for(size_t row=a; row<=b; row++) v->num[row] = (row==a) ? 1 : 0;
        }
        free(lo); free(hi);
        goto done;
    }

    /* --- standard aggregators (sum, mean, sd, min, max, count, median, iqr, pc, rank) --- */
    /* Decide if we need the sorted values per group (median, iqr, pc, rank) */
    bool need_sort = !strcmp(fn,"median") || !strcmp(fn,"iqr") || !strcmp(fn,"p50")
                   || !strcmp(fn,"p25") || !strcmp(fn,"p75") || !strcmp(fn,"pc")
                   || is_rank;

    /* Optional second arg for pc(varname, N): not standard Stata syntax,
     * but Stata uses option p(N).  We support p(N) via options. */
    double pc_pct = 50;  /* default percentile */
    char pc_opt[16]="";
    if(opt_value(c->options,"p",pc_opt,sizeof pc_opt)) pc_pct = atof(pc_opt);

    for(int g=0; g<ng && c->f->nobs; g++){
        size_t a=by?lo[g]:0, b=by?hi[g]:c->f->nobs-1;
        double mn=INFINITY, mx=-INFINITY, sm=0, ssq=0; long cnt=0;
        /* Collect values for this group (with selection mask) */
        size_t cap = b - a + 1;
        double *vals = need_sort ? malloc(cap*sizeof(double)) : NULL;
        for(size_t row=a; row<=b; row++){
            ec.i=row; ec.n=(long)row+1; ec.N=(long)c->f->nobs;
            if(!sel_ok(&sel,&ec,row,row+1)) continue;
            EVal e = expr_eval(arg,&ec);
            double x = e.is_str ? SV_MISS : e.num;
            eval_free(&e);
            if(sv_is_miss(x)) continue;
            sm += x; ssq += x*x;
            if(x < mn) mn = x;
            if(x > mx) mx = x;
            if(vals) vals[cnt] = x;
            cnt++;
        }
        if(vals){
            /* simple insertion sort — group size in tea typically small */
            for(long i=1; i<cnt; i++){
                double x=vals[i]; long j=i-1;
                while(j>=0 && vals[j]>x){ vals[j+1]=vals[j]; j--; }
                vals[j+1]=x;
            }
        }

        double res = SV_MISS;
        if(!strcmp(fn,"sum")||!strcmp(fn,"total"))    res = sm;
        else if(!strcmp(fn,"mean"))                    res = cnt ? sm/cnt : SV_MISS;
        else if(!strcmp(fn,"count"))                   res = (double)cnt;
        else if(!strcmp(fn,"min"))                     res = cnt ? mn : SV_MISS;
        else if(!strcmp(fn,"max"))                     res = cnt ? mx : SV_MISS;
        else if(!strcmp(fn,"sd"))                      res = cnt>1 ? sqrt((ssq - sm*sm/cnt)/(cnt-1)) : SV_MISS;
        else if(!strcmp(fn,"median")||!strcmp(fn,"p50")){
            if(cnt > 0){ if(cnt%2) res = vals[cnt/2];
                         else      res = 0.5*(vals[cnt/2 - 1] + vals[cnt/2]); }
        }
        else if(!strcmp(fn,"p25")){ if(cnt>0) res = vals[(cnt-1)/4]; }
        else if(!strcmp(fn,"p75")){ if(cnt>0) res = vals[(3*(cnt-1))/4]; }
        else if(!strcmp(fn,"pc")){
            /* percentile pc_pct (0-100) */
            if(cnt > 0){
                double pos = pc_pct / 100.0 * (cnt - 1);
                long lo_i = (long)floor(pos), hi_i = (long)ceil(pos);
                if(lo_i == hi_i) res = vals[lo_i];
                else res = vals[lo_i] + (pos - lo_i) * (vals[hi_i] - vals[lo_i]);
            }
        }
        else if(!strcmp(fn,"iqr")){
            if(cnt > 0){
                /* simple p75 - p25 using nearest-rank */
                double q1 = vals[(cnt-1)/4];
                double q3 = vals[(3*(cnt-1))/4];
                res = q3 - q1;
            }
        }
        /* fall-through: rank handled in the second pass below */

        if(!is_rank){
            for(size_t row=a; row<=b; row++) v->num[row] = res;
        } else {
            /* rank within group: average rank for ties.  Walk sorted values
             * and tag each row's rank by lookup. */
            for(size_t row=a; row<=b; row++){
                ec.i=row;
                if(!sel_ok(&sel,&ec,row,row+1)){ v->num[row]=SV_MISS; continue; }
                EVal e = expr_eval(arg,&ec);
                double x = e.is_str ? SV_MISS : e.num;
                eval_free(&e);
                if(sv_is_miss(x)){ v->num[row]=SV_MISS; continue; }
                /* Find first and last index in vals[] where value == x;
                 * rank = average of (i+1) over that range. */
                long lo_i = 0, hi_i = cnt - 1;
                /* binary search lower bound */
                while(lo_i < hi_i){ long m=(lo_i+hi_i)/2;
                    if(vals[m] < x) lo_i = m+1; else hi_i = m; }
                long first_i = lo_i;
                /* linear walk for upper bound (groups are small) */
                long last_i = first_i;
                while(last_i+1 < cnt && vals[last_i+1] == x) last_i++;
                v->num[row] = 0.5 * (first_i + 1 + last_i + 1);
            }
        }
        free(vals);
    }
    free(lo); free(hi);

done:
    free(bv); free(rv);
    tsidx_free(ec.tsidx); node_free(arg); node_free(ifn);
    return 0;
}

/* ---- list -------------------------------------------------------------- */
static int do_list(Cmd *c){
    int *vs=NULL,nv;
    int n_temps = 0;
    const char *vlerr = NULL;
    if(c->varlist[0]) nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    else { nv=c->f->nvar; vs=malloc(nv*sizeof(int)); for(int i=0;i<nv;i++)vs[i]=i; }
    if(nv<0){
        tea_err("list: %s\n", vlerr ? vlerr : "variable not found");
        free(vs); return 111;
    }
    Node *ifn=NULL; const char *pe;
    if(c->ifexp[0]) ifn=expr_parse(c->ifexp,c->f,&pe);
    if(c->ifexp[0]&&!ifn){ tea_err("if error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    /* Compute column width per variable as max(header_len, max(cell_len))
     * over rows in the selection.  This is what Stata does.
     * Heap-allocated to accommodate any number of variables. */
    int *w = malloc((size_t)nv * sizeof(int));
    for(int j=0;j<nv;j++){
        w[j] = u8width(c->f->vars[vs[j]].name);
        if(w[j]<8) w[j]=8;
        for(size_t i=0;i<c->f->nobs;i++){
            if(c->in_lo>0&&(long)i+1<c->in_lo)continue;
            if(c->in_hi>0&&(long)i+1>c->in_hi)continue;
            if(ifn){ EvalCtx ec={0}; ec.f=c->f; ec.i=i; ec.n=(long)i+1; ec.N=(long)c->f->nobs;
                if(!expr_eval_bool(ifn,&ec)) continue; }
            char b[128]; fmt_cell(&c->f->vars[vs[j]],i,b,sizeof b);
            int L=u8width(b); if(L>w[j]) w[j]=L;
        }
    }
    printf("     +");for(int j=0;j<nv;j++)printf("%-*s+",w[j]+2,"");printf("\n     |");
    for(int j=0;j<nv;j++)printf(" %-*s |",u8pad(c->f->vars[vs[j]].name,w[j]),c->f->vars[vs[j]].name);printf("\n");
    EvalCtx ec={0}; ec.f=c->f;
    for(size_t i=0;i<c->f->nobs;i++){
        if(c->in_lo>0&&(long)i+1<c->in_lo)continue;
        if(c->in_hi>0&&(long)i+1>c->in_hi)continue;
        if(ifn){ ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs; if(!expr_eval_bool(ifn,&ec))continue; }
        printf("%4zu.|",i+1);
        for(int j=0;j<nv;j++){ char b[128]; fmt_cell(&c->f->vars[vs[j]],i,b,sizeof b);
            printf(" %-*s |",u8pad(b,w[j]),b); }
        printf("\n");
    }
    free(w); free(vs); node_free(ifn);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- summarize --------------------------------------------------------- */
static int cmp_double_asc(const void *p, const void *q){
    double a = *(const double*)p, b = *(const double*)q;
    return (a > b) - (a < b);
}

/* ---- summarize --------------------------------------------------------- *
 *
 * Stata: `summarize [varlist] [if] [in] [weight] [, detail]`
 *
 * Default output: variable, N, mean, sd, min, max.
 * With ,detail: adds percentiles (1,5,10,25,50,75,90,95,99), skewness,
 *               kurtosis, sum of weights, variance.
 *
 * Stores r(N), r(mean), r(sd), r(min), r(max), r(sum), r(Var); plus
 * r(p1)..r(p99), r(skewness), r(kurtosis) when ,detail is given.
 * Stata note: pweight is NOT accepted by summarize (different from
 * regress); aweight/fweight/iweight are.
 *
 * `bysort g: summarize x` produces one block per group.
 */
static int do_summarize(Cmd *c){
    /* Stata: summarize accepts fweight, aweight, iweight; not pweight. */
    int wrc = validate_wtype(c, WTM_FW|WTM_AW|WTM_IW, "summarize");
    if(wrc) return wrc;
    int *vs=NULL,nv;
    int n_temps = 0;
    const char *vlerr = NULL;
    if(c->varlist[0]) nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    else { nv=0; vs=malloc(c->f->nvar*sizeof(int)); for(int i=0;i<c->f->nvar;i++)if(c->f->vars[i].type==VT_NUM)vs[nv++]=i; }
    if(nv<0){
        tea_err("summarize: %s\n", vlerr ? vlerr : "variable not found");
        free(vs); return 111;
    }
    /* If the user gave specific names or a range (no wildcards), error on
     * string variables — they explicitly asked for them.  With wildcards,
     * silently filter strings (Stata's behavior for `summarize z*`). */
    bool has_wildcard = c->varlist[0] && strpbrk(c->varlist, "*?") != NULL;
    if(c->varlist[0] && !has_wildcard){
        for(int j=0;j<nv;j++){
            Variable *v = &c->f->vars[vs[j]];
            if(v->type != VT_NUM){
                tea_err("summarize: %s is a string variable (cannot summarize)\n", v->name);
                free(vs); tsop_drop_temps(c->f, n_temps); return 109;
            }
        }
    }
    Node *ifn=NULL; const char *pe; if(c->ifexp[0]) ifn=expr_parse(c->ifexp,c->f,&pe);
    if(c->ifexp[0]&&!ifn){ tea_err("if error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    Node *wn=NULL; if(c->wexp[0]) wn=expr_parse(c->wexp,c->f,&pe);
    if(c->wexp[0]&&!wn){ node_free(ifn); tea_err("weight error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    EvalCtx ec={0}; ec.f=c->f;

    /* by-group setup: 'by[sort] g1 g2: summarize ...' iterates per group */
    int by = c->nby>0;
    size_t *gLo=NULL,*gHi=NULL; int ng=1;
    if(by){
        if(c->bysort) frame_physical_sort(c->f,c->byvars,c->nby,NULL);
        ng = by_groups(c->f,c->byvars,c->nby,&gLo,&gHi);
    }

    for(int g=0; g<ng; g++){
        size_t r_a = by ? gLo[g] : 0;
        size_t r_b = by ? gHi[g] : (c->f->nobs? c->f->nobs-1 : 0);

        if(by && !c->quiet){
            /* per-group header: "-> g1=val [g2=val]" */
            printf("\n-> ");
            for(int k=0;k<c->nby;k++){
                Variable *bv=&c->f->vars[c->byvars[k]];
                if(k) printf(", ");
                if(bv->type==VT_NUM){ double dv=bv->num[r_a];
                    if(sv_is_miss(dv)) printf("%s=.",bv->name);
                    else printf("%s=%g",bv->name,dv);
                } else printf("%s=%s",bv->name,bv->str[r_a]);
            }
            printf("\n");
        }
        if(!c->quiet) printf("\n    Variable |        Obs        Mean    Std. dev.         Min         Max\n-------------+--------------------------------------------------------------\n");
        bool detail = opt_present(c->options,"detail") || opt_present(c->options,"d");
        for(int j=0;j<nv;j++){
            Variable *v=&c->f->vars[vs[j]]; if(v->type!=VT_NUM)continue;

            /* If detail requested, collect the values (filtered) for percentile
             * + higher-moment computation.  Otherwise stream-aggregate as
             * before to avoid an O(N) allocation per variable. */
            double *xs = NULL; size_t nx = 0, xcap = 0;

            double sw=0,swx=0,swxx=0,mn=INFINITY,mx=-INFINITY; long n=0;
            for(size_t i=r_a;i<=r_b;i++){
                if(c->in_lo>0&&(long)i+1<c->in_lo)continue;
                if(c->in_hi>0&&(long)i+1>c->in_hi)continue;
                if(ifn){ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs;if(!expr_eval_bool(ifn,&ec))continue;}
                double x=v->num[i]; if(sv_is_miss(x))continue;
                double w=1.0;
                if(wn){ ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs; EVal wv=expr_eval(wn,&ec);
                    w=wv.is_str?SV_MISS:wv.num; eval_free(&wv); if(sv_is_miss(w)||w<=0)continue; }
                sw+=w; swx+=w*x; swxx+=w*x*x; n++; if(x<mn)mn=x; if(x>mx)mx=x;
                if(detail){
                    if(nx==xcap){ xcap = xcap? xcap*2 : 256; xs = realloc(xs, xcap*sizeof(double)); }
                    xs[nx++] = x;
                }
            }
            double mean = sw>0? swx/sw : SV_MISS, sd=SV_MISS, dispObs;
            if(c->wtype==1){ dispObs=sw; if(sw>1) sd=sqrt((swxx - swx*swx/sw)/(sw-1)); }
            else if(c->wtype>=2){ dispObs=n; if(n>1 && sw>0){ double f=n/sw;
                double nx2=swx*f, nxx=swxx*f; sd=sqrt((nxx - nx2*nx2/n)/(n-1)); } }
            else { dispObs=n; if(n>1) sd=sqrt((swxx - swx*swx/n)/(n-1)); }

            if(detail){
                /* sort the collected values; compute Stata's detail block */
                if(nx > 0) qsort(xs, nx, sizeof(double), cmp_double_asc);
                /* percentile helper (linear interpolation, 1-indexed rank) */
                #define PCTL(P) ({ double pv; if(nx==0) pv=SV_MISS; \
                    else if(nx==1) pv=xs[0]; \
                    else { double r=(P)/100.0*(nx-1); long lo=(long)r; double f=r-lo; \
                           pv = (lo+1<(long)nx)? xs[lo]*(1-f)+xs[lo+1]*f : xs[lo]; } \
                    pv; })
                double p1 = PCTL(1), p5 = PCTL(5), p10 = PCTL(10), p25 = PCTL(25),
                       p50 = PCTL(50), p75 = PCTL(75), p90 = PCTL(90), p95 = PCTL(95), p99 = PCTL(99);
                /* skewness / kurtosis using central moments (Stata: g1 and b2) */
                double skew = SV_MISS, kurt = SV_MISS, var = SV_MISS;
                if(n > 1 && !sv_is_miss(sd)){
                    var = sd*sd;
                    double m3=0, m4=0;
                    for(size_t i=0;i<nx;i++){
                        double d = xs[i] - mean;
                        m3 += d*d*d; m4 += d*d*d*d;
                    }
                    m3 /= n; m4 /= n;
                    double sigma3 = sd*sd*sd;          /* sample-sd^3 — matches Stata */
                    if(sigma3 > 0) skew = m3 / sigma3;
                    if(var > 0)    kurt = m4 / (var*var);
                }
                /* four smallest, four largest */
                double sm1 = nx>=1? xs[0] : SV_MISS,
                       sm2 = nx>=2? xs[1] : SV_MISS,
                       sm3 = nx>=3? xs[2] : SV_MISS,
                       sm4 = nx>=4? xs[3] : SV_MISS;
                double lg1 = nx>=1? xs[nx-1] : SV_MISS,
                       lg2 = nx>=2? xs[nx-2] : SV_MISS,
                       lg3 = nx>=3? xs[nx-3] : SV_MISS,
                       lg4 = nx>=4? xs[nx-4] : SV_MISS;
                /* Render Stata-style detail block */
                if(!c->quiet){
                    /* on detail, suppress the brief header for this var so it
                     * doesn't compete with the detail block */
                    printf("\n%s\n", v->name);
                    printf("-------------------------------------------------------------\n");
                    printf("      Percentiles      Smallest\n");
                    printf(" 1%%    %10.6g   %10.6g\n", p1,  sm1);
                    printf(" 5%%    %10.6g   %10.6g\n", p5,  sm2);
                    printf("10%%    %10.6g   %10.6g       Obs        %ld\n", p10, sm3, n);
                    printf("25%%    %10.6g   %10.6g       Sum of wgt. %.0f\n", p25, sm4, sw);
                    printf("\n");
                    printf("50%%    %10.6g                       Mean        %10.6g\n", p50, mean);
                    printf("                          Largest    Std. dev.   %10.6g\n", sv_is_miss(sd)?0.0:sd);
                    printf("75%%    %10.6g   %10.6g\n", p75, lg4);
                    printf("90%%    %10.6g   %10.6g       Variance    %10.6g\n", p90, lg3, sv_is_miss(var)?0.0:var);
                    printf("95%%    %10.6g   %10.6g       Skewness    %10.6g\n", p95, lg2, sv_is_miss(skew)?0.0:skew);
                    printf("99%%    %10.6g   %10.6g       Kurtosis    %10.6g\n", p99, lg1, sv_is_miss(kurt)?0.0:kurt);
                }
                #undef PCTL
            } else {
                if(!c->quiet) printf("%12s |%11.0f %11.4g %12.4g %11.4g %11.4g\n",
                    v->name,dispObs,n?mean:0.0,(!sv_is_miss(sd))?sd:0.0,n?mn:0.0,n?mx:0.0);
            }
            /* set r() only for non-by, single-var summarize (Stata-style) */
            if(!by && nv==1){ char b[32];
                snprintf(b,sizeof b,"%.0f",dispObs); mac_set(&c->ip->rret,"r(N)",b);
                snprintf(b,sizeof b,"%.10g",mean); mac_set(&c->ip->rret,"r(mean)",b);
                snprintf(b,sizeof b,"%.10g",sd);   mac_set(&c->ip->rret,"r(sd)",b);
                snprintf(b,sizeof b,"%.10g",n?mn:SV_MISS); mac_set(&c->ip->rret,"r(min)",b);
                snprintf(b,sizeof b,"%.10g",n?mx:SV_MISS); mac_set(&c->ip->rret,"r(max)",b);
                snprintf(b,sizeof b,"%.10g",swx); mac_set(&c->ip->rret,"r(sum)",b);
            }
            free(xs);
        }
    }
    free(gLo); free(gHi);
    free(vs); node_free(ifn); node_free(wn);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- count ------------------------------------------------------------- */
static int do_count(Cmd *c){
    /* Stata: count accepts only fweight. */
    int wrc = validate_wtype(c, WTM_FW, "count");
    if(wrc) return wrc;
    Node *ifn=NULL; const char *pe; if(c->ifexp[0]) ifn=expr_parse(c->ifexp,c->f,&pe);
    if(c->ifexp[0]&&!ifn){ tea_err("if error: %s\n", pe); return 111; }
    Node *wn=NULL; if(c->wexp[0]&&c->wtype==1) wn=expr_parse(c->wexp,c->f,&pe);
    if(c->wexp[0]&&c->wtype==1&&!wn){ node_free(ifn); tea_err("weight error: %s\n", pe); return 111; }
    EvalCtx ec={0}; ec.f=c->f; double n=0;
    for(size_t i=0;i<c->f->nobs;i++){
        if(c->in_lo>0&&(long)i+1<c->in_lo)continue;
        if(c->in_hi>0&&(long)i+1>c->in_hi)continue;
        if(ifn){ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs;if(!expr_eval_bool(ifn,&ec))continue;}
        if(wn){ ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs; EVal w=expr_eval(wn,&ec);
            double wv=w.is_str?SV_MISS:w.num; eval_free(&w); if(sv_is_miss(wv))continue; n+=wv; }
        else n+=1;
    }
    char b[32]; snprintf(b,sizeof b,"%.0f",n); mac_set(&c->ip->rret,"r(N)",b);
    if(!c->quiet) printf("  %.0f\n",n);
    node_free(ifn); node_free(wn); return 0;
}

/* ---- describe ---------------------------------------------------------- */
static int do_describe(Cmd *c){
    int *vs=NULL; int nv=0;
    if(c->varlist[0]){
        nv = varlist_expand(c->f, c->varlist, &vs);
        if(nv < 0){ tea_err("describe: variable not found\n"); return 111; }
    }
    int ndisp = c->varlist[0] ? nv : c->f->nvar;
    printf("\nContains data\n");
    if(c->f->data_label[0]) printf("  data label: %s\n", c->f->data_label);
    printf("  obs: %zu\n vars: %d%s\n",
           c->f->nobs, ndisp, c->varlist[0] ? " (filtered)" : "");
    printf("-------------------------------------------------------------\n");
    printf("%-16s %-8s %-10s %s\n","variable","type","format","label");
    if(c->varlist[0]){
        for(int k=0;k<nv;k++){ Variable*v=&c->f->vars[vs[k]];
            printf("%-16s %-8s %-10s %s\n",v->name,v->type==VT_STR?"str":"double",v->format,v->vlabel); }
    } else {
        for(int i=0;i<c->f->nvar;i++){ Variable*v=&c->f->vars[i];
            printf("%-16s %-8s %-10s %s\n",v->name,v->type==VT_STR?"str":"double",v->format,v->vlabel); }
    }
    printf("-------------------------------------------------------------\n");
    free(vs);
    return 0;
}

/* ---- drop / keep ------------------------------------------------------- */
static int do_dropkeep(Cmd *c,int keep){
    if(c->ifexp[0]||c->in_lo>0){           /* observation filter */
        Node *ifn=NULL; const char *pe;
        if(c->ifexp[0]){
            ifn=expr_parse(c->ifexp,c->f,&pe);
            if(!ifn){ tea_err("if error: %s\n", pe); return 111; }
        }
        EvalCtx ec={0}; ec.f=c->f; size_t w=0;
        for(size_t i=0;i<c->f->nobs;i++){
            int sel=1;
            if(c->in_lo>0&&(long)i+1<c->in_lo)sel=0;
            if(c->in_hi>0&&(long)i+1>c->in_hi)sel=0;
            if(sel&&ifn){ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs;sel=expr_eval_bool(ifn,&ec);}
            int survive = keep? sel : !sel;
            if(survive){ if(w!=i) for(int v=0;v<c->f->nvar;v++){ Variable*V=&c->f->vars[v];
                if(V->type==VT_NUM)V->num[w]=V->num[i]; else {free(V->str[w]);V->str[w]=V->str[i];V->str[i]=NULL;} } w++; }
            else { for(int v=0;v<c->f->nvar;v++){ Variable*V=&c->f->vars[v]; if(V->type==VT_STR){free(V->str[i]);V->str[i]=NULL;} } }
        }
        /* compact survivors. Sort order and panel/time bookkeeping survive
         * row deletion: we only removed rows in-place, so any pre-existing
         * sort is still sorted, and tsset/xtset still names valid columns.
         * (Gaps in the time variable are fine — tea's TS-op machinery is
         * gap-aware via tsidx_lookup.) */
        c->f->nobs=w; node_free(ifn);
        if(!c->quiet)printf("(observations now %zu)\n",c->f->nobs);
        return 0;
    }
    /* variable list */
    int *vs=NULL,nv=varlist_expand(c->f,c->varlist,&vs);
    if(nv<0){tea_err("%s: variable not found\n",c->cmd);return 111;}
    char dropmask[4096]={0};
    for(int i=0;i<c->f->nvar;i++){ int in=0; for(int j=0;j<nv;j++)if(vs[j]==i)in=1;
        dropmask[i]= keep? !in : in; }
    int w=0;
    for(int i=0;i<c->f->nvar;i++){ Variable*v=&c->f->vars[i];
        if(dropmask[i]){ if(v->type==VT_STR){for(size_t r=0;r<c->f->nobs;r++)free(v->str[r]);free(v->str);} else free(v->num); }
        else c->f->vars[w++]=*v; }
    c->f->nvar=w; free(vs); frame_unsort(c->f);
    return 0;
}

/* ---- rename / order / label ------------------------------------------- */
/* rename: supports several forms
 *   rename oldname newname              -- single rename
 *   rename oldpat   newpat              -- wildcard rename: each * in oldpat
 *                                          captures, replaces same-position *
 *                                          in newpat.  E.g. `rename _C* C*`
 *                                          renames _COUNTRY → COUNTRY,
 *                                          _COUNTRY_ID → COUNTRY_ID, etc.
 *
 * Wildcard rule: oldpat and newpat must contain the same number of *
 * wildcards.  Each * in oldpat is matched against zero or more chars in the
 * variable name; the captured chunks substitute the *s in newpat in order. */
static int wildcard_match_capture(const char *pat, const char *str, char caps[8][64], int *ncap){
    /* Greedy left-to-right matcher for patterns containing '*' wildcards.
     * Returns 1 on match (capturing each '*' segment), 0 otherwise.
     * Limitations: at most 8 wildcards; no '?' support (Stata's rename uses
     * '*' only).  This isn't full regex — but it's what Stata's rename does. */
    *ncap = 0;
    /* Split pattern into literal segments by '*' */
    const char *parts[16]; int part_lens[16]; int np=0;
    const char *seg = pat;
    for(const char *p=pat; ; p++){
        if(*p == '*' || *p == 0){
            if(np >= 16) return 0;
            parts[np] = seg; part_lens[np++] = (int)(p - seg);
            if(*p == 0) break;
            seg = p + 1;
        }
    }
    int n_stars = np - 1;
    if(n_stars > 8) return 0;
    /* First segment must be a prefix of str */
    if(strncmp(str, parts[0], part_lens[0]) != 0) return 0;
    const char *s_pos = str + part_lens[0];
    /* For each remaining segment, find it in str (greedy left match) and
     * capture everything between the previous position and the segment start. */
    for(int i=1; i<np; i++){
        if(part_lens[i] == 0){
            /* trailing * — captures the rest */
            if(i != np-1) return 0;     /* only allowed at the end */
            int cn = (int)strlen(s_pos);
            if(cn >= 64) return 0;
            memcpy(caps[*ncap], s_pos, cn); caps[*ncap][cn] = 0;
            (*ncap)++;
            return 1;
        }
        /* find parts[i] in s_pos */
        const char *found = strstr(s_pos, parts[i]);
        if(!found) return 0;
        /* but for the LAST segment, the match must be at the end of str */
        if(i == np-1){
            const char *last = strstr(s_pos, parts[i]);
            const char *p = last;
            while(p){ const char *q = strstr(p+1, parts[i]); if(!q) break; p = q; }
            if(!p) return 0;
            found = p;
            if(found + part_lens[i] != str + strlen(str)) return 0;
        }
        int cn = (int)(found - s_pos);
        if(cn >= 64) return 0;
        memcpy(caps[*ncap], s_pos, cn); caps[*ncap][cn] = 0;
        (*ncap)++;
        s_pos = found + part_lens[i];
    }
    return 1;
}

static int do_rename(Cmd *c){
    /* group form:  rename (a b c) (x y z)  — pairwise, all-or-nothing */
    {
        const char *p = c->varlist; while(*p==' ')p++;
        if(*p=='('){
            const char *c1 = strchr(p,')');
            const char *o2 = c1 ? strchr(c1+1,'(') : NULL;
            const char *c2 = o2 ? strchr(o2,')') : NULL;
            if(!c1 || !o2 || !c2){
                tea_err("rename: group form is rename (old...) (new...)\n"); return 198;
            }
            char olds[512], news[512];
            snprintf(olds,sizeof olds,"%.*s",(int)(c1-p-1),p+1);
            snprintf(news,sizeof news,"%.*s",(int)(c2-o2-1),o2+1);
            char *ov[64], *nv[64]; int no=0, nn=0;
            for(char *t=strtok(olds," "); t && no<64; t=strtok(NULL," ")) ov[no++]=t;
            for(char *t=strtok(news," "); t && nn<64; t=strtok(NULL," ")) nv[nn++]=t;
            if(no==0 || no!=nn){
                tea_err("rename: group lists differ in length (%d vs %d)\n",no,nn); return 198;
            }
            int idx[64];
            /* validate everything before touching anything */
            for(int k=0;k<no;k++){
                idx[k]=var_find(c->f,ov[k]);
                if(idx[k]<0){ tea_err("rename: %s not found\n",ov[k]); return 111; }
            }
            for(int k=0;k<no;k++){
                int e=var_find(c->f,nv[k]);
                /* a new name may equal an old name being vacated in the same
                 * group (swap); only names surviving the rename collide */
                if(e>=0){
                    int vacated=0;
                    for(int m=0;m<no;m++) if(e==idx[m]){ vacated=1; break; }
                    if(!vacated){ tea_err("rename: %s already exists\n",nv[k]); return 110; }
                }
                for(int m=0;m<k;m++) if(!strcmp(nv[k],nv[m])){
                    tea_err("rename: duplicate new name %s\n",nv[k]); return 198; }
            }
            for(int k=0;k<no;k++) snprintf(c->f->vars[idx[k]].name,33,"%s",nv[k]);
            frame_unsort(c->f);
            return 0;
        }
    }
    char a[128], b[128];
    if(sscanf(c->varlist, "%127s %127s", a, b) != 2){
        tea_err("rename: need old new\n"); return 198;
    }
    /* Simple case: no wildcards. */
    if(!strchr(a, '*') && !strchr(b, '*')){
        int vi = var_find(c->f, a);
        if(vi < 0){ tea_err("rename: %s not found\n", a); return 111; }
        if(strcmp(a, b) != 0 && var_find(c->f, b) >= 0){
            tea_err("rename: %s already exists\n", b); return 110;
        }
        snprintf(c->f->vars[vi].name, 33, "%s", b);
        return 0;
    }
    /* Wildcard case: counts of * must match. */
    int na=0, nb=0;
    for(const char *p=a; *p; p++) if(*p=='*') na++;
    for(const char *p=b; *p; p++) if(*p=='*') nb++;
    if(na != nb){
        tea_err("rename: '%s' and '%s' must have the same number of * wildcards\n", a, b);
        return 198;
    }
    /* Walk all variables; rename ones matching pattern. */
    long renamed = 0;
    for(int i=0; i<c->f->nvar; i++){
        char caps[8][64]; int ncap=0;
        if(!wildcard_match_capture(a, c->f->vars[i].name, caps, &ncap)) continue;
        /* Build new name by substituting *s in b with captures in order. */
        char newname[128]; int wo=0; int ci=0;
        for(const char *p=b; *p && wo<127; p++){
            if(*p == '*' && ci < ncap){
                int cl = (int)strlen(caps[ci++]);
                if(wo + cl > 127) cl = 127 - wo;
                memcpy(newname+wo, caps[ci-1], cl); wo += cl;
            } else {
                newname[wo++] = *p;
            }
        }
        newname[wo] = 0;
        if(!newname[0]){ tea_err("rename: '%s' would produce empty name\n", c->f->vars[i].name); return 198; }
        /* Check for collision with another existing variable (other than self) */
        if(strcmp(c->f->vars[i].name, newname) != 0){
            int existing = var_find(c->f, newname);
            if(existing >= 0 && existing != i){
                tea_err("rename: target name '%s' (from '%s') already exists\n",
                        newname, c->f->vars[i].name);
                return 110;
            }
        }
        snprintf(c->f->vars[i].name, 33, "%s", newname);
        renamed++;
    }
    if(!c->quiet) printf("(renamed %ld variable%s)\n", renamed, renamed==1?"":"s");
    return 0;
}
static int do_order(Cmd *c){ int *vs=NULL,nv=varlist_expand(c->f,c->varlist,&vs);
    if(nv<0){tea_err("order: var not found\n");return 111;}
    Variable *nw=malloc(c->f->cap_var*sizeof(Variable)); int w=0;
    char *seen = calloc((size_t)c->f->nvar, 1);
    for(int j=0;j<nv;j++){ nw[w++]=c->f->vars[vs[j]]; seen[vs[j]]=1; }
    for(int i=0;i<c->f->nvar;i++) if(!seen[i]) nw[w++]=c->f->vars[i];
    free(seen); free(c->f->vars); c->f->vars=nw; free(vs); return 0; }

/* aorder [varlist] — reorder columns alphabetically.  With no varlist,
 * alphabetizes all columns.  With a varlist, alphabetizes just those and
 * leaves the rest in their existing positions.  Stata-compatible. */
static int do_aorder(Cmd *c){
    int *vs=NULL, nv;
    if(c->varlist[0]){
        nv = varlist_expand(c->f, c->varlist, &vs);
        if(nv<0){ tea_err("aorder: var not found\n"); return 111; }
    } else {
        nv = c->f->nvar;
        vs = malloc(nv*sizeof(int));
        for(int i=0;i<nv;i++) vs[i]=i;
    }
    /* sort the selected indices by variable name (case-insensitive, Stata-style) */
    for(int i=0;i<nv;i++) for(int j=i+1;j<nv;j++)
        if(strcasecmp(c->f->vars[vs[i]].name, c->f->vars[vs[j]].name) > 0){
            int t=vs[i]; vs[i]=vs[j]; vs[j]=t; }
    /* build new layout: for each existing slot, if it was in the selection,
     * fill with the next alphabetized name; otherwise keep what was there. */
    char *in_selection = calloc((size_t)c->f->nvar, 1);
    for(int j=0;j<nv;j++) in_selection[vs[j]]=1;
    Variable *nw = malloc(c->f->cap_var * sizeof(Variable));
    int next_alpha = 0;
    for(int i=0;i<c->f->nvar;i++){
        if(in_selection[i]){
            nw[i] = c->f->vars[vs[next_alpha++]];
        } else {
            nw[i] = c->f->vars[i];
        }
    }
    free(in_selection); free(c->f->vars); c->f->vars = nw;
    free(vs);
    return 0;
}

static int do_label(Cmd *c){
    char sub[16]; const char *s=c->args; while(*s==' ')s++;
    sscanf(s,"%15s",sub); s+=strlen(sub); while(*s==' ')s++;
    if(!strcmp(sub,"variable")||!strcmp(sub,"var")){ char vn[64]; sscanf(s,"%63s",vn); s+=strlen(vn);
        while(*s==' '||*s=='"')s++; char lbl[81]; snprintf(lbl,sizeof lbl,"%s",s);
        for(int i=(int)strlen(lbl)-1;i>=0&&(lbl[i]=='"'||lbl[i]==' ');i--)lbl[i]=0;
        int vi=var_find(c->f,vn); if(vi<0){tea_err("label: %s not found\n",vn);return 111;}
        snprintf(c->f->vars[vi].vlabel,81,"%s",lbl); return 0; }
    if(!strcmp(sub,"define")){           /* label define name # "txt" # "txt" ... */
        char nm[33]; sscanf(s,"%32s",nm); s+=strlen(nm); while(*s==' ')s++;
        VLabel *L=vlabel_ensure(c->ws,nm);
        while(*s){ while(*s==' ')s++; if(!*s)break;
            char *e; double v=strtod(s,&e); if(e==s)break; s=e; while(*s==' ')s++;
            if(*s=='"'){ s++; char txt[256]; int n=0;
                while(*s&&*s!='"'&&n<255)txt[n++]=*s++; txt[n]=0; if(*s=='"')s++;
                vlabel_put(L,v,txt); }
        }
        return 0; }
    if(!strcmp(sub,"values")){           /* label values varlist lblname */
        char rest[1024]; snprintf(rest,sizeof rest,"%s",s);
        /* last token is the label name (or . to clear) */
        char *sp=NULL; char *toks[64]; int nt=0;
        for(char *t=strtok_r(rest," ",&sp);t&&nt<64;t=strtok_r(NULL," ",&sp))toks[nt++]=t;
        if(nt<2){ tea_err("label values: need varlist and label name\n"); return 198; }
        const char *lname=toks[nt-1];
        for(int k=0;k<nt-1;k++){ int vi=var_find(c->f,toks[k]);
            if(vi<0){ tea_err("label values: %s not found\n",toks[k]); return 111; }
            if(!strcmp(lname,".")) c->f->vars[vi].vallab[0]=0;
            else snprintf(c->f->vars[vi].vallab,33,"%s",lname); }
        return 0; }
    if(!strcmp(sub,"list")){             /* label list [name] */
        char nm[33]=""; sscanf(s,"%32s",nm);
        for(VLabel *L=c->ws->vlabels;L;L=L->next){ if(nm[0]&&strcmp(L->name,nm))continue;
            printf("%s:\n",L->name);
            for(VLItem *it=L->items;it;it=it->next) printf("%11g %s\n",it->val,it->txt); }
        return 0; }
    if(!strcmp(sub,"data")){             /* label data "text" — frame dataset label */
        /* skip optional opening quote and trailing whitespace/quote */
        while(*s==' '||*s=='"')s++;
        char lbl[81]; snprintf(lbl,sizeof lbl,"%s",s);
        for(int i=(int)strlen(lbl)-1;i>=0&&(lbl[i]=='"'||lbl[i]==' '||lbl[i]=='\n');i--)lbl[i]=0;
        snprintf(c->f->data_label, sizeof c->f->data_label, "%s", lbl);
        return 0;
    }
    if(!strcmp(sub,"drop")||!strcmp(sub,"dir")) return 0;
    return 0;
}
static int do_format(Cmd *c){ /* format var %fmt  OR  format %fmt var */
    char a[64],b[64]; if(sscanf(c->varlist,"%63s %63s",a,b)!=2)return 198;
    char *fmt=a[0]=='%'?a:b, *vn=a[0]=='%'?b:a;
    int *vs=NULL,nv=varlist_expand(c->f,vn,&vs);
    for(int i=0;i<nv;i++) snprintf(c->f->vars[vs[i]].format,33,"%s",fmt);
    free(vs); return 0; }

/* ---- sort / gsort ------------------------------------------------------ */
static int do_sort(Cmd *c){ int *vs=NULL,nv=varlist_expand(c->f,c->varlist,&vs);
    if(nv<0){tea_err("sort: var not found\n");return 111;}
    frame_physical_sort(c->f,vs,nv,NULL); free(vs); return 0; }
static int do_gsort(Cmd *c){
    int keys[64],desc[64],nk=0; char buf[512]; snprintf(buf,sizeof buf,"%s",c->varlist);
    char *sv=NULL;
    for(char *t=strtok_r(buf," ",&sv);t&&nk<64;t=strtok_r(NULL," ",&sv)){
        int d=0; if(*t=='-'){d=1;t++;} else if(*t=='+')t++;
        int vi=var_find(c->f,t); if(vi<0){tea_err("gsort: %s?\n",t);free(NULL);return 111;}
        keys[nk]=vi; desc[nk]=d; nk++; }
    frame_physical_sort(c->f,keys,nk,desc); return 0; }

/* ---- tabulate (oneway) ------------------------------------------------- */
static int do_tabulate(Cmd *c){
    /* Stata: tabulate accepts fweight (integer) and aweight; rejects iweight
     * and pweight ("iweight not allowed r(101)"). */
    int wrc = validate_wtype(c, WTM_FW|WTM_AW, "tabulate");
    if(wrc) return wrc;
    int *vs=NULL,nv;
    int n_temps = 0;
    const char *vlerr = NULL;
    nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    if(nv<1){tea_err("tabulate: %s\n", vlerr ? vlerr : "need a variable"); free(vs); return 198;}
    Variable *v=&c->f->vars[vs[0]];
    /* keys can be much longer than 64 for real-world string data; use 256
     * which fits any reasonable category label, and we cap display width
     * separately below. */
    typedef struct{char key[256];double n;}Cell; Cell *t=NULL; int nt=0,cap=0;
    Node *ifn=NULL; const char *pe; if(c->ifexp[0])ifn=expr_parse(c->ifexp,c->f,&pe);
    if(c->ifexp[0]&&!ifn){ tea_err("if error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    Node *wn=NULL; if(c->wexp[0]) wn=expr_parse(c->wexp,c->f,&pe);
    if(c->wexp[0]&&!wn){ node_free(ifn); tea_err("weight error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    EvalCtx ec={0}; ec.f=c->f;
    for(size_t i=0;i<c->f->nobs;i++){
        if(ifn){ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs;if(!expr_eval_bool(ifn,&ec))continue;}
        double w=1.0; if(wn){ ec.i=i;ec.n=(long)i+1;ec.N=(long)c->f->nobs; EVal wv=expr_eval(wn,&ec);
            w=wv.is_str?SV_MISS:wv.num; eval_free(&wv); if(sv_is_miss(w)||w<=0)continue; }
        /* fmt_cell would use a 128-byte temp; route string values directly */
        char k[256];
        if(v->type==VT_STR){ snprintf(k,sizeof k,"%s", v->str[i] ? v->str[i] : ""); }
        else { char tmp[128]; fmt_cell(v,i,tmp,sizeof tmp); snprintf(k,sizeof k,"%s",tmp); }
        int f=-1; for(int j=0;j<nt;j++)if(!strcmp(t[j].key,k)){f=j;break;}
        if(f<0){ if(nt==cap){cap=cap?cap*2:16;t=realloc(t,cap*sizeof*t);} snprintf(t[nt].key,256,"%s",k);t[nt].n=0;f=nt++; }
        t[f].n+=w;
    }
    for(int i=0;i<nt;i++)for(int j=i+1;j<nt;j++)if(strcmp(t[i].key,t[j].key)>0){Cell tmp=t[i];t[i]=t[j];t[j]=tmp;}
    double tot=0; for(int i=0;i<nt;i++)tot+=t[i].n;

    /* Dynamic column width: max of (var name, longest key, "Total"), capped
     * at 60 chars.  Anything longer is truncated with an ellipsis. */
    int w = u8width(v->name);
    if((int)strlen("Total") > w) w = (int)strlen("Total");
    for(int i=0;i<nt;i++){ int k = u8width(t[i].key); if(k > w) w = k; }
    if(w > 60) w = 60;
    if(w < 14) w = 14;        /* keep a minimum width */

    /* helper: render a key, truncating with "..." if it exceeds w */
    #define RENDER_KEY(buf, sz, key) do { \
        if(u8width(key) <= w) snprintf(buf, sz, "%s", key); \
        else { /* truncate at a codepoint boundary to display width w-3 */ \
            int bytes = 0, cols = 0; \
            for(const unsigned char *p=(const unsigned char*)(key); *p; p++){ \
                if((*p & 0xC0) != 0x80){ if(cols == w-3) break; cols++; } \
                bytes++; } \
            snprintf(buf, sz, "%.*s...", bytes, key); } \
    } while(0)

    /* header */
    printf("%*s |      Freq.     Percent\n", w, v->name);
    for(int i=0;i<w+1;i++) putchar('-'); printf("+----------------------\n");
    for(int i=0;i<nt;i++){
        char shown[300]; RENDER_KEY(shown, sizeof shown, t[i].key);
        printf("%*s | %10.0f   %8.2f\n", u8pad(shown, w), shown, t[i].n, 100.0*t[i].n/(tot?tot:1));
    }
    for(int i=0;i<w+1;i++) putchar('-'); printf("+----------------------\n");
    printf("%*s | %10.0f     100.00\n", w, "Total", tot);
    #undef RENDER_KEY
    free(t);free(vs);node_free(ifn);node_free(wn);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- tabstat ----------------------------------------------------------- */
/* tabstat varlist [if] [in], statistics(stat1 stat2 ...) [by(groupvar)]
 *                            [columns(statistics|variables)]
 *
 * A compact stat-by-variable (or variable-by-stat) table.  Heavily used by
 * Stata users — much more compact than `summarize` for many vars.
 *
 * Stats supported: mean, sd, var, cv, min, max, range, sum, count, n,
 * median, p1, p5, p10, p25, p50, p75, p90, p95, p99, iqr.
 */
typedef struct { const char *name; const char *header; } TabstatStat;
static const TabstatStat g_tabstat_stats[] = {
    {"mean",   "mean"},
    {"sd",     "sd"},
    {"var",    "variance"},
    {"cv",     "cv"},
    {"min",    "min"},
    {"max",    "max"},
    {"range",  "range"},
    {"sum",    "sum"},
    {"count",  "N"},
    {"n",      "N"},
    {"median", "p50"},
    {"p1",     "p1"},
    {"p5",     "p5"},
    {"p10",    "p10"},
    {"p25",    "p25"},
    {"p50",    "p50"},
    {"p75",    "p75"},
    {"p90",    "p90"},
    {"p95",    "p95"},
    {"p99",    "p99"},
    {"iqr",    "iqr"},
    {NULL, NULL}
};
/* Compute a single statistic over an already-collected, sorted value buffer.
 * Returns SV_MISS if the stat is undefined for the data. */
static double tabstat_compute(const char *stat, const double *xs, size_t nx, double mean, double sd){
    if(nx == 0) return SV_MISS;
    if(!strcmp(stat,"mean"))   return mean;
    if(!strcmp(stat,"sd"))     return sd;
    if(!strcmp(stat,"var"))    return sv_is_miss(sd) ? SV_MISS : sd*sd;
    if(!strcmp(stat,"cv"))     return (sv_is_miss(sd) || mean==0) ? SV_MISS : sd/mean;
    if(!strcmp(stat,"min"))    return xs[0];
    if(!strcmp(stat,"max"))    return xs[nx-1];
    if(!strcmp(stat,"range"))  return xs[nx-1] - xs[0];
    if(!strcmp(stat,"sum"))    return mean * (double)nx;
    if(!strcmp(stat,"count") || !strcmp(stat,"n")) return (double)nx;
    /* percentile group */
    double pct = -1;
    if(!strcmp(stat,"median") || !strcmp(stat,"p50")) pct = 50;
    else if(!strcmp(stat,"p1"))  pct = 1;
    else if(!strcmp(stat,"p5"))  pct = 5;
    else if(!strcmp(stat,"p10")) pct = 10;
    else if(!strcmp(stat,"p25")) pct = 25;
    else if(!strcmp(stat,"p75")) pct = 75;
    else if(!strcmp(stat,"p90")) pct = 90;
    else if(!strcmp(stat,"p95")) pct = 95;
    else if(!strcmp(stat,"p99")) pct = 99;
    if(pct > 0){
        if(nx == 1) return xs[0];
        double r = pct/100.0 * (double)(nx-1);
        long lo = (long)r; double f = r - (double)lo;
        return ((size_t)lo+1 < nx) ? xs[lo]*(1.0-f) + xs[lo+1]*f : xs[lo];
    }
    if(!strcmp(stat,"iqr")){
        double q25, q75;
        if(nx == 1) { q25 = q75 = xs[0]; }
        else {
            double r1 = 0.25*(double)(nx-1), r3 = 0.75*(double)(nx-1);
            long lo1=(long)r1; double f1=r1-(double)lo1;
            long lo3=(long)r3; double f3=r3-(double)lo3;
            q25 = ((size_t)lo1+1<nx)? xs[lo1]*(1.0-f1)+xs[lo1+1]*f1 : xs[lo1];
            q75 = ((size_t)lo3+1<nx)? xs[lo3]*(1.0-f3)+xs[lo3+1]*f3 : xs[lo3];
        }
        return q75 - q25;
    }
    return SV_MISS;
}

/* ---- tabstat ----------------------------------------------------------- *
 *
 * Stata: `tabstat varlist [if] [in] [weight] [, statistics(stat ...) by(g) columns(stats|variables)]`
 *
 * Compact statistics tables, more flexible than `summarize` for
 * producing one's preferred set of stats per variable (means, percentiles,
 * counts, etc.).  Stat keywords: n, mean, sum, sd, variance, min, max,
 * range, p1..p99, median, iqr.
 *
 * by(group) produces a row per group; columns(variables) transposes
 * so variables are columns instead of rows.
 */
static int do_tabstat(Cmd *c){
    int *vs=NULL,nv;
    int n_temps = 0;
    const char *vlerr = NULL;
    if(c->varlist[0]) nv = tsop_expand_varlist(c->f, c->varlist, &vs, &n_temps, &vlerr);
    else { tea_err("tabstat: varlist required\n"); return 198; }
    if(nv<0){ tea_err("tabstat: %s\n", vlerr ? vlerr : "variable not found"); free(vs); return 111; }
    /* explicit-string check, matching summarize's contract */
    bool has_wildcard = strpbrk(c->varlist, "*?") != NULL;
    if(!has_wildcard){
        for(int j=0;j<nv;j++){
            Variable *v = &c->f->vars[vs[j]];
            if(v->type != VT_NUM){
                tea_err("tabstat: %s is a string variable (cannot compute statistics)\n", v->name);
                free(vs); tsop_drop_temps(c->f, n_temps); return 109;
            }
        }
    }
    /* parse statistics() option; default = mean */
    char statsopt[512]=""; opt_value(c->options,"statistics",statsopt,sizeof statsopt);
    if(!statsopt[0]) opt_value(c->options,"stats",statsopt,sizeof statsopt);
    if(!statsopt[0]) snprintf(statsopt,sizeof statsopt,"mean");
    /* strip any surrounding parens left over */
    if(statsopt[0]=='(' || statsopt[0]=='"'){
        char *e = strrchr(statsopt, statsopt[0]=='('?')':'"');
        if(e>statsopt) *e = 0;
        memmove(statsopt, statsopt+1, strlen(statsopt+1)+1);
    }
    /* tokenize */
    char statnames[16][32]; int nstats=0;
    { char buf[512]; snprintf(buf,sizeof buf,"%s",statsopt);
      char *sv=NULL;
      for(char *t=strtok_r(buf," ,",&sv); t && nstats<16; t=strtok_r(NULL," ,",&sv)){
        /* validate */
        int ok=0;
        for(int i=0; g_tabstat_stats[i].name; i++)
            if(!strcmp(g_tabstat_stats[i].name, t)){ ok=1; break; }
        if(!ok){ tea_err("tabstat: unknown statistic '%s'\n", t); free(vs); tsop_drop_temps(c->f, n_temps); return 198; }
        snprintf(statnames[nstats++],32,"%s",t);
      }
    }
    /* parse by() option */
    char byopt[64]=""; opt_value(c->options,"by",byopt,sizeof byopt);
    int byvar = -1;
    if(byopt[0]){ byvar = var_find(c->f, byopt);
        if(byvar < 0){ tea_err("tabstat: by() variable %s not found\n",byopt); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    }
    /* columns(statistics|variables) controls table orientation.
     *
     * Per the Stata manual: 'columns(variables) is the default when more
     * than one variable is specified'.  With a single variable, the
     * statistics-as-columns layout reads more naturally; with multiple,
     * variables-as-columns is more compact. */
    char colopt[32]=""; opt_value(c->options,"columns",colopt,sizeof colopt);
    int cols_are_stats = (nv == 1);
    if(colopt[0]){
        if(!strncmp(colopt,"var",3)) cols_are_stats = 0;
        else if(!strncmp(colopt,"stat",4)) cols_are_stats = 1;
        else { tea_err("tabstat: columns() must be 'statistics' or 'variables'\n"); free(vs); tsop_drop_temps(c->f, n_temps); return 198; }
    }
    Node *ifn=NULL; const char *pe; if(c->ifexp[0]) ifn=expr_parse(c->ifexp,c->f,&pe);
    if(c->ifexp[0]&&!ifn){ tea_err("if error: %s\n", pe); free(vs); tsop_drop_temps(c->f, n_temps); return 111; }
    EvalCtx ec={0}; ec.f=c->f;
    /* set up by-groups (if by() given, sort first then walk groups) */
    int gcnt = 1;
    size_t *gLo=NULL, *gHi=NULL;
    int saved_byvars[1] = {byvar};
    if(byvar >= 0){
        frame_physical_sort(c->f, saved_byvars, 1, NULL);
        gcnt = by_groups(c->f, saved_byvars, 1, &gLo, &gHi);
    }
    /* For each group, compute all (stat × var) cells, then render.
     * `header_printed` tracks whether we've emitted the "Summary statistics:"
     * banner + table column-header yet.  We can't gate that on g==0 because
     * the first few groups may get skipped via the if/in filter. */
    bool header_printed = false;
    for(int g=0; g<gcnt; g++){
        size_t r_a = byvar>=0 ? gLo[g] : 0;
        size_t r_b = byvar>=0 ? gHi[g] : (c->f->nobs? c->f->nobs-1 : 0);
        /* Count how many rows in this group pass if/in filters.  If none do,
         * skip the entire group — otherwise the user gets a row of dots for
         * every excluded by-value, which is unhelpful.  Stata's tabstat
         * silently omits empty groups. */
        long group_n = 0;
        for(size_t i=r_a; i<=r_b; i++){
            if(c->in_lo>0 && (long)i+1<c->in_lo) continue;
            if(c->in_hi>0 && (long)i+1>c->in_hi) continue;
            if(ifn){ ec.i=i; ec.n=(long)i+1; ec.N=(long)c->f->nobs;
                if(!expr_eval_bool(ifn,&ec)) continue; }
            group_n++;
        }
        if(group_n == 0 && byvar >= 0) continue;
        /* Per-variable: compute mean, sd, and sorted value buffer once;
         * then derive each requested stat from those. */
        double *table = malloc((size_t)nv * (size_t)nstats * sizeof(double));
        for(int j=0;j<nv;j++){
            Variable *v = &c->f->vars[vs[j]];
            if(v->type != VT_NUM){
                for(int s=0;s<nstats;s++) table[j*nstats + s] = SV_MISS;
                continue;
            }
            double *xs = NULL; size_t nx=0, xcap=0;
            double sum=0, sumsq=0;
            for(size_t i=r_a;i<=r_b;i++){
                if(c->in_lo>0&&(long)i+1<c->in_lo)continue;
                if(c->in_hi>0&&(long)i+1>c->in_hi)continue;
                if(ifn){ ec.i=i; ec.n=(long)i+1; ec.N=(long)c->f->nobs;
                    if(!expr_eval_bool(ifn,&ec)) continue; }
                double x = v->num[i]; if(sv_is_miss(x)) continue;
                if(nx==xcap){ xcap = xcap? xcap*2 : 256; xs = realloc(xs, xcap*sizeof(double)); }
                xs[nx++] = x; sum += x; sumsq += x*x;
            }
            if(nx > 0) qsort(xs, nx, sizeof(double), cmp_double_asc);
            double mean = nx ? sum/(double)nx : SV_MISS;
            double sd = (nx > 1) ? sqrt((sumsq - sum*sum/(double)nx)/(double)(nx-1)) : SV_MISS;
            for(int s=0;s<nstats;s++)
                table[j*nstats + s] = tabstat_compute(statnames[s], xs, nx, mean, sd);
            free(xs);
        }
        /* ---- render Stata-style ------------------------------------- */
        /* Stata's number format inside tabstat: roughly %9.0g, which gives
         * full significant digits without trailing zeros and falls back to
         * scientific only when the number doesn't fit.  We keep right-
         * justification in a fixed-width column. */
        #define CELLW 10
        #define FMT_NUM(buf, x) do { \
            if(sv_is_miss(x)) snprintf((buf), sizeof(buf), "%*s", CELLW, "."); \
            else snprintf((buf), sizeof(buf), "%*.*g", CELLW, 7, (x)); \
        } while(0)

        /* Helper: render the Stata-style header once per group (or just
         * once if no by).  Print on the first group that survives the
         * if/in filter, not necessarily g==0. */
        if(!header_printed){
            printf("\nSummary statistics: ");
            for(int s=0;s<nstats;s++) printf("%s%s", s? ", ":"", statnames[s]);
            printf("\n  for variables: ");
            for(int j=0;j<nv;j++) printf("%s%s", j? " ":"", c->f->vars[vs[j]].name);
            printf("\n");
            if(byvar >= 0){
                printf("  by categories of: %s\n", c->f->vars[byvar].name);
            }
            /* Don't mark header_printed yet — the by-table column header
             * below also needs to fire once, and it's keyed off the same
             * "first surviving group" condition.  The flag gets set after
             * both headers have run. */
        }

        if(byvar < 0){
            /* ---- no by(): single table ---- */
            if(cols_are_stats){
                /* rows = vars, cols = stats */
                int namew = 12;
                for(int j=0;j<nv;j++){ int L=(int)strlen(c->f->vars[vs[j]].name); if(L>namew)namew=L; }
                printf("\n    variable |");
                for(int s=0;s<nstats;s++){
                    const char *hdr = statnames[s];
                    for(int i=0; g_tabstat_stats[i].name; i++)
                        if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                    printf(" %*s", CELLW, hdr);
                }
                printf("\n-------------+");
                for(int s=0;s<nstats;s++) printf("-----------");
                printf("\n");
                for(int j=0;j<nv;j++){
                    printf("%12s |", c->f->vars[vs[j]].name);
                    for(int s=0;s<nstats;s++){
                        char b[32]; FMT_NUM(b, table[j*nstats+s]);
                        printf(" %s", b);
                    }
                    printf("\n");
                }
            } else {
                /* rows = stats, cols = vars */
                printf("\n       stats |");
                for(int j=0;j<nv;j++) printf(" %*s", CELLW, c->f->vars[vs[j]].name);
                printf("\n-------------+");
                for(int j=0;j<nv;j++) printf("-----------");
                printf("\n");
                for(int s=0;s<nstats;s++){
                    const char *hdr = statnames[s];
                    for(int i=0; g_tabstat_stats[i].name; i++)
                        if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                    printf("%12s |", hdr);
                    for(int j=0;j<nv;j++){
                        char b[32]; FMT_NUM(b, table[j*nstats+s]);
                        printf(" %s", b);
                    }
                    printf("\n");
                }
            }
            header_printed = true;
        } else {
            /* ---- with by(): one block per group ----
             *
             * Two layouts (per `columns()`):
             *
             *  - columns(variables): stats stack inside the group, variable
             *      names across the top.  Compact for multi-variable cases.
             *
             *  - columns(statistics): stat names across the top.  With one
             *      variable, each group is one row.  With multiple variables,
             *      each group is a sub-block where rows are variables.
             */
            char by_lbl[64];
            Variable *bv = &c->f->vars[byvar];
            if(bv->type==VT_STR) snprintf(by_lbl, sizeof by_lbl, "%s", bv->str[r_a] ? bv->str[r_a] : "");
            else if(sv_is_miss(bv->num[r_a])) snprintf(by_lbl, sizeof by_lbl, ".");
            else snprintf(by_lbl, sizeof by_lbl, "%g", bv->num[r_a]);

            if(cols_are_stats){
                /* ---- columns(statistics) ---- */
                if(!header_printed){
                    /* Header: group-name column then stat names */
                    printf("\n%12s |", bv->name);
                    for(int s=0;s<nstats;s++){
                        const char *hdr = statnames[s];
                        for(int i=0; g_tabstat_stats[i].name; i++)
                            if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                        printf(" %*s", CELLW, hdr);
                    }
                    printf("\n-------------+");
                    for(int s=0;s<nstats;s++) printf("-----------");
                    printf("\n");
                    header_printed = true;
                }
                if(nv == 1){
                    /* Single var: one row per group, stat-columns. */
                    printf("%12s |", by_lbl);
                    for(int s=0;s<nstats;s++){
                        char b[32]; FMT_NUM(b, table[0*nstats+s]);
                        printf(" %s", b);
                    }
                    printf("\n");
                } else {
                    /* Multi-var: group label on its own row, then variable
                     * rows underneath.  Mirrors estpost tabstat output. */
                    printf("%12s |\n", by_lbl);
                    for(int j=0;j<nv;j++){
                        printf("%12s |", c->f->vars[vs[j]].name);
                        for(int s=0;s<nstats;s++){
                            char b[32]; FMT_NUM(b, table[j*nstats+s]);
                            printf(" %s", b);
                        }
                        printf("\n");
                    }
                    /* separator between groups */
                    printf("-------------+");
                    for(int s=0;s<nstats;s++) printf("-----------");
                    printf("\n");
                }
            } else {
                /* ---- columns(variables) ---- */
                if(!header_printed){
                    /* Header: group-name column then variable names */
                    printf("\n%12s |", bv->name);
                    for(int j=0;j<nv;j++) printf(" %*s", CELLW, c->f->vars[vs[j]].name);
                    printf("\n-------------+");
                    for(int j=0;j<nv;j++) printf("-----------");
                    printf("\n");
                    header_printed = true;
                }
                /* Single-stat: one row per group, var-columns. */
                if(nstats == 1){
                    printf("%12s |", by_lbl);
                    for(int j=0;j<nv;j++){
                        char b[32]; FMT_NUM(b, table[j*nstats+0]);
                        printf(" %s", b);
                    }
                    printf("\n");
                } else {
                    /* Multi-stat: stack stats inside the group; first row has
                     * the group label, subsequent rows blank in the label slot.
                     * A trailing "(stat)" hint appears at the right end so the
                     * reader doesn't lose track. */
                    for(int s=0;s<nstats;s++){
                        const char *hdr = statnames[s];
                        for(int i=0; g_tabstat_stats[i].name; i++)
                            if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                        if(s == 0) printf("%12s |", by_lbl);
                        else       printf("%12s |", "");
                        for(int j=0;j<nv;j++){
                            char b[32]; FMT_NUM(b, table[j*nstats+s]);
                            printf(" %s", b);
                        }
                        printf("    (%s)\n", hdr);
                    }
                    /* separator between groups (and before Total) */
                    printf("-------------+");
                    for(int j=0;j<nv;j++) printf("-----------");
                    printf("\n");
                }
            }
        }
        #undef FMT_NUM
        #undef CELLW
        free(table);
    }
    /* Total row: re-run the computation on the full sample with no by(). */
    if(byvar >= 0){
        /* count rows passing if/in across the whole frame; if none, suppress
         * the Total row entirely (otherwise we'd print a stray header with
         * a row of dots — unhelpful). */
        long total_n = 0;
        for(size_t i=0; i<c->f->nobs; i++){
            if(c->in_lo>0 && (long)i+1<c->in_lo) continue;
            if(c->in_hi>0 && (long)i+1>c->in_hi) continue;
            if(ifn){ ec.i=i; ec.n=(long)i+1; ec.N=(long)c->f->nobs;
                if(!expr_eval_bool(ifn,&ec)) continue; }
            total_n++;
        }
        if(total_n == 0){
            free(gLo); free(gHi); free(vs); node_free(ifn);
            if(!header_printed) printf("\nno observations\n");
            return 0;
        }
        double *table = malloc((size_t)nv * (size_t)nstats * sizeof(double));
        for(int j=0;j<nv;j++){
            Variable *v = &c->f->vars[vs[j]];
            if(v->type != VT_NUM){ for(int s=0;s<nstats;s++) table[j*nstats+s] = SV_MISS; continue; }
            double *xs = NULL; size_t nx=0, xcap=0;
            double sum=0, sumsq=0;
            for(size_t i=0; i<c->f->nobs; i++){
                if(c->in_lo>0 && (long)i+1<c->in_lo) continue;
                if(c->in_hi>0 && (long)i+1>c->in_hi) continue;
                if(ifn){ ec.i=i; ec.n=(long)i+1; ec.N=(long)c->f->nobs;
                    if(!expr_eval_bool(ifn,&ec)) continue; }
                double x = v->num[i]; if(sv_is_miss(x)) continue;
                if(nx==xcap){ xcap = xcap? xcap*2 : 256; xs = realloc(xs, xcap*sizeof(double)); }
                xs[nx++] = x; sum += x; sumsq += x*x;
            }
            if(nx > 0) qsort(xs, nx, sizeof(double), cmp_double_asc);
            double mean = nx ? sum/(double)nx : SV_MISS;
            double sd = (nx > 1) ? sqrt((sumsq - sum*sum/(double)nx)/(double)(nx-1)) : SV_MISS;
            for(int s=0;s<nstats;s++)
                table[j*nstats + s] = tabstat_compute(statnames[s], xs, nx, mean, sd);
            free(xs);
        }
        #define CELLW 10
        #define FMT_NUM(buf, x) do { \
            if(sv_is_miss(x)) snprintf((buf), sizeof(buf), "%*s", CELLW, "."); \
            else snprintf((buf), sizeof(buf), "%*.*g", CELLW, 7, (x)); \
        } while(0)
        /* If no group ever rendered (every group was filtered out), the
         * banner+column-header haven't been emitted yet — do it now so the
         * Total row has context.  The header layout has to match the body
         * layout chosen by columns(). */
        if(!header_printed){
            printf("\nSummary statistics: ");
            for(int s=0;s<nstats;s++) printf("%s%s", s? ", ":"", statnames[s]);
            printf("\n  for variables: ");
            for(int j=0;j<nv;j++) printf("%s%s", j? " ":"", c->f->vars[vs[j]].name);
            printf("\n  by categories of: %s\n", c->f->vars[byvar].name);
            if(cols_are_stats){
                printf("\n%12s |", c->f->vars[byvar].name);
                for(int s=0;s<nstats;s++){
                    const char *hdr = statnames[s];
                    for(int i=0; g_tabstat_stats[i].name; i++)
                        if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                    printf(" %*s", CELLW, hdr);
                }
                printf("\n-------------+");
                for(int s=0;s<nstats;s++) printf("-----------");
            } else {
                printf("\n%12s |", c->f->vars[byvar].name);
                for(int j=0;j<nv;j++) printf(" %*s", CELLW, c->f->vars[vs[j]].name);
                printf("\n-------------+");
                for(int j=0;j<nv;j++) printf("-----------");
            }
            printf("\n");
        }
        /* Render Total in the layout chosen by columns(). */
        if(cols_are_stats){
            if(nv == 1){
                printf("%12s |", "Total");
                for(int s=0;s<nstats;s++){ char b[32]; FMT_NUM(b, table[0*nstats+s]); printf(" %s", b); }
                printf("\n");
            } else {
                printf("%12s |\n", "Total");
                for(int j=0;j<nv;j++){
                    printf("%12s |", c->f->vars[vs[j]].name);
                    for(int s=0;s<nstats;s++){ char b[32]; FMT_NUM(b, table[j*nstats+s]); printf(" %s", b); }
                    printf("\n");
                }
            }
        } else {
            if(nstats == 1){
                printf("%12s |", "Total");
                for(int j=0;j<nv;j++){ char b[32]; FMT_NUM(b, table[j*nstats+0]); printf(" %s", b); }
                printf("\n");
            } else {
                for(int s=0;s<nstats;s++){
                    const char *hdr = statnames[s];
                    for(int i=0; g_tabstat_stats[i].name; i++)
                        if(!strcmp(g_tabstat_stats[i].name, statnames[s])){ hdr = g_tabstat_stats[i].header; break; }
                    if(s == 0) printf("%12s |", "Total");
                    else       printf("%12s |", "");
                    for(int j=0;j<nv;j++){ char b[32]; FMT_NUM(b, table[j*nstats+s]); printf(" %s", b); }
                    printf("    (%s)\n", hdr);
                }
            }
        }
        #undef FMT_NUM
        #undef CELLW
        free(table);
    }
    free(gLo); free(gHi);
    free(vs); node_free(ifn);
    tsop_drop_temps(c->f, n_temps);
    return 0;
}

/* ---- tsset / xtset ----------------------------------------------------- */
static int do_tsset(Cmd *c,int xt){
    char a[64]="",b[64]=""; int k=sscanf(c->varlist,"%63s %63s",a,b);
    if(k<1){ if(c->f->ts_time>=0)printf("  time variable: %s\n",c->f->vars[c->f->ts_time].name); return 0; }
    int pv=-1,tv=-1;
    if(xt){ pv=var_find(c->f,a); tv=k>1?var_find(c->f,b):-1; }
    else  { tv=var_find(c->f,a); }
    if(xt&&pv<0){tea_err("%sset: panel var?\n",xt?"xt":"ts");return 111;}
    if(tv<0&&!(xt&&k==1)){tea_err("tsset: time var not found\n");return 111;}
    /* sort by panel,time so groups are contiguous */
    int keys[2],nk=0; if(pv>=0)keys[nk++]=pv; if(tv>=0)keys[nk++]=tv;
    if(nk)frame_physical_sort(c->f,keys,nk,NULL);
    c->f->ts_panel=pv; c->f->ts_time=tv; c->f->ts_delta=1;
    /* infer display format from option or var format */
    char fb[16]; if(opt_value(c->options,"format",fb,sizeof fb)&&tv>=0)snprintf(c->f->vars[tv].format,33,"%s",fb);
    if(!c->quiet){ if(pv>=0)printf("panel variable: %s\n",c->f->vars[pv].name);
        if(tv>=0)printf(" time variable: %s\n",c->f->vars[tv].name);
        printf("         delta: 1 unit\n"); }
    return 0;
}

/* ---- xtdescribe -------------------------------------------------------- */
/* xtdescribe — summarize the panel structure declared by xtset.  Shows:
 *   - number of panels (n) and time periods (T)
 *   - time variable range and delta
 *   - distribution of obs-per-panel (min, p25, p50, p75, max)
 *   - whether the panel is balanced (every panel has the same number of obs).
 *
 * Stata's xtdescribe also prints a pattern frequency table; we omit that
 * for v1 — the balanced/unbalanced summary + distribution is usually
 * enough. */
static int do_xtdescribe(Cmd *c)
{
    Frame *f = c->f;
    if(f->ts_panel < 0 || f->ts_time < 0){
        tea_err("xtdescribe: data not xtset\n");
        return 459;
    }
    Variable *pv = &f->vars[f->ts_panel];
    Variable *tv = &f->vars[f->ts_time];
    bool pv_str = (pv->type == VT_STR);

    /* Per-panel aggregation.  Since xtset sorted by (panel,time), panels
     * are contiguous; we can stream rows and start a new group when the
     * panel id changes. */
    typedef struct {
        size_t first_row;
        long   count;
        double tmin, tmax;
    } PanelInfo;
    PanelInfo *panels = NULL;
    int n_panels = 0, cap_panels = 0;
    double t_min_all = INFINITY, t_max_all = -INFINITY;

    for(size_t i = 0; i < f->nobs; i++){
        double t = tv->num[i];
        if(sv_is_miss(t)) continue;
        if(t < t_min_all) t_min_all = t;
        if(t > t_max_all) t_max_all = t;

        bool new_panel = (n_panels == 0);
        if(!new_panel){
            size_t prev = panels[n_panels-1].first_row;
            if(pv_str) new_panel = (strcmp(pv->str[i], pv->str[prev]) != 0);
            else       new_panel = (pv->num[i] != pv->num[prev]);
        }
        if(new_panel){
            if(n_panels == cap_panels){
                cap_panels = cap_panels ? cap_panels*2 : 32;
                panels = realloc(panels, cap_panels * sizeof *panels);
            }
            panels[n_panels].first_row = i;
            panels[n_panels].count = 0;
            panels[n_panels].tmin = INFINITY;
            panels[n_panels].tmax = -INFINITY;
            n_panels++;
        }
        PanelInfo *p = &panels[n_panels-1];
        p->count++;
        if(t < p->tmin) p->tmin = t;
        if(t > p->tmax) p->tmax = t;
    }

    if(n_panels == 0){
        printf("(no observations with non-missing time variable)\n");
        free(panels);
        return 0;
    }

    long total_obs = 0;
    long min_count = panels[0].count, max_count = panels[0].count;
    for(int j = 0; j < n_panels; j++){
        total_obs += panels[j].count;
        if(panels[j].count < min_count) min_count = panels[j].count;
        if(panels[j].count > max_count) max_count = panels[j].count;
    }
    bool balanced = (min_count == max_count);

    /* Header lines */
    char buf_first[64], buf_last[64];
    if(pv_str){
        snprintf(buf_first, sizeof buf_first, "%s", pv->str[panels[0].first_row]);
        snprintf(buf_last,  sizeof buf_last,  "%s", pv->str[panels[n_panels-1].first_row]);
    } else {
        snprintf(buf_first, sizeof buf_first, "%g", pv->num[panels[0].first_row]);
        snprintf(buf_last,  sizeof buf_last,  "%g", pv->num[panels[n_panels-1].first_row]);
    }

    printf("\n");
    if(n_panels >= 2){
        printf("       %s:  %s, ..., %s%*sn = %8d\n",
               pv->name, buf_first, buf_last,
               (int)(40 - strlen(pv->name) - strlen(buf_first) - strlen(buf_last)), "",
               n_panels);
    } else {
        printf("       %s:  %s%*sn = %8d\n",
               pv->name, buf_first,
               (int)(45 - strlen(pv->name) - strlen(buf_first)), "",
               n_panels);
    }
    printf("       %s:  %g, ..., %g%*sT = %8ld\n",
           tv->name, t_min_all, t_max_all,
           (int)(30 - strlen(tv->name)), "",
           max_count);
    printf("              Delta(%s) = %g unit%s\n",
           tv->name, (double)f->ts_delta, f->ts_delta == 1 ? "" : "s");
    printf("              Span(%s)  = %ld periods\n",
           tv->name, (long)(t_max_all - t_min_all + 1));
    printf("              (%s*%s uniquely identifies each observation)\n",
           pv->name, tv->name);
    printf("\n");

    /* Sort counts to compute quartiles. */
    long *cs = malloc(n_panels * sizeof(long));
    for(int j = 0; j < n_panels; j++) cs[j] = panels[j].count;
    /* simple insertion sort — n_panels is typically small */
    for(int i = 1; i < n_panels; i++){
        long v = cs[i]; int j = i-1;
        while(j >= 0 && cs[j] > v){ cs[j+1] = cs[j]; j--; }
        cs[j+1] = v;
    }
    long p5  = cs[(n_panels-1) *  5 / 100];
    long p25 = cs[(n_panels-1) * 25 / 100];
    long p50 = cs[(n_panels-1) * 50 / 100];
    long p75 = cs[(n_panels-1) * 75 / 100];
    long p95 = cs[(n_panels-1) * 95 / 100];

    printf("     Distribution of T_i:   min      5%%     25%%     50%%     75%%     95%%     max\n");
    printf("                          %5ld %7ld %7ld %7ld %7ld %7ld %7ld\n",
           min_count, p5, p25, p50, p75, p95, max_count);
    free(cs);

    printf("\n");
    if(balanced){
        printf("     %d panels x %ld periods = %ld observations (strongly balanced)\n",
               n_panels, max_count, total_obs);
    } else {
        printf("     %d panels, %ld total observations (unbalanced)\n",
               n_panels, total_obs);
    }
    printf("\n");

    free(panels);
    return 0;
}


static int do_collapse(Cmd *c){
    /* ---- collapse ----------------------------------------------------- *
     *
     * Stata: `collapse (stat) v1 v2 (stat) v3 ... [if] [in] [weight], by(g1 g2)`
     *
     * Reduce the dataset to one observation per by-group, with each
     * v_k replaced by stat(v_k) within the group.  Without by(),
     * collapses to a single observation.  Stat keywords: mean, median,
     * sum, min, max, sd, count, first, last, p25, p50, p75, p10, p90.
     *
     * Renaming form: `collapse (sd) sd_x = x` gives the SD of x stored
     * in a new variable sd_x (otherwise the variable name is kept).
     *
     * Weights: fweight applies frequency replication; aweight/iweight
     * use weighted statistics within each group.  pweight is rejected
     * (matches Stata).
     *
     * IMPORTANT: collapse is destructive — the original dataset is
     * replaced.  Use `frame copy` first to preserve a copy.
     * ------------------------------------------------------------------ */
    /* collapse (stat) v1 v2 (stat) v3 ... , by(g1 g2)
     *
     * Two paren-using forms must be disambiguated at the start of a token:
     *   (mean)              -> stat directive (alphabetic content)
     *   L(1/2).x            -> TS-op token (starts with L/F/D/S, parens
     *                          contain a numlist, not stat name)
     * The disambiguator: '(' at top level marks a stat directive only.
     * Inside a varlist token, parens are nested as part of the TS-op
     * grammar and we read them paren-aware.
     *
     * collapse builds a NEW frame and swaps it in via frame_clear(c->f).
     * Any temp columns added during TS-op resolution are freed by that
     * frame_clear, so we do not need tsop_drop_temps here. */
    char byspec[256]=""; int *bv=NULL,nbv=0;
    if(opt_value(c->options,"by",byspec,sizeof byspec)) nbv=varlist_expand(c->f,byspec,&bv);
    /* parse stat/var groups */
    typedef struct{int vi;int stat;}Agg; Agg ag[128]; int nag=0;
    char sp[2048]; snprintf(sp,sizeof sp,"%s",c->varlist);
    char cur[16]="mean"; char *p=sp;
    while(*p){
        while(*p==' ')p++;
        if(!*p) break;
        if(*p=='('){ char *e=strchr(p,')'); if(!e)break; *e=0; snprintf(cur,sizeof cur,"%s",p+1); p=e+1; continue; }
        /* Paren-aware token: read until top-level whitespace.  Parens
         * inside the token (TS-op syntax) are kept. */
        char tok[256]; int n=0; int depth=0;
        while(*p && (depth>0 || (*p!=' '))){
            if(n < (int)sizeof tok - 1) tok[n++] = *p;
            if(*p == '(') depth++;
            else if(*p == ')' && depth > 0) depth--;
            p++;
        }
        tok[n] = 0;
        if(n){
            int *xs=NULL; int xtemps = 0; const char *vlerr = NULL;
            int nx = tsop_expand_varlist(c->f, tok, &xs, &xtemps, &vlerr);
            if(nx < 0){
                tea_err("collapse: %s\n", vlerr ? vlerr : "variable not found");
                tsop_drop_temps(c->f, xtemps);
                free(xs); free(bv);
                return 111;
            }
            for(int i=0;i<nx;i++){ ag[nag].vi=xs[i];
                ag[nag].stat = !strcmp(cur,"sum")?1:!strcmp(cur,"count")?2:!strcmp(cur,"max")?3:!strcmp(cur,"min")?4:!strcmp(cur,"sd")?5:!strcmp(cur,"median")?6:!strcmp(cur,"first")?7:!strcmp(cur,"last")?8:0;
                nag++; }
            free(xs);
            /* xtemps stay on the frame until frame_clear below */
        }
    }
    if(nbv>0) frame_physical_sort(c->f,bv,nbv,NULL);
    size_t *lo=NULL,*hi=NULL; int ng = nbv>0? by_groups(c->f,bv,nbv,&lo,&hi) : 1;
    int wcol=-1; if(c->wexp[0]) wcol=var_find(c->f,c->wexp);  /* simple: weight is a var */
    /* build new frame */
    Frame *nf=frame_create(c->ws,"__collapse_tmp");
    for(int k=0;k<nbv;k++){ Variable *src=&c->f->vars[bv[k]]; var_add(nf,src->name,src->type); }
    for(int j=0;j<nag;j++) var_add(nf,c->f->vars[ag[j].vi].name,VT_NUM);
    frame_set_nobs(nf,(size_t)(nbv>0?ng:1));
    for(int g=0; g<(nbv>0?ng:1); g++){
        size_t a=nbv>0?lo[g]:0, b=nbv>0?hi[g]:(c->f->nobs?c->f->nobs-1:0);
        int col=0;
        for(int k=0;k<nbv;k++){ Variable *src=&c->f->vars[bv[k]],*dst=&nf->vars[col++];
            if(src->type==VT_NUM)dst->num[g]=src->num[a]; else str_set(dst,g,src->str[a]); }
        for(int j=0;j<nag;j++){ Variable *src=&c->f->vars[ag[j].vi];
            double sw=0,swx=0,swxx=0,mn=INFINITY,mx=-INFINITY;long n=0;double first=SV_MISS,last=SV_MISS;
            for(size_t r=a;r<=b&&c->f->nobs;r++){ double x=src->num[r]; if(sv_is_miss(x))continue;
                double w=1.0; if(wcol>=0){ w=c->f->vars[wcol].num[r]; if(sv_is_miss(w)||w<=0)continue; }
                if(n==0)first=x; last=x; sw+=w; swx+=w*x; swxx+=w*x*x; n++; if(x<mn)mn=x;if(x>mx)mx=x; }
            double mean = sw>0? swx/sw : SV_MISS;
            double res; switch(ag[j].stat){
                case 1:res=swx;break; case 2:res=n;break; case 3:res=n?mx:SV_MISS;break;
                case 4:res=n?mn:SV_MISS;break;
                case 5:res=(n>1&&sw>0)?sqrt((swxx-swx*swx/sw)/(sw-1)):SV_MISS;break;
                case 7:res=first;break; case 8:res=last;break;
                default:res=mean; }
            nf->vars[col++].num[g]=res; }
    }
    free(lo);free(hi);free(bv);
    /* swap collapsed data into the active frame, drop temp */
    char keep[33]; snprintf(keep,sizeof keep,"%s",c->f->name);
    frame_clear(c->f);
    c->f->vars=nf->vars; c->f->nvar=nf->nvar; c->f->cap_var=nf->cap_var; c->f->nobs=nf->nobs;
    nf->vars=NULL; nf->nvar=nf->cap_var=0; nf->nobs=0;
    /* unlink temp frame */
    for(Frame **pp=&c->ws->frames; *pp; pp=&(*pp)->next) if(*pp==nf){ *pp=nf->next; free(nf->sortvars); free(nf); break; }
    frame_unsort(c->f);
    if(!c->quiet) printf("(collapsed to %zu observations)\n",c->f->nobs);
    return 0;
}

/* ---- import / export delimited ---------------------------------------- */
/* shell out to ssconvert (gnumeric) or libreoffice to turn xlsx/ods into csv.
 * Returns 0 on success and writes the temp .csv path into out_path. */
static int convert_spreadsheet(const char *src, const char *sheet, char *out_path, size_t op_sz){
    if(access(src,R_OK)!=0){
        tea_err("import: file %s not found\n", src);
        if(strchr(src,'\\'))
            tea_err("(note: the path contains backslashes \u2014 on Linux/macOS use forward slashes)\n");
        return 601;
    }
    /* try ssconvert first (gnumeric — fast, scriptable) */
    int have_ss = (system("command -v ssconvert >/dev/null 2>&1")==0);

    /* libreoffice can be named 'libreoffice' (Linux), 'soffice' (some installs,
     * and macOS Homebrew), or live inside the macOS app bundle at
     * /Applications/LibreOffice.app/Contents/MacOS/soffice (default macOS install). */
    const char *lo_path = NULL;
    if(system("command -v libreoffice >/dev/null 2>&1")==0) lo_path = "libreoffice";
    else if(system("command -v soffice >/dev/null 2>&1")==0) lo_path = "soffice";
    else if(access("/Applications/LibreOffice.app/Contents/MacOS/soffice", X_OK)==0)
        lo_path = "/Applications/LibreOffice.app/Contents/MacOS/soffice";

    if(!have_ss && !lo_path){
        tea_err(
            "import: excel/ods support requires ssconvert (gnumeric) or libreoffice.\n"
            "        neither was found on $PATH.\n");
        return 198;
    }
    char tmpdir[64]; snprintf(tmpdir,sizeof tmpdir,"/tmp/tea-xlsx-%d",(int)getpid());
    mkdir(tmpdir,0700);
    char cmd[4096];
    if(have_ss){
        if(sheet && sheet[0]){
            /* sheet names may contain spaces ("All codes"): gnumeric's -O
             * parser splits options on spaces, so the value must be
             * inner-quoted.  Single quotes inside the name can't be
             * expressed this way — error clearly rather than guess. */
            if(strchr(sheet,'\'')){
                tea_err("import: sheet names containing a single quote are not supported\n");
                return 198;
            }
            snprintf(cmd,sizeof cmd,
                "ssconvert -O \"sheet='%s'\" %s '%s' '%s/out.csv' >/dev/null 2>&1",
                sheet,"--export-type=Gnumeric_stf:stf_csv",src,tmpdir);
        } else
            snprintf(cmd,sizeof cmd,"ssconvert %s '%s' '%s/out.csv' >/dev/null 2>&1",
                     "--export-type=Gnumeric_stf:stf_csv",src,tmpdir);
        if(system(cmd)==0){
            /* trust but verify: an unmatched sheet or a quiet failure can
             * exit 0 with no/empty output */
            char probe[600]; snprintf(probe,sizeof probe,"%s/out.csv",tmpdir);
            struct stat st;
            if(stat(probe,&st)==0 && st.st_size>0){
                snprintf(out_path,op_sz,"%s",probe); return 0;
            }
        }
        if(sheet && sheet[0]){
            tea_err("import: ssconvert could not export sheet \"%s\" \u2014 check the name with the workbook open\n", sheet);
            return 603;
        }
        /* whole-file ssconvert failed; fall through to libreoffice */
    }
    if(lo_path){
        if(sheet && sheet[0]){
            /* importing the WRONG sheet silently is a data-correctness
             * hazard; refuse rather than substitute the first sheet */
            tea_err("import: sheet() requires ssconvert (gnumeric); libreoffice cannot select sheets\n");
            return 198;
        }
        snprintf(cmd,sizeof cmd,
            "'%s' --headless --convert-to csv --outdir '%s' '%s' >/dev/null 2>&1",
            lo_path, tmpdir, src);
        if(system(cmd)==0){
            const char *base = strrchr(src,'/'); base = base? base+1 : src;
            char tmp[512]; snprintf(tmp,sizeof tmp,"%s",base);
            char *dot=strrchr(tmp,'.'); if(dot)*dot=0;
            snprintf(out_path,op_sz,"%s/%s.csv",tmpdir,tmp);
            return 0;
        }
    }
    tea_err("import: spreadsheet conversion failed\n");
    return 601;
}

static int do_import(Cmd *c){
    /* ---- import ------------------------------------------------------- *
     *
     * Stata: `import delimited|excel|ods FILE [, clear options]`
     *
     * Reads CSV/TSV (delimited), Excel (.xlsx), or OpenDocument (.ods).
     * Subcommand is the first token after `import`:
     *   delimited    auto-detects ',' vs '\t' by extension or content
     *   excel        requires LibreOffice headless (soffice on $PATH);
     *                option `firstrow` uses row 1 as variable names,
     *                option `sheet("Name")` picks the sheet
     *   ods          ditto, OpenDocument spreadsheet
     *
     * delimited options:
     *   clear              clears any data in memory first (required if
     *                      data already loaded)
     *   delimiter(",")     explicit delimiter (defaults to auto-detect)
     *   varnames(N)        use row N as header (default 1)
     *
     * CSV handling: RFC 4180 quoting is honored — fields containing the
     * delimiter or newlines can be wrapped in double quotes, with "" as
     * an internal quote.  Trailing newlines optional.  Numeric columns
     * are auto-detected; non-numeric values force the column to string.
     * ------------------------------------------------------------------ */
    char fn[1024]=""; const char *s=c->args;
    /* dispatch by subcommand */
    char w1[32]; sscanf(s,"%31s",w1); s+=strlen(w1); while(*s==' ')s++;
    int is_xls = !strcmp(w1,"excel");
    int is_ods = !strcmp(w1,"ods");
    /* Stata: import requires ',clear' if data are in memory */
    bool has_clear = opt_present(c->options,"clear");
    if(c->f->nvar > 0 && !has_clear){
        tea_err("import: no; data in memory would be lost (use ',clear' to discard it)\n");
        return 4;
    }
    if(is_xls || is_ods){
        if(!strncmp(s,"using",5)){s+=5;while(*s==' ')s++;}
        scan_filename(&s, fn, sizeof fn);
        char sheet[128]=""; opt_value(c->options,"sheet",sheet,sizeof sheet);
        /* strip surrounding quotes from sheet */
        if(sheet[0]=='"'){ char *e=strrchr(sheet,'"'); if(e>sheet)*e=0; memmove(sheet,sheet+1,strlen(sheet+1)+1); }
        char tmpcsv[512];
        int rc=convert_spreadsheet(fn,sheet,tmpcsv,sizeof tmpcsv);
        if(rc) return rc;
        /* now load the temp CSV using the same code path as 'import delimited' */
        frame_clear(c->f);
        rc = load_csv_into(c->f, tmpcsv, ',');
        unlink(tmpcsv);
        /* try to remove the temp dir; ignore failure */
        char *slash=strrchr(tmpcsv,'/'); if(slash){ *slash=0; rmdir(tmpcsv); }
        if(rc){ tea_err("import %s: conversion produced no usable CSV\n",w1); return rc; }
        if(!c->quiet) printf("(%d vars, %zu obs)\n",c->f->nvar,c->f->nobs);
        return 0;
    }
    /* default: import delimited (uses the shared load_csv_into) */
    if(!strncmp(s,"using",5)){s+=5;while(*s==' ')s++;}
    scan_filename(&s, fn, sizeof fn);
    char delim=','; char db[8]; if(opt_value(c->options,"delimiters",db,sizeof db))delim=db[0]=='\\'&&db[1]=='t'?'\t':db[0];
    if(strstr(fn,".tsv"))delim='\t';
    frame_clear(c->f);
    int rc2 = load_csv_into(c->f, fn, delim);
    if(rc2){ tea_err("import: cannot read %s (rc=%d)\n", fn, rc2); return rc2; }
    if(!c->quiet) printf("(%d vars, %zu obs)\n",c->f->nvar,c->f->nobs);
    return 0;
}
/* Write a single cell to CSV output, quoting only when needed (RFC 4180):
 * if the cell contains the delimiter, a double-quote, a newline, or
 * carriage return, wrap in double quotes and double any embedded quotes. */
static void csv_quote_write(FILE *fp, const char *s, char delim)
{
    bool need = false;
    for(const char *p = s; *p; p++){
        if(*p == delim || *p == '"' || *p == '\n' || *p == '\r'){ need = true; break; }
    }
    if(!need){ fputs(s, fp); return; }
    fputc('"', fp);
    for(const char *p = s; *p; p++){
        if(*p == '"') fputc('"', fp);   /* "" escaping */
        fputc(*p, fp);
    }
    fputc('"', fp);
}

static int do_export(Cmd *c){
    char fn[1024]=""; const char *s=c->args; char w1[32]; sscanf(s,"%31s",w1); s+=strlen(w1);
    while(*s==' ')s++; if(!strncmp(s,"using",5)){s+=5;while(*s==' ')s++;}
    scan_filename(&s, fn, sizeof fn);
    char delim=','; if(strstr(fn,".tsv"))delim='\t';
    FILE *fp=fopen(fn,"w"); if(!fp){tea_err("export: cannot write %s\n",fn);return 603;}
    /* Header */
    for(int j=0;j<c->f->nvar;j++){
        csv_quote_write(fp, c->f->vars[j].name, delim);
        fputc(j+1<c->f->nvar?delim:'\n', fp);
    }
    /* Rows */
    for(size_t i=0;i<c->f->nobs;i++) for(int j=0;j<c->f->nvar;j++){
        char b[128]; fmt_cell(&c->f->vars[j],i,b,sizeof b);
        csv_quote_write(fp, b, delim);
        fputc(j+1<c->f->nvar?delim:'\n', fp);
    }
    fclose(fp); if(!c->quiet)printf("file %s saved\n",fn); return 0;
}

/* ---- native save / use ------------------------------------------------- */
/* load a .tea (or .csv/.tsv) file into an already-cleared frame. */
static int load_pst_into(Frame *f,const char *fn){
    FILE *fp=fopen(fn,"rb"); if(!fp)return 601;
    char mg[4]; if(fread(mg,1,4,fp)!=4||memcmp(mg,"TEA1",4)){fclose(fp);return 610;}
    size_t nobs; int nvar;
    if(fread(&nobs,sizeof(size_t),1,fp)!=1||fread(&nvar,sizeof(int),1,fp)!=1){fclose(fp);return 610;}
    for(int j=0;j<nvar;j++){ char nm[33];VarType t;char fmt[33],lbl[81];
        if(fread(nm,1,33,fp)!=33||fread(&t,sizeof t,1,fp)!=1||fread(fmt,1,33,fp)!=33||fread(lbl,1,81,fp)!=81){fclose(fp);return 610;}
        Variable*v=var_add(f,nm,t); snprintf(v->format,33,"%s",fmt); snprintf(v->vlabel,81,"%s",lbl); }
    frame_set_nobs(f,nobs);
    for(int j=0;j<nvar;j++){ Variable*v=&f->vars[j];
        if(v->type==VT_NUM){ if(fread(v->num,sizeof(double),nobs,fp)!=nobs){fclose(fp);return 610;} }
        else for(size_t i=0;i<nobs;i++){ int L; if(fread(&L,4,1,fp)!=1){fclose(fp);return 610;} char *b=malloc(L+1); if(L&&fread(b,1,L,fp)!=(size_t)L){free(b);fclose(fp);return 610;} b[L]=0; free(v->str[i]); v->str[i]=b; } }
    fclose(fp); frame_unsort(f);
    return 0;
}
/* csv_unquote: in-place strip surrounding double quotes from a CSV field.
 * Handles "" -> " escaping (CSV standard).  Does nothing if not quoted. */
static void csv_unquote(char *s){
    size_t L = strlen(s);
    if(L < 2 || s[0] != '"' || s[L-1] != '"') return;
    /* shift body left by one, removing leading "; then handle "" -> " in the
     * body and clip the trailing ". */
    size_t r = 1, w = 0;
    while(r < L-1){
        if(s[r] == '"' && s[r+1] == '"'){ s[w++] = '"'; r += 2; }
        else s[w++] = s[r++];
    }
    s[w] = 0;
}

/* sanitize_colname: turn a raw CSV header field into a valid identifier:
 *   - drop leading UTF-8 BOM
 *   - any non-alphanumeric (and non-_) char becomes _
 *   - collapse consecutive _ to a single _
 *   - strip leading _ (Stata identifiers can't start with _ either)
 *   - if it would start with a digit, prepend 'v'
 *   - if empty, use 'v' */
static void sanitize_colname(const char *in, char *out, size_t outsz){
    const char *p = in;
    /* skip BOM */
    if((unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p+=3;
    char buf[256]; size_t w=0;
    int last_underscore = 1;   /* treat start as "after underscore" so leading _ is dropped */
    for(; *p && w<sizeof(buf)-1; p++){
        char c = *p;
        if(isalnum((unsigned char)c) || c=='_'){
            if(c=='_'){ if(last_underscore) continue; buf[w++]='_'; last_underscore=1; }
            else { buf[w++]=c; last_underscore=0; }
        } else {
            /* non-identifier char becomes underscore; collapsed */
            if(!last_underscore){ buf[w++]='_'; last_underscore=1; }
        }
    }
    /* trim trailing underscore */
    while(w>0 && buf[w-1]=='_') w--;
    buf[w] = 0;
    if(!buf[0]){ snprintf(out, outsz, "v"); return; }
    if(isdigit((unsigned char)buf[0])) snprintf(out, outsz, "v%s", buf);
    else snprintf(out, outsz, "%s", buf);
}

static int load_csv_into(Frame *f,const char *fn,char delim){
    FILE *fp=fopen(fn,"r"); if(!fp)return 601;
    /* Buffer for one logical record (may span multiple physical lines if a
     * quoted field contains embedded newlines). */
    size_t bufcap = 1<<16;
    char *line = malloc(bufcap);
    char chunk[1<<16];
    int row=0, ncol=0;
    int rc = 0;
    while(1){
        /* Read until quotes balance — that completes a logical CSV record. */
        size_t len = 0; int inq = 0;
        line[0] = 0;
        int got_anything = 0;
        while(fgets(chunk, sizeof chunk, fp)){
            got_anything = 1;
            size_t clen = strlen(chunk);
            /* grow line buffer if needed */
            while(len + clen + 1 > bufcap){
                bufcap *= 2;
                line = realloc(line, bufcap);
            }
            memcpy(line + len, chunk, clen + 1);
            len += clen;
            /* update quote state across the newly appended chunk */
            for(size_t i = len - clen; i < len; i++){
                if(line[i] == '"') inq = !inq;
            }
            if(!inq) break;     /* record complete */
            /* else: quoted field continues on next physical line; keep reading */
        }
        if(!got_anything) break;
        /* strip trailing CR/LF from the assembled record (but only at the very end —
         * newlines inside quotes must stay as part of the field value) */
        while(len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')){
            line[--len] = 0;
        }
        /* split, respecting quotes: don't break on delim inside "..." */
        char *flds[512]; int nf=0; char *p=line; inq=0;
        flds[nf++]=p;
        for(char *d=p; *d; d++){
            if(*d=='"') inq=!inq;
            else if(*d==delim && !inq){ *d=0; if(nf<512) flds[nf++]=d+1; }
        }
        if(row==0){
            ncol=nf;
            for(int j=0;j<nf;j++){
                csv_unquote(flds[j]);
                char nm[64]; sanitize_colname(flds[j], nm, sizeof nm);
                /* ensure unique: if collision, append _2, _3, ... */
                char unique[64]; snprintf(unique, sizeof unique, "%s", nm);
                int suffix = 2;
                while(var_find(f, unique) >= 0){
                    snprintf(unique, sizeof unique, "%s_%d", nm, suffix++);
                }
                var_add(f, unique, VT_NUM);
            }
            row++; continue;
        }
        frame_set_nobs(f,(size_t)row);
        for(int j=0;j<ncol&&j<nf;j++){
            csv_unquote(flds[j]);
            Variable *v=&f->vars[j]; char *e; double x=strtod(flds[j],&e);
            int empty=(flds[j][0]==0);
            if(empty){ if(v->type==VT_STR)str_set(v,row-1,""); else v->num[row-1]=SV_MISS; }
            else if(*e==0){ if(v->type==VT_STR)str_set(v,row-1,flds[j]); else v->num[row-1]=x; }
            else { if(v->type==VT_NUM){ char **ns=malloc((size_t)row*sizeof(char*));
                    for(int r=0;r<row-1;r++){ char tb[64]; if(sv_is_miss(v->num[r]))tb[0]=0; else snprintf(tb,64,"%g",v->num[r]); ns[r]=strdup(sv_is_miss(v->num[r])?"":tb);}
                    ns[row-1]=strdup(""); free(v->num);v->num=NULL;v->str=ns;v->type=VT_STR;
                    v->cap=(size_t)row;  /* swapped storage: capacity is the new alloc */ }
                str_set(v,row-1,flds[j]); } }
        row++;
    }
    free(line);
    fclose(fp); frame_unsort(f); return rc;
}
/* Case-insensitive end-of-string match.  Used for file extension dispatch
 * so 'foo.DTA' and 'foo.dta' route the same way. */
static bool ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return false;
    return strcasecmp(s + ls - lf, suf) == 0;
}

/* Dispatch a load by file extension.
 *   err_out — optional; if non-NULL and the route returns a rich error
 *             message, it lands here so the caller can print a single
 *             clear "use: ..." or "merge: ..." line.  Set to NULL on
 *             success, or for routes that don't yet supply one. */
static int load_into(Frame *f, Workspace *ws, const char *fn, const char **err_out){
    const char *sink = NULL;
    const char **err = err_out ? err_out : &sink;
    *err = NULL;
    if(ends_with_ci(fn, ".dta")){
        return dta_read(f, ws, fn, err);
    }
    if(strstr(fn,".csv")) return load_csv_into(f,fn,',');
    if(strstr(fn,".tsv")) return load_csv_into(f,fn,'\t');
    return load_pst_into(f,fn);
}

/* ---- merge ------------------------------------------------------------- */
static void keystr(Frame *f,int *k,int nk,size_t r,char *out,size_t n){
    size_t o=0; out[0]=0;
    for(int j=0;j<nk;j++){ Variable *v=&f->vars[k[j]]; char b[64];
        if(v->type==VT_NUM){ double x=v->num[r];
            if(sv_is_miss(x)) snprintf(b,sizeof b,"#m%d",sv_miss_code(x));
            else snprintf(b,sizeof b,"%.17g",x); }
        else snprintf(b,sizeof b,"%s",v->str[r]);
        int w=snprintf(out+o,n-o,"%s\x1f",b); if(w>0)o+=(size_t)w; }
}
typedef struct KMap { char **keys; long *row; size_t cap,used; } KMap;
static unsigned long kh(const char*s){ unsigned long h=1469598103934665603UL; for(;*s;s++){h^=(unsigned char)*s;h*=1099511628211UL;} return h; }
static void kmap_init(KMap*m,size_t hint){ m->cap=16; while(m->cap<hint*2+16)m->cap<<=1; m->keys=calloc(m->cap,sizeof(char*)); m->row=malloc(m->cap*sizeof(long)); m->used=0; }
static int  kmap_put(KMap*m,const char*k,long r){ size_t h=kh(k)&(m->cap-1); int dup=0;
    while(m->keys[h]){ if(!strcmp(m->keys[h],k)){ m->row[h]=r; return 1; } h=(h+1)&(m->cap-1); }
    m->keys[h]=strdup(k); m->row[h]=r; m->used++; return dup; }
static long kmap_get(KMap*m,const char*k){ size_t h=kh(k)&(m->cap-1);
    while(m->keys[h]){ if(!strcmp(m->keys[h],k))return m->row[h]; h=(h+1)&(m->cap-1);} return -1; }
static void kmap_free(KMap*m){ for(size_t i=0;i<m->cap;i++)free(m->keys[i]); free(m->keys); free(m->row); }

static int do_merge(Cmd *c){
    /* ---- merge -------------------------------------------------------- *
     *
     * Stata: `merge 1:1 | m:1 | 1:m keyvars using FILE [, options]`
     *
     * Joins the in-memory dataset (the "master") with a dataset on disk
     * (the "using" file), matching rows by keyvars.  After the merge, a
     * new variable `_merge` is created:
     *   1 = observation only in master
     *   2 = observation only in using
     *   3 = matched
     *
     * Multiplicity:
     *   1:1 — both sides have unique key combinations
     *   m:1 — master has duplicates, using is unique
     *   1:m — master is unique, using has duplicates
     *   m:m — DELIBERATELY REJECTED with an explanatory error.  m:m
     *         is almost always either a confusion with joinby (which
     *         is a cross-product) or a sign that the keys aren't
     *         actually identifying the relationship the user thinks.
     *
     * Options:
     *   nogenerate       suppress the _merge variable
     *   generate(name)   use a different name than _merge
     *   keep(...)        restrict result to a subset of merge codes
     *                    accepts master|match|using|1|2|3 keywords
     *   assert(...)      verify all observations fall into the given
     *                    merge codes; abort if not (sanity check)
     *   keepusing(vl)    only bring in selected columns from using
     *   update           bring in non-missing using values when master
     *                    has missing; option `replace` overrides even
     *                    non-missing master values
     *
     * The using file may be a .tea, .dta, or .csv/.tsv.
     *
     * Implementation: builds a hash map of (concat-of-key-columns) →
     * row-id from the using file, then iterates master rows and looks
     * each up.  Multiplicity check happens during the hash build (1:1
     * and m:1 require unique keys on the using side).
     * ------------------------------------------------------------------ */
    /* syntax: merge n1:n2 keyvars using FILE [, options]   (args, post word) */
    const char *s=c->args; while(*s==' ')s++;
    char spec[16]=""; sscanf(s,"%15[^ ]",spec); s+=strlen(spec); while(*s==' ')s++;
    if(strchr(spec,':')==NULL){ tea_err("merge: spec must be 1:1, m:1, or 1:m\n"); return 198; }
    if(!strcmp(spec,"m:m")){ tea_err("merge: m:m is not supported (deliberately — it is a footgun; reshape or restructure instead)\n"); return 198; }
    int mside = (spec[0]=='m'); int uside = (spec[2]=='m');
    if(mside&&uside){ tea_err("merge: m:m is not supported\n"); return 198; }
    /* key varlist up to 'using' */
    const char *up=strstr(s," using "); const char *up2=strstr(s,"using ");
    const char *u = up? up+7 : (up2==s? s+6 : NULL);
    if(!u){ tea_err("merge: 'using' required\n"); return 198; }
    char kspec[1024];
    if(up) snprintf(kspec,sizeof kspec,"%.*s",(int)(up-s),s);
    else   snprintf(kspec,sizeof kspec,"%.*s",(int)(up2-s),s);
    char fn[1024]=""; const char *up_local = u; scan_filename(&up_local, fn, sizeof fn);
    if(!strstr(fn,"."))strcat(fn,".tea");

    int *km=NULL,nk=varlist_expand(c->f,kspec,&km);
    if(nk<1){ tea_err("merge: key variables not found\n"); free(km); return 111; }

    Frame *U=frame_create(c->ws,"__merge_using");
    const char *load_err = NULL;
    int rc=load_into(U,c->ws,fn,&load_err);
    if(rc){
        if(load_err) tea_err("merge: %s\n", load_err);
        else         tea_err("merge: cannot read %s (rc=%d)\n",fn,rc);
        for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)if(*pp==U){*pp=U->next;break;} free(U);free(km);return rc; }

    /* map master keys to using key indices by name */
    int ku[64]; for(int j=0;j<nk;j++){ ku[j]=var_find(U,c->f->vars[km[j]].name);
        if(ku[j]<0){ tea_err("merge: key %s not in using\n",c->f->vars[km[j]].name);
            for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)if(*pp==U){*pp=U->next;break;} frame_clear(U);free(U);free(km);return 111; } }

    /* keepusing() restricts which using vars are pulled in */
    char kuopt[512]; int *kuv=NULL,nkuv=-1;
    if(opt_value(c->options,"keepusing",kuopt,sizeof kuopt)) nkuv=varlist_expand(U,kuopt,&kuv);

    /* classify using vars: key / common-with-master / using-only */
    int uonly[512],nuo=0, ucommon[512],mcommon[512],ncom=0;
    for(int j=0;j<U->nvar;j++){
        int isk=0; for(int q=0;q<nk;q++)if(ku[q]==j)isk=1; if(isk)continue;
        if(nkuv>=0){ int inkeep=0; for(int q=0;q<nkuv;q++)if(kuv[q]==j)inkeep=1; if(!inkeep)continue; }
        int mi=var_find(c->f,U->vars[j].name);
        if(mi>=0){ ucommon[ncom]=j; mcommon[ncom]=mi; ncom++; }
        else uonly[nuo++]=j;
    }
    int do_update=opt_present(c->options,"update");
    int do_repl  =opt_present(c->options,"replace");

    /* build the lookup map on the unique side */
    int iter_master = !uside;            /* 1:1,m:1 iterate master; 1:m iterate using */
    KMap map; char kb[2048];
    if(iter_master){ kmap_init(&map,U->nobs);
        for(size_t r=0;r<U->nobs;r++){ keystr(U,ku,nk,r,kb,sizeof kb); kmap_put(&map,kb,(long)r); } }
    else { kmap_init(&map,c->f->nobs);
        for(size_t r=0;r<c->f->nobs;r++){ keystr(c->f,km,nk,r,kb,sizeof kb); kmap_put(&map,kb,(long)r); } }

    /* result frame: master columns + using-only columns + _merge */
    Frame *R=frame_create(c->ws,"__merge_res");
    for(int j=0;j<c->f->nvar;j++){ Variable*sv=&c->f->vars[j]; Variable*d=var_add(R,sv->name,sv->type); snprintf(d->format,33,"%s",sv->format); }
    for(int j=0;j<nuo;j++){ Variable*sv=&U->vars[uonly[j]]; Variable*d=var_add(R,sv->name,sv->type); snprintf(d->format,33,"%s",sv->format); }
    char mgname[33]="_merge"; { char g[33]; if(opt_value(c->options,"generate",g,sizeof g)) snprintf(mgname,33,"%s",g); }
    int wantmg = !opt_present(c->options,"nogenerate") && !opt_present(c->options,"nogen");
    var_add(R,mgname,VT_NUM); int mgvar=R->nvar-1;   /* always built; dropped at end if nogen */

    char *useu = U->nobs? calloc(U->nobs,1):NULL;
    char *usem = c->f->nobs? calloc(c->f->nobs,1):NULL;
    size_t outn=0;

    #define GROW() do{ frame_set_nobs(R,outn+1); }while(0)
    #define COPYMASTER(rr) do{ for(int j=0;j<c->f->nvar;j++){ Variable*S=&c->f->vars[j],*D=&R->vars[j]; \
        if(S->type==VT_NUM)D->num[outn]=S->num[rr]; else str_set(D,outn,S->str[rr]); } }while(0)
    #define MKEYFROMU(ur) do{ for(int q=0;q<nk;q++){ Variable*S=&U->vars[ku[q]],*D=&R->vars[km[q]]; \
        if(S->type==VT_NUM)D->num[outn]=S->num[ur]; else str_set(D,outn,S->str[ur]); } }while(0)
    #define COPYUONLY(ur) do{ for(int j=0;j<nuo;j++){ Variable*S=&U->vars[uonly[j]]; Variable*D=&R->vars[c->f->nvar+j]; \
        if(S->type==VT_NUM)D->num[outn]=S->num[ur]; else str_set(D,outn,S->str[ur]); } }while(0)
    #define APPLYUPDATE(ur) do{ if(do_update) for(int q=0;q<ncom;q++){ Variable*S=&U->vars[ucommon[q]],*D=&R->vars[mcommon[q]]; \
        if(D->type==VT_NUM){ if(do_repl){ if(!sv_is_miss(S->num[ur]))D->num[outn]=S->num[ur]; } \
            else if(sv_is_miss(D->num[outn])) D->num[outn]=S->num[ur]; } \
        else { if(do_repl){ if(S->str[ur][0])str_set(D,outn,S->str[ur]); } \
            else if(R->vars[mcommon[q]].str[outn][0]==0) str_set(D,outn,S->str[ur]); } } }while(0)

    if(iter_master){
        for(size_t i=0;i<c->f->nobs;i++){
            keystr(c->f,km,nk,i,kb,sizeof kb); long ur=kmap_get(&map,kb);
            GROW(); COPYMASTER(i);
            if(ur>=0){ useu[ur]=1; COPYUONLY((size_t)ur); APPLYUPDATE((size_t)ur);
                if(mgvar>=0)R->vars[mgvar].num[outn]=3; }
            else { if(mgvar>=0)R->vars[mgvar].num[outn]=1; }
            outn++;
        }
        for(size_t r=0;r<U->nobs;r++) if(!useu[r]){
            GROW(); MKEYFROMU(r); COPYUONLY(r);
            if(mgvar>=0)R->vars[mgvar].num[outn]=2; outn++;
        }
    } else {  /* 1:m : iterate using, map on master */
        for(size_t r=0;r<U->nobs;r++){
            keystr(U,ku,nk,r,kb,sizeof kb); long mr=kmap_get(&map,kb);
            GROW();
            if(mr>=0){ usem[mr]=1; COPYMASTER((size_t)mr); COPYUONLY(r); APPLYUPDATE(r);
                if(mgvar>=0)R->vars[mgvar].num[outn]=3; }
            else { MKEYFROMU(r); COPYUONLY(r); if(mgvar>=0)R->vars[mgvar].num[outn]=2; }
            outn++;
        }
        for(size_t i=0;i<c->f->nobs;i++) if(!usem[i]){
            GROW(); COPYMASTER(i); if(mgvar>=0)R->vars[mgvar].num[outn]=1; outn++;
        }
    }
    free(useu);free(usem); kmap_free(&map);

    /* keep(...) filter on _merge */
    char keepopt[128];
    if(mgvar>=0 && opt_value(c->options,"keep",keepopt,sizeof keepopt)){
        int allow[4]={0,0,0,0};
        if(strstr(keepopt,"master")||strstr(keepopt,"1"))allow[1]=1;
        if(strstr(keepopt,"using") ||strstr(keepopt,"2"))allow[2]=1;
        if(strstr(keepopt,"match") ||strstr(keepopt,"3"))allow[3]=1;
        size_t w=0;
        for(size_t i=0;i<outn;i++){ int m=(int)R->vars[mgvar].num[i];
            if(allow[m]){ if(w!=i)for(int j=0;j<R->nvar;j++){Variable*V=&R->vars[j];
                if(V->type==VT_NUM)V->num[w]=V->num[i]; else {free(V->str[w]);V->str[w]=V->str[i];V->str[i]=NULL;}}
                w++; } }
        for(size_t i=w;i<outn;i++)for(int j=0;j<R->nvar;j++){Variable*V=&R->vars[j];if(V->type==VT_STR){free(V->str[i]);V->str[i]=NULL;}}
        outn=w; R->nobs=w;
    }
    /* assert(...) */
    char asopt[128];
    if(mgvar>=0 && opt_value(c->options,"assert",asopt,sizeof asopt)){
        int allow[4]={0,0,0,0};
        if(strstr(asopt,"master")||strstr(asopt,"1"))allow[1]=1;
        if(strstr(asopt,"using") ||strstr(asopt,"2"))allow[2]=1;
        if(strstr(asopt,"match") ||strstr(asopt,"3"))allow[3]=1;
        for(size_t i=0;i<outn;i++){ int m=(int)R->vars[mgvar].num[i];
            if(!allow[m]){ tea_err("merge: assertion failed (found _merge==%d)\n",m);
                /* still swap result in, like Stata leaves data; return error */ break; } }
    }

    long n1=0,n2=0,n3=0;
    if(mgvar>=0) for(size_t i=0;i<outn;i++){ int m=(int)R->vars[mgvar].num[i]; if(m==1)n1++;else if(m==2)n2++;else n3++; }

    if(!wantmg && mgvar>=0){   /* nogenerate: discard the _merge column */
        Variable *mv=&R->vars[mgvar];
        if(mv->type==VT_STR){ for(size_t i=0;i<R->nobs;i++)free(mv->str[i]); free(mv->str); } else free(mv->num);
        R->nvar--;             /* it was the last column */
        mgvar=-1;
    }

    /* swap result into active frame; drop temps */
    frame_clear(c->f);
    c->f->vars=R->vars; c->f->nvar=R->nvar; c->f->cap_var=R->cap_var; c->f->nobs=R->nobs;
    R->vars=NULL;R->nvar=R->cap_var=0;R->nobs=0;
    for(Frame**pp=&c->ws->frames;*pp;){ if(*pp==R||*pp==U){ Frame*g=*pp; *pp=g->next; if(g==U)frame_clear(g); free(g->sortvars); free(g);} else pp=&(*pp)->next; }
    frame_unsort(c->f);
    free(km); free(kuv);
    if(!c->quiet){
        printf("\n    Result                      Number of obs\n");
        printf("    -----------------------------------------\n");
        printf("    Not matched%30ld\n",n1+n2);
        printf("        from master (_merge==1)%14ld\n",n1);
        printf("        from using  (_merge==2)%14ld\n",n2);
        printf("    Matched     (_merge==3)%18ld\n",n3);
        printf("    -----------------------------------------\n");
    }
    return 0;
    #undef GROW
    #undef COPYMASTER
    #undef MKEYFROMU
    #undef COPYUONLY
    #undef APPLYUPDATE
}

/* ---- reshape (in-place; user must `frame copy` first if they want a copy) */
static int do_reshape(Cmd *c){
    const char *s=c->args; while(*s==' ')s++;
    char dir[8]=""; sscanf(s,"%7s",dir); s+=strlen(dir); while(*s==' ')s++;
    int islong = !strcmp(dir,"long");
    if(!islong && strcmp(dir,"wide")){ tea_err("reshape: specify long or wide\n"); return 198; }
    /* stubs are everything up to options; i()/j() are options */
    char head[1024]; const char *cm=strchr(s,',');
    snprintf(head,sizeof head,"%.*s",(int)(cm?cm-s:(long)strlen(s)),s);
    /* Trim trailing whitespace from head so we can tell empty-stubs from
     * stub-list-with-trailing-space. */
    for(int q=(int)strlen(head)-1; q>=0 && (head[q]==' '||head[q]=='\t'); q--) head[q]=0;

    char iopt[512]="",jopt[64]="";
    opt_value(c->options,"i",iopt,sizeof iopt);
    opt_value(c->options,"j",jopt,sizeof jopt);

    /* Diagnostic for the most common mistakes: no stubs, no options, or
     * options written before the comma (as if Stata took options inline). */
    if(!head[0] && !iopt[0] && !jopt[0]){
        tea_err("reshape: stub list and options missing.  Syntax is:\n"
                "          reshape long  stub [stub ...] , i(idvars) j(jvar)\n"
                "          reshape wide  stub [stub ...] , i(idvars) j(jvar)\n"
                "        Example:  reshape long v, i(country_id) j(year)\n");
        return 198;
    }
    if(!head[0]){
        tea_err("reshape: no stub list before the comma.\n"
                "        Syntax: reshape %s  stub [stub ...] , i(...) j(...)\n"
                "        (the stub is the prefix of variables like v1980, v1981, ... — pass just 'v')\n",
                dir);
        return 198;
    }
    if(!iopt[0] && !jopt[0]){
        /* Common cause: user put i()/j() before the comma, where they look
         * like stubs.  Detect that and say so. */
        if(strstr(head,"i(") || strstr(head,"j(")){
            tea_err("reshape: i() and j() are options — they go AFTER the comma.\n"
                    "        Got:    reshape %s %s\n"
                    "        Try:    reshape %s STUB, i(...) j(...)\n", dir, head, dir);
        } else {
            tea_err("reshape: i() and j() required.\n"
                    "        Try:  reshape %s %s, i(idvars) j(jvar)\n", dir, head);
        }
        return 198;
    }
    if(!iopt[0]){ tea_err("reshape: i() option required (the id variable(s) per row in long form)\n"); return 198; }
    if(!jopt[0]){ tea_err("reshape: j() option required (the variable holding the level, e.g. 'year')\n"); return 198; }

    char jname[64]; sscanf(jopt,"%63s",jname);
    /* stub names: tokens in head.  In Stata, 'reshape long stub' takes the
     * literal stub, not a wildcard pattern.  Resolve wildcards by stripping
     * trailing '*' or '?' from each token (treat the prefix as the stub). */
    char stubs[32][33]; int ns=0;
    { char hb[1024]; snprintf(hb,sizeof hb,"%s",head);
        char *sv=NULL;
        for(char *t=strtok_r(hb," \t",&sv); t && ns<32; t=strtok_r(NULL," \t",&sv)){
            /* strip trailing wildcards to recover the stub */
            size_t L=strlen(t);
            while(L && (t[L-1]=='*'||t[L-1]=='?')) t[--L]=0;
            if(L) snprintf(stubs[ns++],33,"%s",t);
        }
    }
    if(ns==0){ tea_err("reshape: at least one stub required\n"); return 198; }
    int *iv=NULL,niv=varlist_expand(c->f,iopt,&iv);
    if(niv<1){ tea_err("reshape: i() vars not found\n"); free(iv); return 111; }

    /* result built in a separate frame, then swapped into the current frame. */
    Frame *Rsrc = c->f;
    Frame *R=frame_create(c->ws,"__reshape_res");

    if(islong){
        /* discover j levels from columns stub<level> */
        double levels[512]; int nl=0;
        for(int j=0;j<Rsrc->nvar;j++){ const char *nm=Rsrc->vars[j].name;
            for(int sIdx=0;sIdx<ns;sIdx++){ size_t L=strlen(stubs[sIdx]);
                if(!strncmp(nm,stubs[sIdx],L)&&nm[L]){ char *e; double lv=strtod(nm+L,&e);
                    if(*e==0){ int seen=0; for(int q=0;q<nl;q++)if(levels[q]==lv)seen=1; if(!seen&&nl<512)levels[nl++]=lv; } } } }
        if(nl==0){ tea_err("reshape long: no variables matching stub(s) found\n");
            /* drop the temp frame */
            for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)
                if(*pp==R){ *pp=R->next; frame_clear(R); free(R->sortvars); free(R); break; }
            free(iv); return 111; }
        for(int a=0;a<nl;a++)for(int b=a+1;b<nl;b++)if(levels[a]>levels[b]){double t=levels[a];levels[a]=levels[b];levels[b]=t;}

        /* Validate: per stub, all the stub<level> variables must have the SAME
         * type — otherwise we can't fit them in one column.  Stata errors here
         * with "type mismatch in long format".  Also error if any i() var is
         * also matched by a stub (e.g. user passed `i(z*)` with stub `z`). */
        for(int sIdx=0;sIdx<ns;sIdx++){
            /* Collect matched variables per type so we can give a useful
             * error if types disagree.  Two-pass keeps the error message
             * concrete: name both groups and suggest the fix. */
            char numvars[32][64]; int n_num=0;
            char strvars[32][64]; int n_str=0;
            for(int a=0;a<nl;a++){
                char probe[64]; snprintf(probe,sizeof probe,"%s%g",stubs[sIdx],levels[a]);
                int pv=var_find(Rsrc,probe); if(pv<0) continue;
                if(Rsrc->vars[pv].type==VT_NUM && n_num<32) snprintf(numvars[n_num++],64,"%s",probe);
                else if(Rsrc->vars[pv].type==VT_STR && n_str<32) snprintf(strvars[n_str++],64,"%s",probe);
                /* check overlap with i() in same loop */
                for(int q=0;q<niv;q++){
                    if(pv == iv[q]){
                        tea_err("reshape long: variable %s appears in both i() and the stub list\n", probe);
                        for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)
                            if(*pp==R){ *pp=R->next; frame_clear(R); free(R->sortvars); free(R); break; }
                        free(iv); return 198;
                    }
                }
            }
            if(n_num > 0 && n_str > 0){
                /* concatenate each list into a comma-separated string */
                char num_list[512]={0}, str_list[512]={0};
                for(int q=0;q<n_num;q++){
                    if(q) strncat(num_list, " ", sizeof num_list-strlen(num_list)-1);
                    strncat(num_list, numvars[q], sizeof num_list-strlen(num_list)-1);
                }
                for(int q=0;q<n_str;q++){
                    if(q) strncat(str_list, " ", sizeof str_list-strlen(str_list)-1);
                    strncat(str_list, strvars[q], sizeof str_list-strlen(str_list)-1);
                }
                tea_err(
                  "reshape long: stub '%s' matches variables of different types:\n"
                  "    numeric: %s\n"
                  "    string:  %s\n"
                  "  All variables collapsing into a single stub must have the same type.\n"
                  "  Drop the variables you don't want before reshape, e.g.:\n"
                  "      drop %s\n"
                  "      reshape long %s, i(...) j(...)\n",
                  stubs[sIdx], num_list, str_list,
                  /* suggest dropping the smaller group; reshape the rest */
                  n_str <= n_num ? str_list : num_list,
                  stubs[sIdx]);
                for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)
                    if(*pp==R){ *pp=R->next; frame_clear(R); free(R->sortvars); free(R); break; }
                free(iv); return 109;
            }
        }

        /* result columns: i vars, j var, one per stub (type from first found col) */
        for(int q=0;q<niv;q++){ Variable*sv=&Rsrc->vars[iv[q]]; var_add(R,sv->name,sv->type); }
        var_add(R,jname,VT_NUM);
        int stubcol[32];
        for(int sIdx=0;sIdx<ns;sIdx++){ VarType t=VT_NUM; char probe[64];
            for(int a=0;a<nl;a++){ snprintf(probe,sizeof probe,"%s%g",stubs[sIdx],levels[a]); int pv=var_find(Rsrc,probe); if(pv>=0){t=Rsrc->vars[pv].type;break;} }
            var_add(R,stubs[sIdx],t); stubcol[sIdx]=R->nvar-1; }
        frame_set_nobs(R,Rsrc->nobs*(size_t)nl);
        size_t on=0;
        for(size_t r=0;r<Rsrc->nobs;r++) for(int a=0;a<nl;a++){
            for(int q=0;q<niv;q++){ Variable*S=&Rsrc->vars[iv[q]],*D=&R->vars[q];
                if(S->type==VT_NUM)D->num[on]=S->num[r]; else str_set(D,on,S->str[r]); }
            R->vars[niv].num[on]=levels[a];
            for(int sIdx=0;sIdx<ns;sIdx++){ char nm[64]; snprintf(nm,sizeof nm,"%s%g",stubs[sIdx],levels[a]);
                int pv=var_find(Rsrc,nm); Variable*D=&R->vars[stubcol[sIdx]];
                if(pv<0){ if(D->type==VT_NUM)D->num[on]=SV_MISS; else str_set(D,on,""); }
                else { Variable*S=&Rsrc->vars[pv]; if(S->type==VT_NUM)D->num[on]=S->num[r]; else str_set(D,on,S->str[r]); } }
            on++;
        }
    } else {
        /* reshape wide: group by i vars; spread stub over distinct j values */
        frame_physical_sort(Rsrc,iv,niv,NULL);
        size_t *lo=NULL,*hi=NULL; int ng=by_groups(Rsrc,iv,niv,&lo,&hi);
        int jv=var_find(Rsrc,jname); if(jv<0){ tea_err("reshape: j var %s not found\n",jname);
            for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)
                if(*pp==R){ *pp=R->next; frame_clear(R); free(R->sortvars); free(R); break; }
            free(iv);free(lo);free(hi); return 111; }
        /* distinct j levels (numeric) */
        double levels[512]; int nl=0;
        for(size_t r=0;r<Rsrc->nobs;r++){ double lv=Rsrc->vars[jv].num[r]; int seen=0;
            for(int q=0;q<nl;q++)if(levels[q]==lv)seen=1; if(!seen&&nl<512)levels[nl++]=lv; }
        for(int a=0;a<nl;a++)for(int b=a+1;b<nl;b++)if(levels[a]>levels[b]){double t=levels[a];levels[a]=levels[b];levels[b]=t;}
        for(int q=0;q<niv;q++){ Variable*sv=&Rsrc->vars[iv[q]]; var_add(R,sv->name,sv->type); }
        int base[32];
        for(int sIdx=0;sIdx<ns;sIdx++){ int src=var_find(Rsrc,stubs[sIdx]); VarType t=src>=0?Rsrc->vars[src].type:VT_NUM;
            base[sIdx]=R->nvar;
            for(int a=0;a<nl;a++){ char nm[64]; snprintf(nm,sizeof nm,"%s%g",stubs[sIdx],levels[a]); var_add(R,nm,t); } }
        frame_set_nobs(R,(size_t)ng);
        for(int g=0;g<ng;g++){
            for(int q=0;q<niv;q++){ Variable*S=&Rsrc->vars[iv[q]],*D=&R->vars[q];
                if(S->type==VT_NUM)D->num[g]=S->num[lo[g]]; else str_set(D,g,S->str[lo[g]]); }
            for(int sIdx=0;sIdx<ns;sIdx++){ int src=var_find(Rsrc,stubs[sIdx]); if(src<0)continue;
                Variable*S=&Rsrc->vars[src];
                for(size_t r=lo[g];r<=hi[g];r++){ double lv=Rsrc->vars[jv].num[r]; int idx=-1;
                    for(int a=0;a<nl;a++)if(levels[a]==lv){idx=a;break;} if(idx<0)continue;
                    Variable*D=&R->vars[base[sIdx]+idx];
                    if(S->type==VT_NUM)D->num[g]=S->num[r]; else str_set(D,g,S->str[r]); }
            }
        }
        free(lo); free(hi);
    }

    /* swap R's contents into the active frame, then drop R from the workspace. */
    frame_clear(c->f);
    c->f->vars=R->vars; c->f->nvar=R->nvar; c->f->cap_var=R->cap_var; c->f->nobs=R->nobs;
    R->vars=NULL; R->nvar=R->cap_var=0; R->nobs=0;
    for(Frame**pp=&c->ws->frames;*pp;pp=&(*pp)->next)
        if(*pp==R){ *pp=R->next; free(R->sortvars); free(R); break; }
    frame_unsort(c->f);

    if(!c->quiet) printf("(reshaped %s; now %d vars, %zu obs)\n",
                         islong?"long":"wide", c->f->nvar, c->f->nobs);
    free(iv);
    return 0;
}

/* ---- recode ------------------------------------------------------------ */
typedef struct { int kind; double a,b,to; int has_to; } RRule;
/* kind: 0=value/range, 1=missing, 2=nonmissing, 3=else */
/* ---- encode / decode --------------------------------------------------- */
/* encode oldstrvar, generate(newvar) [label(lblname)]
 *   Map a string variable's distinct values to integers 1..K, attach a value
 *   label so list/describe show the original strings.  Missing/empty strings
 *   map to system missing.  Order: alphabetical.  Matches Stata's behavior. */
static int do_encode(Cmd *c){
    char src[64]=""; sscanf(c->varlist,"%63s",src);
    if(!src[0]){ tea_err("encode: source variable required\n"); return 198; }
    char newvar[64]=""; opt_value(c->options,"generate",newvar,sizeof newvar);
    if(!newvar[0]) opt_value(c->options,"gen",newvar,sizeof newvar);
    if(!newvar[0]){ tea_err("encode: generate(newvar) required\n"); return 198; }
    char lbl[64]=""; opt_value(c->options,"label",lbl,sizeof lbl);
    if(!lbl[0]) snprintf(lbl, sizeof lbl, "%s", newvar);

    int si = var_find(c->f, src);
    if(si < 0){ tea_err("encode: %s not found\n",src); return 111; }
    Variable *sv = &c->f->vars[si];
    if(sv->type != VT_STR){ tea_err("encode: %s must be string\n",src); return 109; }
    if(var_find(c->f, newvar) >= 0){ tea_err("encode: %s already exists\n",newvar); return 110; }

    /* collect distinct non-empty values */
    char **uniq = NULL; int K = 0;
    for(size_t i=0;i<c->f->nobs;i++){
        const char *s = sv->str[i]; if(!s || !s[0]) continue;
        int found = 0;
        for(int k=0;k<K;k++) if(!strcmp(uniq[k], s)){ found=1; break; }
        if(!found){ uniq = realloc(uniq, (K+1)*sizeof(char*)); uniq[K++] = strdup(s); }
    }
    /* sort uniq ascending so encoding is deterministic */
    for(int i=0;i<K;i++) for(int j=i+1;j<K;j++)
        if(strcmp(uniq[i], uniq[j]) > 0){ char *t=uniq[i]; uniq[i]=uniq[j]; uniq[j]=t; }

    /* create the new numeric variable */
    Variable *nv = var_add(c->f, newvar, VT_NUM);
    snprintf(nv->vlabel, sizeof nv->vlabel, "%s", lbl);

    /* register value label set */
    VLabel *L = vlabel_ensure(c->ws, lbl);
    for(int k=0;k<K;k++) vlabel_put(L, (double)(k+1), uniq[k]);

    /* fill the new variable */
    long mapped = 0;
    for(size_t i=0;i<c->f->nobs;i++){
        const char *s = sv->str[i];
        if(!s || !s[0]){ nv->num[i] = SV_MISS; continue; }
        int code = -1;
        for(int k=0;k<K;k++) if(!strcmp(uniq[k], s)){ code = k+1; break; }
        nv->num[i] = code>0 ? (double)code : SV_MISS;
        if(code>0) mapped++;
    }
    if(!c->quiet) printf("(%d distinct values encoded; %ld obs mapped)\n", K, mapped);

    for(int k=0;k<K;k++) free(uniq[k]); free(uniq);
    return 0;
}

/* decode encvar, generate(newstrvar)
 *   Inverse of encode: numeric+vlabel -> string variable. */
static int do_decode(Cmd *c){
    char src[64]=""; sscanf(c->varlist,"%63s",src);
    if(!src[0]){ tea_err("decode: source variable required\n"); return 198; }
    char newvar[64]=""; opt_value(c->options,"generate",newvar,sizeof newvar);
    if(!newvar[0]) opt_value(c->options,"gen",newvar,sizeof newvar);
    if(!newvar[0]){ tea_err("decode: generate(newvar) required\n"); return 198; }

    int si = var_find(c->f, src);
    if(si < 0){ tea_err("decode: %s not found\n",src); return 111; }
    Variable *sv = &c->f->vars[si];
    if(sv->type != VT_NUM){ tea_err("decode: %s must be numeric\n",src); return 109; }
    if(var_find(c->f, newvar) >= 0){ tea_err("decode: %s already exists\n",newvar); return 110; }
    VLabel *L = sv->vlabel[0] ? vlabel_get(c->ws, sv->vlabel) : NULL;
    if(!L){ tea_err("decode: %s has no value label attached\n",src); return 198; }

    Variable *nv = var_add(c->f, newvar, VT_STR);
    long mapped = 0;
    for(size_t i=0;i<c->f->nobs;i++){
        free(nv->str[i]);            /* free the empty placeholder from var_add */
        double x = sv->num[i];
        if(sv_is_miss(x)){ nv->str[i] = strdup(""); continue; }
        const char *txt = NULL;
        for(VLItem *it=L->items; it; it=it->next) if(it->val == x){ txt = it->txt; break; }
        nv->str[i] = strdup(txt ? txt : "");
        if(txt) mapped++;
    }
    if(!c->quiet) printf("(%ld obs decoded)\n", mapped);
    return 0;
}

/* ---- destring / tostring ----------------------------------------------- */

/* parse_destring: try to interpret a string as a number.
 *   Returns 1 on success (out = the parsed value), 0 on failure.
 *   Empty strings and the literal "." return success with missing.
 *   `ignore` is an optional set of characters to strip first (e.g. "$,"). */
static int parse_destring(const char *s, const char *ignore, double *out){
    if(!s || !s[0]){ *out = SV_MISS; return 1; }
    /* "." or ".a"–".z" are missing values */
    if(s[0]=='.' && (s[1]==0 || (s[1]>='a'&&s[1]<='z'&&s[2]==0))){ *out = SV_MISS; return 1; }
    char buf[128]; int bi=0;
    for(int i=0; s[i] && bi<127; i++){
        if(ignore && strchr(ignore, s[i])) continue;
        /* also strip whitespace (Stata behavior) */
        if(isspace((unsigned char)s[i])) continue;
        buf[bi++] = s[i];
    }
    buf[bi] = 0;
    if(bi == 0){ *out = SV_MISS; return 1; }
    char *end;
    double v = strtod(buf, &end);
    if(*end != 0) return 0;        /* leftover non-numeric content */
    *out = v;
    return 1;
}

/* destring varlist, {replace | generate(stub)} [force] [ignore("chars")]
 *   Convert string variables to numeric.  Without 'force', refuses if any
 *   value can't be parsed (Stata behavior).  With 'force', non-parseable
 *   values become missing. */
static int do_destring(Cmd *c){
    int *vs=NULL,nv=varlist_expand(c->f,c->varlist,&vs);
    if(nv<0){ tea_err("destring: variable not found\n"); return 111; }
    bool do_replace = opt_present(c->options,"replace");
    bool do_force   = opt_present(c->options,"force");
    char gen_stub[64]="";
    opt_value(c->options,"generate",gen_stub,sizeof gen_stub);
    if(!gen_stub[0]) opt_value(c->options,"gen",gen_stub,sizeof gen_stub);
    char ignore[64]="";
    opt_value(c->options,"ignore",ignore,sizeof ignore);
    /* strip surrounding quotes from ignore */
    if(ignore[0]=='"'){ char *e=strrchr(ignore,'"'); if(e>ignore)*e=0; memmove(ignore,ignore+1,strlen(ignore+1)+1); }

    if(!do_replace && !gen_stub[0]){
        tea_err("destring: must specify ',replace' or ',generate(stub)'\n");
        free(vs); return 198;
    }
    if(do_replace && gen_stub[0]){
        tea_err("destring: cannot specify both ',replace' and ',generate()'\n");
        free(vs); return 198;
    }

    /* First pass: validate that everything parses (unless ,force). */
    if(!do_force){
        for(int k=0;k<nv;k++){
            Variable *v = &c->f->vars[vs[k]];
            if(v->type != VT_STR) continue;
            for(size_t i=0;i<c->f->nobs;i++){
                double tmp;
                if(!parse_destring(v->str[i], ignore[0]?ignore:NULL, &tmp)){
                    tea_err("destring: %s contains non-numeric value at obs %zu: %s\n"
                            "         (use ',force' to convert anyway, replacing with missing)\n",
                            v->name, i+1, v->str[i]);
                    free(vs); return 109;
                }
            }
        }
    }

    /* Second pass: do the conversion. */
    long changed=0, bad=0;
    for(int k=0;k<nv;k++){
        Variable *src = &c->f->vars[vs[k]];
        if(src->type != VT_STR){
            /* already numeric — skip silently (Stata behavior) */
            continue;
        }
        /* gather the numeric values into a buffer */
        double *vals = malloc(c->f->nobs * sizeof(double));
        for(size_t i=0;i<c->f->nobs;i++){
            double tmp;
            if(parse_destring(src->str[i], ignore[0]?ignore:NULL, &tmp)){
                vals[i] = tmp;
            } else {
                /* must be ,force path here since pass 1 would have rejected */
                vals[i] = SV_MISS;
                bad++;
            }
        }
        if(do_replace){
            /* in-place: free the string storage, switch type, install nums */
            for(size_t i=0;i<c->f->nobs;i++) free(src->str[i]);
            free(src->str); src->str = NULL;
            src->type = VT_NUM;
            src->num = vals;            /* take ownership */
            src->cap = c->f->nobs;      /* swapped storage: cap = new alloc */
            snprintf(src->format, sizeof src->format, "%%9.0g");
        } else {
            /* gen(stub): create stub<varname> or use stub directly if 1 var */
            char newname[64];
            if(nv == 1) snprintf(newname, sizeof newname, "%s", gen_stub);
            else snprintf(newname, sizeof newname, "%s%s", gen_stub, src->name);
            if(var_find(c->f, newname) >= 0){
                tea_err("destring: %s already exists\n", newname);
                free(vals); free(vs); return 110;
            }
            Variable *dst = var_add(c->f, newname, VT_NUM);
            memcpy(dst->num, vals, c->f->nobs * sizeof(double));
            free(vals);
        }
        changed++;
    }
    if(!c->quiet){
        if(do_force && bad)
            printf("(%ld var%s converted; %ld value%s set to missing)\n",
                   changed, changed==1?"":"s", bad, bad==1?"":"s");
        else
            printf("(%ld var%s converted)\n", changed, changed==1?"":"s");
    }
    free(vs);
    return 0;
}

/* tostring varlist, {replace | generate(stub)} [force] [format(%fmt)]
 *   Convert numeric variables to string.  Without 'force', refuses if the
 *   conversion would lose information (a non-integer when default format
 *   produces an integer).  With 'force', always converts using format(). */
static int do_tostring(Cmd *c){
    int *vs=NULL,nv=varlist_expand(c->f,c->varlist,&vs);
    if(nv<0){ tea_err("tostring: variable not found\n"); return 111; }
    bool do_replace = opt_present(c->options,"replace");
    bool do_force   = opt_present(c->options,"force");
    char gen_stub[64]="";
    opt_value(c->options,"generate",gen_stub,sizeof gen_stub);
    if(!gen_stub[0]) opt_value(c->options,"gen",gen_stub,sizeof gen_stub);
    char fmt[32]="%.10g";
    char fmt_user[32]="";
    if(opt_value(c->options,"format",fmt_user,sizeof fmt_user) && fmt_user[0])
        snprintf(fmt, sizeof fmt, "%s", fmt_user);

    if(!do_replace && !gen_stub[0]){
        tea_err("tostring: must specify ',replace' or ',generate(stub)'\n");
        free(vs); return 198;
    }
    long changed = 0;
    for(int k=0;k<nv;k++){
        Variable *src = &c->f->vars[vs[k]];
        if(src->type != VT_NUM){ continue; }  /* already string */
        /* Allocate string storage */
        char **strs = malloc(c->f->nobs * sizeof(char*));
        bool lossy = false;
        for(size_t i=0;i<c->f->nobs;i++){
            double x = src->num[i];
            char buf[64];
            if(sv_is_miss(x)){ buf[0]='.'; buf[1]=0; }
            else snprintf(buf, sizeof buf, fmt, x);
            strs[i] = strdup(buf);
            /* lossy check (not in ,force): does buf round-trip back to x? */
            if(!do_force && !sv_is_miss(x)){
                char *e; double back = strtod(buf, &e);
                if(*e != 0 || back != x) lossy = true;
            }
        }
        if(!do_force && lossy){
            for(size_t i=0;i<c->f->nobs;i++) free(strs[i]);
            free(strs);
            tea_err("tostring: %s cannot be converted reversibly under format '%s'\n"
                    "          (use ',force' to convert anyway, or pass ',format(...)')\n",
                    src->name, fmt);
            free(vs); return 109;
        }
        if(do_replace){
            free(src->num); src->num = NULL;
            src->type = VT_STR;
            src->str = strs;
            src->cap = c->f->nobs;   /* swapped storage: cap = new alloc */
            snprintf(src->format, sizeof src->format, "%%9s");
        } else {
            char newname[64];
            if(nv == 1) snprintf(newname, sizeof newname, "%s", gen_stub);
            else snprintf(newname, sizeof newname, "%s%s", gen_stub, src->name);
            if(var_find(c->f, newname) >= 0){
                for(size_t i=0;i<c->f->nobs;i++) free(strs[i]);
                free(strs);
                tea_err("tostring: %s already exists\n", newname);
                free(vs); return 110;
            }
            Variable *dst = var_add(c->f, newname, VT_STR);
            /* var_add allocated empty strings; swap in ours */
            for(size_t i=0;i<c->f->nobs;i++){ free(dst->str[i]); dst->str[i] = strs[i]; }
            free(strs);   /* string contents transferred, array container freed */
        }
        changed++;
    }
    if(!c->quiet) printf("(%ld var%s converted)\n", changed, changed==1?"":"s");
    free(vs);
    return 0;
}

static int do_recode(Cmd *c){
    /* ---- recode ------------------------------------------------------- *
     *
     * Stata: `recode varlist (rule) (rule) ... [, gen(stub) test]`
     *
     * Applies value-recoding rules in order to each variable in varlist.
     * Rules have the form `(lhs = rhs)` or `(lhs = rhs "label")`.
     *
     * lhs forms:
     *   single value   (7 = 9)
     *   range          (1/3 = 1)            inclusive
     *   open-low       (min/0 = .)
     *   open-high      (65/max = 1)
     *   list           (1 3 5 = 1)
     *   missing        (missing = 9)        any missing code
     *   nonmissing     (nonmissing = 1)
     *   else / *       (else = .)           catch-all (must be last)
     *
     * rhs is a single numeric value or `.` for system missing.
     *
     * If `gen(stub)` is given, the recoded values go to NEW variables
     * named stub_OLDNAME (one per input variable).  Without gen, the
     * original variables are modified in place.
     *
     * Rules are evaluated in order; the first matching rule wins, so
     * put more specific rules before broader ones.  Missing values
     * pass through unless an explicit `(missing = ...)` rule catches them.
     * ------------------------------------------------------------------ */
    /* recode varlist (rule) (rule) ... [, gen(stub)] */
    char gen[33]=""; int hasgen=opt_value(c->options,"generate",gen,sizeof gen)
                          || opt_value(c->options,"gen",gen,sizeof gen);
    const char *s=c->varlist;
    /* varlist is up to first '(' */
    const char *lp=strchr(s,'(');
    char vlspec[512]; snprintf(vlspec,sizeof vlspec,"%.*s",(int)(lp?lp-s:(long)strlen(s)),s);
    int *vv=NULL,nv=varlist_expand(c->f,vlspec,&vv);
    if(nv<1){ tea_err("recode: variables not found\n"); free(vv); return 111; }
    RRule rules[64]; int nr=0;
    const char *p=lp;
    while(p&&*p){
        p=strchr(p,'('); if(!p)break; p++;
        const char *e=strchr(p,')'); if(!e)break;
        char body[256]; snprintf(body,sizeof body,"%.*s",(int)(e-p),p);
        char *eq=strchr(body,'='); double to=SV_MISS; int has_to=0;
        if(eq){ *eq=0; char *tp=eq+1; while(*tp==' ')tp++;
            if(*tp=='.'&&(tp[1]==0||tp[1]==' ')) to=SV_MISS; else to=strtod(tp,NULL);
            has_to=1; }
        char *sp=NULL;
        for(char *t=strtok_r(body," ",&sp);t;t=strtok_r(NULL," ",&sp)){
            RRule r; memset(&r,0,sizeof r); r.to=to; r.has_to=has_to;
            if(!strcmp(t,"missing")) r.kind=1;
            else if(!strcmp(t,"nonmissing")) r.kind=2;
            else if(!strcmp(t,"else")||!strcmp(t,"*")) r.kind=3;
            else { char *sl=strchr(t,'/');
                if(sl){ *sl=0; r.a=strtod(t,NULL); r.b=strtod(sl+1,NULL); }
                else r.a=r.b=strtod(t,NULL);
                r.kind=0; }
            if(nr<64)rules[nr++]=r;
        }
        p=e+1;
    }
    for(int k=0;k<nv;k++){
        Variable *src=&c->f->vars[vv[k]]; if(src->type!=VT_NUM)continue;
        Variable *dst=src;
        if(hasgen){ char nm[33]; snprintf(nm,33,"%s%s",gen, nv>1?src->name:"");
            if(nv==1) snprintf(nm,33,"%s",gen);
            dst=var_add(c->f,nm,VT_NUM); src=&c->f->vars[vv[k]]; }
        for(size_t i=0;i<c->f->nobs;i++){
            double x=src->num[i], outv=x; int done=0;
            for(int rI=0;rI<nr&&!done;rI++){ RRule *r=&rules[rI];
                int match=0;
                if(r->kind==1) match=sv_is_miss(x);
                else if(r->kind==2) match=!sv_is_miss(x);
                else if(r->kind==3) match=1;
                else match=!sv_is_miss(x)&&x>=r->a&&x<=r->b;
                if(match){ if(r->has_to)outv=r->to; done=1; }
            }
            dst->num[i]=outv;
        }
    }
    free(vv);
    if(hasgen) frame_unsort(c->f);
    return 0;
}

/* ---- codebook ---------------------------------------------------------- */
static int do_codebook(Cmd *c){
    int *vs=NULL,nv;
    if(c->varlist[0]) nv=varlist_expand(c->f,c->varlist,&vs);
    else { nv=c->f->nvar; vs=malloc(nv*sizeof(int)); for(int i=0;i<nv;i++)vs[i]=i; }
    if(nv<0){ tea_err("codebook: variable not found\n"); return 111; }
    for(int j=0;j<nv;j++){ Variable*v=&c->f->vars[vs[j]];
        printf("\n%-20s %s\n",v->name,v->vlabel);
        printf("------------------------------------------------------------\n");
        if(v->type==VT_STR){
            long miss=0; size_t uniq=0; char **seen=calloc(c->f->nobs+1,sizeof(char*));
            for(size_t i=0;i<c->f->nobs;i++){ if(!v->str[i][0]){miss++;continue;}
                int dup=0; for(size_t u=0;u<uniq;u++)if(!strcmp(seen[u],v->str[i])){dup=1;break;}
                if(!dup)seen[uniq++]=v->str[i]; }
            printf("                  type:  string\n");
            printf("         unique values:  %zu\n",uniq);
            printf("           missing \"\":  %ld/%zu\n",miss,c->f->nobs);
            free(seen);
        } else {
            double mn=INFINITY,mx=-INFINITY,sm=0; long n=0,miss=0; size_t uniq=0;
            double *seen=calloc(c->f->nobs+1,sizeof(double));
            for(size_t i=0;i<c->f->nobs;i++){ double x=v->num[i];
                if(sv_is_miss(x)){miss++;continue;}
                sm+=x;n++; if(x<mn)mn=x; if(x>mx)mx=x;
                int dup=0; for(size_t u=0;u<uniq;u++)if(seen[u]==x){dup=1;break;}
                if(!dup&&uniq<(size_t)c->f->nobs)seen[uniq++]=x; }
            printf("                  type:  numeric%s\n",v->vallab[0]?" (labeled)":"");
            printf("         unique values:  %zu\n",uniq);
            printf("           missing .:  %ld/%zu\n",miss,c->f->nobs);
            if(n) printf("                 range:  [%g, %g]\n                  mean:  %g\n",mn,mx,sm/n);
            free(seen);
        }
    }
    free(vs); return 0;
}

static int do_save(Cmd *c){
    /* ---- save --------------------------------------------------------- *
     *
     * Stata: `save FILENAME [, replace]`
     *
     * Writes the in-memory dataset to disk.  Format is selected by
     * extension:
     *   .dta   Stata format, via readstat.  Per-column type compression
     *          mirroring Stata's `compress` command — float/int columns
     *          stored as the smallest fitting Stata type.  Round-trips
     *          variable names, formats, variable labels, value labels.
     *          NOT preserved: xtset state (use re-runs xtset).
     *   .tea   Native tea binary.  Faster than .dta for round-trips
     *          between tea sessions.  No external library dependency.
     *
     * If no extension is given, defaults to .dta (Stata-compatible).
     *
     * The `replace` option is REQUIRED if the target file already exists,
     * to prevent accidental data loss (matches Stata).
     * ------------------------------------------------------------------ */
    char fn[1024]=""; const char *sv = c->varlist; scan_filename(&sv, fn, sizeof fn);
    /* default extension is .dta (Stata-compatible); .tea is opt-in */
    if(!strstr(fn,"."))strcat(fn,".dta");

    /* .dta -> readstat-backed writer with per-column compression */
    if(ends_with_ci(fn, ".dta")){
        /* optional `version(NUM)' to pick a DTA format (104-119) */
        int version = 0;
        char vb[16]; if(opt_value(c->options,"version",vb,sizeof vb)){
            version = atoi(vb);
        }
        const char *err = NULL;
        int rc = dta_write(c->f, c->ws, fn, version, &err);
        if(rc){ tea_err("save: %s\n", err ? err : "write failed"); return rc; }
        if(!c->quiet) printf("file %s saved\n", fn);
        return 0;
    }

    /* native .tea binary format (fast internal exchange) */
    FILE *fp=fopen(fn,"wb"); if(!fp)return 603;
    fwrite("TEA1",1,4,fp);
    fwrite(&c->f->nobs,sizeof(size_t),1,fp); fwrite(&c->f->nvar,sizeof(int),1,fp);
    for(int j=0;j<c->f->nvar;j++){ Variable*v=&c->f->vars[j];
        fwrite(v->name,1,33,fp); fwrite(&v->type,sizeof(VarType),1,fp);
        fwrite(v->format,1,33,fp); fwrite(v->vlabel,1,81,fp); }
    for(int j=0;j<c->f->nvar;j++){ Variable*v=&c->f->vars[j];
        if(v->type==VT_NUM)fwrite(v->num,sizeof(double),c->f->nobs,fp);
        else for(size_t i=0;i<c->f->nobs;i++){ int L=(int)strlen(v->str[i]); fwrite(&L,4,1,fp); fwrite(v->str[i],1,L,fp);} }
    fclose(fp); if(!c->quiet)printf("file %s saved\n",fn); return 0;
}
/* ---- sysuse: load a bundled practice dataset ---------------------------
 * The datasets are embedded in the binary (src/sysdata.c, generated from
 * the CSVs in data/ by tools/gen_sysdata.py — provenance in data/SOURCES.md), so
 * `sysuse grunfeld` works identically on native and WASM builds with no
 * files installed.  Loading rides the existing CSV parser: the bytes are
 * written to a temp file and fed to load_csv_into, then unlinked.
 *
 *   sysuse dir             list bundled datasets
 *   sysuse NAME [, clear]  load one (clear required if data in memory)
 * ---------------------------------------------------------------------- */
/* ---- history: list / save / clear the interactive command history ------ */
/* ---- confirm: assert existence/type; the capture-confirm idiom --------- */
static int do_confirm(Cmd *c){
    char t1[32]="", t2[32]="", rest[512]="";
    sscanf(c->args, "%31s %31s", t1, t2);
    int isnew = !strcmp(t1,"new");
    const char *kind = isnew ? t2 : t1;
    /* the object name = remainder after the keywords, unquoted */
    const char *p = c->args;
    for(int k = 0; k < (isnew?2:1); k++){ while(*p==' ')p++; while(*p&&*p!=' ')p++; }
    while(*p==' ')p++;
    snprintf(rest,sizeof rest,"%s",p);
    size_t L=strlen(rest); while(L&&(rest[L-1]==' '))rest[--L]=0;
    if(L>=2 && rest[0]=='"' && rest[L-1]=='"'){ rest[L-1]=0; memmove(rest,rest+1,L-1); }
    if(!kind[0]||!rest[0]){ tea_err("confirm: syntax is confirm [new] file|variable NAME\n"); return 198; }

    if(!strcmp(kind,"file")){
        int exists = (access(rest,F_OK)==0);
        if(isnew){ if(exists){ tea_err("file %s already exists\n",rest); return 602; } return 0; }
        if(!exists){
            tea_err("file %s not found\n",rest);
            if(strchr(rest,'\\'))
                tea_err("(note: the path contains backslashes \u2014 on Linux/macOS use forward slashes)\n");
            return 601;
        }
        return 0;
    }
    if(!strcmp(kind,"variable")||!strcmp(kind,"numeric")||!strcmp(kind,"string")){
        const char *vn = rest;
        if(!strcmp(kind,"numeric")||!strcmp(kind,"string")){
            /* confirm numeric variable X / confirm string variable X */
            if(!strncmp(rest,"variable ",9)) vn = rest+9;
        }
        int vi = var_find(c->f, vn);
        if(isnew){ if(vi>=0){ tea_err("variable %s already defined\n",vn); return 110; } return 0; }
        if(vi<0){ tea_err("variable %s not found\n",vn); return 111; }
        if(!strcmp(kind,"numeric") && c->f->vars[vi].type!=VT_NUM){ tea_err("%s is a string variable\n",vn); return 7; }
        if(!strcmp(kind,"string") && c->f->vars[vi].type==VT_NUM){ tea_err("%s is a numeric variable\n",vn); return 7; }
        return 0;
    }
    tea_err("confirm: unknown kind '%s' (file|variable)\n", kind);
    return 198;
}

static int do_history(Cmd *c){
    extern Interp *g_tea_interp;
    Interp *ip = g_tea_interp;
    if(!ip){ tea_err("history: no session\n"); return 198; }
    char sub[64]=""; sscanf(c->args,"%63s",sub);
    if(!strcmp(sub,"clear")){
        for(int i=0;i<ip->nhist;i++) free(ip->hist[i]);
        ip->nhist = 0;
        printf("(history cleared)\n");
        return 0;
    }
    if(!strcmp(sub,"save")){
        char fn[512]="";
        { char buf[600]; snprintf(buf,sizeof buf,"%s",c->args);
          buf[strcspn(buf,",")]=0; sscanf(buf,"%*s %511s",fn); }
        if(!fn[0]){ tea_err("history save: filename required\n"); return 198; }
        if(access(fn,F_OK)==0 && !opt_present(c->options,"replace")){
            tea_err("history save: file exists (use ,replace)\n"); return 602; }
        FILE *o=fopen(fn,"w");
        if(!o){ tea_err("history save: cannot write %s\n",fn); return 603; }
        for(int i=0;i<ip->nhist;i++) fprintf(o,"%s\n",ip->hist[i]);
        fclose(o);
        printf("(%d lines saved to %s)\n", ip->nhist, fn);
        return 0;
    }
    int n = ip->nhist;                       /* history [N]: last N lines */
    if(sub[0]){ int k=atoi(sub); if(k>0 && k<n) n=k; }
    for(int i=ip->nhist-n; i<ip->nhist; i++)
        printf("%5d  %s\n", i+1, ip->hist[i]);
    return 0;
}

static int do_sysuse(Cmd *c){
    char name[64]="";
    {   /* c->args still carries the ', options' tail — cut at the comma */
        char buf[128]; snprintf(buf,sizeof buf,"%s",c->args);
        buf[strcspn(buf,",")]=0;
        sscanf(buf,"%63s",name);
    }
    if(!name[0] || !strcmp(name,"dir")){
        printf("bundled practice datasets (load with: sysuse NAME):\n");
        for(int i=0;i<SYSDATA_N;i++)
            printf("  %-9s %s\n", SYSDATA[i].name, SYSDATA[i].desc);
        printf("provenance & citations: data/SOURCES.md in the source tree\n");
        return 0;
    }
    const SysDataset *d=NULL;
    for(int i=0;i<SYSDATA_N;i++) if(!strcmp(name,SYSDATA[i].name)){ d=&SYSDATA[i]; break; }
    if(!d){
        tea_err("sysuse: no bundled dataset '%s' (try: sysuse dir)\n", name);
        return 601;
    }
    if(c->f->nvar > 0 && !opt_present(c->options,"clear")){
        tea_err("sysuse: no; data in memory would be lost (use ',clear' to discard it)\n");
        return 4;
    }
    char tmp[] = "/tmp/tea_sysuse_XXXXXX";
    int fd = mkstemp(tmp);
    if(fd < 0){ tea_err("sysuse: cannot create temp file\n"); return 693; }
    FILE *o = fdopen(fd, "wb");
    if(!o){ close(fd); unlink(tmp); tea_err("sysuse: cannot open temp file\n"); return 693; }
    fwrite(d->csv, 1, d->len, o);
    fclose(o);
    frame_clear(c->f);
    int rc = load_csv_into(c->f, tmp, ',');
    unlink(tmp);
    if(rc==0)
        printf("(%s: %zu obs loaded \u2014 %s)\n", d->name, c->f->nobs, d->desc);
    return rc;
}

static int do_use(Cmd *c){
    /* ---- use ---------------------------------------------------------- *
     *
     * Stata: `use [varlist] [if] [in] using FILENAME [, clear]`
     *
     * Loads a dataset from disk into the current frame.  Extension
     * selects format (.dta via readstat, .tea native).  If no extension,
     * defaults to .dta.
     *
     * `clear` is REQUIRED if data is already in memory (Stata-compat
     * footgun protection).  Without `clear` and with data present,
     * returns rc=4 with no action.
     *
     * Selective load: a leading varlist restricts which columns to read
     * (efficient for wide .dta files); `if` / `in` clauses filter rows.
     *
     * `use foo` reads `foo.dta`; `use "foo.tea"` reads native format.
     *
     * After use, xtset/tsset state is NOT restored (see KNOWN_BUGS).
     * Re-run xtset or tsset before time-series / panel commands.
     * ------------------------------------------------------------------ */
    char fn[1024]=""; const char *s=c->varlist; if(!strncmp(s,"using",5))s+=5; while(*s==' ')s++;
    scan_filename(&s, fn, sizeof fn);
    /* default extension is .dta (Stata-compatible); .tea is opt-in */
    if(!strstr(fn,"."))strcat(fn,".dta");
    bool has_clear = opt_present(c->options,"clear");
    if(c->f->nvar > 0 && !has_clear){
        tea_err("use: no; data in memory would be lost (use ',clear' to discard it)\n");
        return 4;
    }
    frame_clear(c->f);
    /* Stata semantics: `use, clear` resets data AND value labels.
     * Otherwise stale labels from a prior session would leak through. */
    ws_clear_labels(c->ws);
    c->f->data_label[0] = 0;
    c->f->fweight_var[0] = 0;
    const char *load_err = NULL;
    int rc=load_into(c->f,c->ws,fn,&load_err);
    if(rc){
        if(load_err) tea_err("use: %s\n", load_err);
        else         tea_err("use: cannot read %s (rc=%d)\n",fn,rc);
        return rc;
    }
    if(!c->quiet)printf("(%d vars, %zu obs)\n",c->f->nvar,c->f->nobs);
    return 0;
}

/* ---- set obs / clear / frame ------------------------------------------- */
static int do_clear(Cmd *c){
    frame_clear(c->f);
    ws_clear_labels(c->ws);
    c->f->data_label[0] = 0;
    c->f->fweight_var[0] = 0;
    return 0;
}
static int do_setobs(Cmd *c){ long n=strtol(c->args,NULL,10);
    if(n<(long)c->f->nobs){tea_err("set obs: cannot shrink\n");return 198;}
    frame_set_nobs(c->f,(size_t)n); if(!c->quiet)printf("number of observations set to %ld\n",n); return 0; }
/* deep-copy src's data into dst (dst must be empty). */
static void clone_frame(Frame *dst,Frame *src){
    for(int j=0;j<src->nvar;j++){ Variable*s=&src->vars[j];
        Variable*d=var_add(dst,s->name,s->type);
        snprintf(d->format,33,"%s",s->format); snprintf(d->vlabel,81,"%s",s->vlabel); }
    frame_set_nobs(dst,src->nobs);
    for(int j=0;j<src->nvar;j++){ Variable*s=&src->vars[j],*d=&dst->vars[j];
        if(s->type==VT_NUM) memcpy(d->num,s->num,src->nobs*sizeof(double));
        else for(size_t i=0;i<src->nobs;i++) str_set(d,i,s->str[i]); }
}
static void frame_unlink_free(Workspace *ws,Frame *f){
    for(Frame**pp=&ws->frames;*pp;pp=&(*pp)->next) if(*pp==f){ *pp=f->next; break; }
    frame_clear(f); free(f->sortvars); free(f);
}

static int do_frame(Cmd *c){
    /* ---- frame -------------------------------------------------------- *
     *
     * Stata: `frame create | change | copy | drop | rename ...`
     *
     * Multiple named datasets can be in memory simultaneously, each in
     * its own frame.  Per-frame state includes: variables/observations,
     * sort state, xtset/tsset declaration.  Only one frame is "current"
     * at a time; commands operate on the current frame.
     *
     * Subcommands:
     *   create NEW            make an empty frame named NEW
     *   change NAME           switch to frame NAME (becomes current)
     *   copy SRC DST [, repl] duplicate SRC into a new frame DST
     *                         (essential before reshape/collapse if
     *                         you want to keep the original)
     *   drop NAME             remove frame NAME (errors if it's current)
     *   rename OLD NEW        rename a frame
     *   dir                   list all frames with their sizes
     *
     * The default frame is named "default" and exists at startup.
     * ------------------------------------------------------------------ */
    char sub[32]="",a2[64]="",a3[64]=""; sscanf(c->args,"%31s %63s %63s",sub,a2,a3);
    if(!strcmp(sub,"create")){
        if(frame_get(c->ws,a2)){ tea_err("frame %s already exists\n",a2); return 110; }
        frame_create(c->ws,a2);
    }
    else if(!strcmp(sub,"copy")){            /* frame copy old new [, replace] */
        Frame*src=frame_get(c->ws,a2); if(!src){ tea_err("frame %s not found\n",a2); return 111; }
        Frame*dst=frame_get(c->ws,a3);
        if(dst){ if(!opt_present(c->options,"replace")){ tea_err("frame %s exists (use , replace)\n",a3); return 110; } frame_clear(dst); }
        else dst=frame_create(c->ws,a3);
        clone_frame(dst,src);
        if(!c->quiet) printf("(%d vars, %zu obs copied to frame %s)\n",dst->nvar,dst->nobs,a3);
    }
    else if(!strcmp(sub,"rename")){          /* frame rename old new */
        Frame*src=frame_get(c->ws,a2); if(!src){ tea_err("frame %s not found\n",a2); return 111; }
        if(frame_get(c->ws,a3)){ tea_err("frame %s already exists\n",a3); return 110; }
        snprintf(src->name,33,"%s",a3);
    }
    else if(!strcmp(sub,"put")){             /* frame put varlist , into(name) */
        char into[33]=""; if(!opt_value(c->options,"into",into,sizeof into)){ tea_err("frame put: into() required\n"); return 198; }
        char into2[33]; sscanf(into,"%32s",into2);
        char vl[1024]; const char *cm=strchr(c->args,',');
        const char *vs=c->args+strlen("put"); while(*vs==' ')vs++;
        snprintf(vl,sizeof vl,"%.*s",(int)(cm?cm-vs:(long)strlen(vs)),vs);
        int *vv=NULL,nv=varlist_expand(c->f,vl,&vv);
        if(nv<1){ tea_err("frame put: variables not found\n"); free(vv); return 111; }
        if(frame_get(c->ws,into2)){ tea_err("frame %s exists\n",into2); free(vv); return 110; }
        Frame*dst=frame_create(c->ws,into2);
        for(int k=0;k<nv;k++){ Variable*s=&c->f->vars[vv[k]]; Variable*d=var_add(dst,s->name,s->type);
            snprintf(d->format,33,"%s",s->format); snprintf(d->vlabel,81,"%s",s->vlabel); }
        frame_set_nobs(dst,c->f->nobs);
        for(int k=0;k<nv;k++){ Variable*s=&c->f->vars[vv[k]],*d=&dst->vars[k];
            if(s->type==VT_NUM) memcpy(d->num,s->num,c->f->nobs*sizeof(double));
            else for(size_t i=0;i<c->f->nobs;i++) str_set(d,i,s->str[i]); }
        free(vv);
        if(!c->quiet) printf("(%d vars, %zu obs put into frame %s)\n",dst->nvar,dst->nobs,into2);
    }
    else if(!strcmp(sub,"change")||!strcmp(sub,"")){
        const char *target = a2[0]?a2:sub;
        Frame*f=frame_get(c->ws,target);
        if(a2[0]){ if(f) c->ws->cur=f; else { tea_err("frame %s not found\n",a2); return 111; } }
    }
    else if(!strcmp(sub,"drop")){
        Frame*f=frame_get(c->ws,a2);
        if(!f){ tea_err("frame %s not found\n",a2); return 111; }
        if(f==c->ws->cur){ tea_err("cannot drop the current frame\n"); return 198; }
        frame_unlink_free(c->ws,f);
    }
    else if(!strcmp(sub,"dir")||!strcmp(sub,"list")){
        for(Frame*f=c->ws->frames;f;f=f->next)
            printf("  %-16s %d vars, %zu obs%s\n",f->name,f->nvar,f->nobs,f==c->ws->cur?"  *":"");
    }
    else { tea_err("frame: unknown subcommand %s\n",sub); return 198; }
    return 0;
}

/* ---- dispatch table ---------------------------------------------------- */
/* ---- help / version / pwd / cd / log / exit ---------------------------- */

typedef struct { const char *name; int (*fn)(Cmd*); int needs_data; const char *help; } Disp;
extern Disp TABLE[];

static int do_help(Cmd *c){
    extern Disp TABLE[];
    /* native statements (handled in run_line, not dispatch) — listed here
     * so 'help X' still works.  Matched against `what` below. */
    static const struct { const char *name; const char *help; } NATIVE[] = {
        {"assert",  "assert exp [if]                              fail with rc=9 if exp is false anywhere"},
        {"shell",   "shell cmd      |  ! cmd                       run a shell command"},
        {"!",       "! cmd          |  shell cmd                   run a shell command"},
        {"#delimit","#delimit ;  |  #delimit cr                   switch statement terminator (; or newline)"},
        {"display", "display \"str\" exp \"str\" ...                   print mixed strings and expressions"},
        {"di",      "di \"str\" exp ...                              short for display"},
        {"local",   "local name = exp  |  local name text          set a local macro `name'"},
        {"global",  "global name = exp |  global name text         set a global macro $name"},
        {"foreach", "foreach x in|of varlist LIST { ... }          loop over a list"},
        {"forvalues","forvalues i = a/b { ... }  |  a(step)b       loop over a numeric range"},
        {"if",      "if (cond) { ... } [else { ... }]              conditional"},
        {"capture", "capture CMD                                   swallow error, expose rc"},
        {"quietly", "quietly CMD  |  qui CMD                       suppress output of one command"},
        {NULL,NULL}
    };
    char what[64]=""; sscanf(c->args,"%63s",what);
    if(!what[0]){
        printf("\ntea %s — tiny econometric assistant\n",TEA_VERSION);
        printf("type 'help CMD' for one command's syntax.  available commands:\n\n");
        int col=0;
        for(Disp *d=TABLE;d->name;d++){
            if(!d->help) continue;
            printf("  %-12s",d->name);
            if(++col==5){ putchar('\n'); col=0; }
        }
        if(col) putchar('\n');
        printf("\nnative statements: assert shell ! #delimit display local global\n");
        printf("                   foreach forvalues if capture quietly\n");
        printf("\nqualifiers usable on most commands:\n");
        printf("  [by[sort] g (s):]  if exp   in range   [weight=exp]   , options\n");
        return 0;
    }
    for(int i=0;NATIVE[i].name;i++) if(!strcmp(NATIVE[i].name,what)){
        printf("\n  %s\n\n",NATIVE[i].help); return 0; }
    for(Disp *d=TABLE;d->name;d++) if(!strcmp(d->name,what)){
        if(d->help) printf("\n  %s\n\n",d->help);
        else for(Disp *p=TABLE;p->name;p++) if(p->fn==d->fn && p->help){ printf("\n  %s\n  (alias for '%s')\n\n",p->help,p->name); break; }
        return 0;
    }
    tea_err("help: unknown command '%s'\n",what); return 111;
}

static int do_version(Cmd *c){ (void)c;
    printf("tea %s — tiny econometric assistant\n",TEA_VERSION);
    printf("built %s %s\n",__DATE__,__TIME__);
    printf("Copyright (C) 2026 Mico Mrkaic.  License GPLv3+: GNU GPL v3 or later.\n");
    printf("This is free software: you are free to change and redistribute it.\n");
    printf("There is NO WARRANTY, to the extent permitted by law.\n");
    printf("\n");
    printf("tea is an independent project.  It is not affiliated with, endorsed by,\n");
    printf("or sponsored by StataCorp LLC.  Stata is a trademark of StataCorp LLC.\n");
    return 0;
}

static int do_pwd(Cmd *c){ (void)c;
    char buf[4096];
    if(getcwd(buf,sizeof buf)) printf("%s\n",buf);
    else { perror("pwd"); return 1; }
    return 0;
}

static int do_cd(Cmd *c){
    char dir[1024]=""; const char *s=c->args; scan_filename(&s, dir, sizeof dir);
    if(!dir[0]){ const char *h=getenv("HOME"); if(h) snprintf(dir,sizeof dir,"%s",h); else return 0; }
    if(chdir(dir)){ perror("cd"); return 1; }
    if(!c->quiet){ char buf[4096]; if(getcwd(buf,sizeof buf)) printf("%s\n",buf); }
    return 0;
}

/* mkdir DIR [, recursive] — create a directory.  With 'recursive', creates
 * intermediate parents (equivalent to 'mkdir -p'). */
/* dir [pattern] — list files in current directory.  Stata's syntax. */
static int do_dir(Cmd *c){
    char pat[1024]=""; const char *s=c->varlist; scan_filename(&s, pat, sizeof pat);
    DIR *d = opendir(".");
    if(!d){ perror("dir"); return 601; }
    struct dirent *e;
    long count=0;
    while((e = readdir(d))){
        if(e->d_name[0]=='.') continue;       /* skip hidden + . / .. */
        if(pat[0] && fnmatch(pat, e->d_name, 0) != 0) continue;
        struct stat st;
        if(stat(e->d_name, &st) == 0){
            char kind = S_ISDIR(st.st_mode) ? 'd' : '-';
            printf("  %c  %10lld  %s\n", kind, (long long)st.st_size, e->d_name);
        } else {
            printf("  ?  %10s  %s\n", "?", e->d_name);
        }
        count++;
    }
    closedir(d);
    if(!count && pat[0]) printf("  (no files matching %s)\n", pat);
    return 0;
}

static int do_mkdir(Cmd *c){
    char dir[1024]=""; const char *s=c->varlist; scan_filename(&s, dir, sizeof dir);
    if(!dir[0]){ tea_err("mkdir: directory name required\n"); return 198; }
    bool recursive = opt_present(c->options,"recursive") || opt_present(c->options,"p");
    if(recursive && c->ip->strict_stata){
        tea_err("mkdir: 'recursive' is a tea extension (Stata's mkdir creates only one level).\n"
                "       Rerun with --tea-extensions to enable, or call mkdir once per level.\n");
        return 198;
    }
    if(!recursive){
        if(mkdir(dir, 0755) && errno != EEXIST){ perror("mkdir"); return 693; }
    } else {
        char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",dir);
        size_t L = strlen(tmp);
        if(L && tmp[L-1]=='/') tmp[--L] = 0;
        for(size_t i=1;i<L;i++){
            if(tmp[i]=='/'){ tmp[i]=0;
                if(mkdir(tmp, 0755) && errno != EEXIST){ perror("mkdir"); return 693; }
                tmp[i]='/'; }
        }
        if(mkdir(tmp, 0755) && errno != EEXIST){ perror("mkdir"); return 693; }
    }
    return 0;
}

/* rmdir DIR — remove an empty directory. */
static int do_rmdir(Cmd *c){
    char dir[1024]=""; const char *s=c->varlist; scan_filename(&s, dir, sizeof dir);
    if(!dir[0]){ tea_err("rmdir: directory required\n"); return 198; }
    if(rmdir(dir)){ perror("rmdir"); return 693; }
    return 0;
}

/* erase FILE  (alias: rm) — delete a file. */
static int do_erase(Cmd *c){
    char fn[1024]=""; const char *s=c->varlist; scan_filename(&s, fn, sizeof fn);
    if(!fn[0]){ tea_err("erase: filename required\n"); return 198; }
    if(unlink(fn)){ perror("erase"); return 602; }
    return 0;
}

/* copy SRC DST [, replace] — copy a file. */
static int do_copy(Cmd *c){
    char src[1024]="", dst[1024]="";
    const char *s = c->varlist;
    scan_filename(&s, src, sizeof src);
    scan_filename(&s, dst, sizeof dst);
    if(!src[0] || !dst[0]){ tea_err("copy: source and destination required\n"); return 198; }
    bool replace = opt_present(c->options,"replace");
    FILE *fi = fopen(src,"rb"); if(!fi){ perror("copy: source"); return 601; }
    if(!replace){ FILE *t = fopen(dst,"rb"); if(t){ fclose(t); fclose(fi);
        tea_err("copy: destination exists (use ,replace)\n"); return 602; } }
    FILE *fo = fopen(dst,"wb"); if(!fo){ perror("copy: destination"); fclose(fi); return 603; }
    char buf[65536]; size_t n;
    while((n = fread(buf,1,sizeof buf,fi)) > 0){
        if(fwrite(buf,1,n,fo) != n){ perror("copy: write"); fclose(fi); fclose(fo); return 603; }
    }
    fclose(fi); fclose(fo);
    return 0;
}

/* do FILENAME [args] — source another do-file from inside a session. */
static int do_dofile(Cmd *c){
    char fn[1024]=""; const char *s=c->varlist; scan_filename(&s, fn, sizeof fn);
    if(!fn[0]){ tea_err("do: filename required\n"); return 198; }
    FILE *fp = fopen(fn,"r");
    if(!fp){ tea_err("do: cannot open %s\n",fn); return 601; }
    extern int g_current_line;
    int saved = g_current_line; g_current_line = 0;
    int rc = run_stream(c->ip, fp, false);
    g_current_line = saved;
    fclose(fp);
    return rc;
}

static int do_log(Cmd *c){
    char sub[16]="",fn[1024]=""; sscanf(c->args,"%15s %1023[^,\n ]",sub,fn);
    if(!strcmp(sub,"close")){
        if(!g_logfp){ if(!c->quiet) printf("(no log open)\n"); return 0; }
        fclose(g_logfp); g_logfp=NULL;
        if(!c->quiet) printf("(log closed)\n");
        return 0;
    }
    if(!strcmp(sub,"using")){
        if(!fn[0]){ tea_err("log: filename required\n"); return 198; }
        if(g_logfp){ tea_err("log: a log is already open (log close first)\n"); return 604; }
        const char *mode = opt_present(c->options,"append")? "a" : "w";
        g_logfp = fopen(fn,mode);
        if(!g_logfp){ perror("log"); return 603; }
        if(!c->quiet) printf("(log to %s, mode=%s)\n",fn,mode);
        return 0;
    }
    tea_err("log: use 'log using FILE' or 'log close'\n"); return 198;
}

/* exit: signal main() to leave the loop.  We do this via a flag on the
 * workspace pointer hack — set ws->cur=NULL?  cleaner: use a static flag
 * checked by run_stream after each command. */
extern int g_exit_requested;        /* defined in interp.c */
extern int g_exit_code;
static int do_exit(Cmd *c){
    if(opt_present(c->options,"clear")) frame_clear(c->f);
    g_exit_requested = 1;
    char code[16]=""; sscanf(c->args,"%15s",code);
    g_exit_code = code[0]&&isdigit((unsigned char)code[0])? atoi(code) : 0;
    return 0;
}

static int wrap_gen(Cmd*c){return do_genrep(c,0);}
static int wrap_rep(Cmd*c){return do_genrep(c,1);}
static int wrap_drop(Cmd*c){return do_dropkeep(c,0);}
static int wrap_keep(Cmd*c){return do_dropkeep(c,1);}
static int wrap_ts(Cmd*c){return do_tsset(c,0);}
static int wrap_xt(Cmd*c){return do_tsset(c,1);} 


/* ---- completion oracle (web front-end) ---------------------------------
 * Newline-joined candidates for the word ending at `point` in `line`:
 * first word -> command names; after `sysuse`/`help` -> dataset/command
 * names; otherwise -> variable names of the current frame.  Returns the
 * number of candidates written.  Native builds use GNU readline instead;
 * this exists so the browser terminal completes like the real REPL. */
int tea_complete(Frame *f, const char *line, int point, char *out, size_t outsz)
{
    if(point < 0 || point > (int)strlen(line)) point = (int)strlen(line);
    int ws = point;                       /* start of the word being completed */
    while(ws > 0 && line[ws-1] != ' ' && line[ws-1] != ',' && line[ws-1] != '(')
        ws--;
    const char *word = line + ws;
    int wlen = point - ws;

    /* which context? first token decides */
    char first[64] = ""; sscanf(line, "%63s", first);
    int first_word = 1;
    for(int i = 0; i < ws; i++) if(line[i] != ' ') { first_word = 0; break; }

    size_t o = 0; int n = 0;
    #define EMIT(s) do{ size_t L=strlen(s); if(o+L+2<outsz){ memcpy(out+o,(s),L); out[o+L]='\n'; o+=L+1; n++; } }while(0)

    if(first_word){
        extern Disp TABLE[];
        for(int i = 0; TABLE[i].name; i++)
            if(!strncmp(TABLE[i].name, word, wlen)) EMIT(TABLE[i].name);
    } else if(!strcmp(first, "sysuse")){
        for(int i = 0; i < SYSDATA_N; i++)
            if(!strncmp(SYSDATA[i].name, word, wlen)) EMIT(SYSDATA[i].name);
        if(!strncmp("dir", word, wlen)) EMIT("dir");
    } else if(!strcmp(first, "help")){
        extern Disp TABLE[];
        for(int i = 0; TABLE[i].name; i++)
            if(!strncmp(TABLE[i].name, word, wlen)) EMIT(TABLE[i].name);
    } else if(f){
        for(int i = 0; i < f->nvar; i++)
            if(!strncmp(f->vars[i].name, word, wlen)) EMIT(f->vars[i].name);
    }
    #undef EMIT
    if(o < outsz) out[o] = 0;
    return n;
}

Disp TABLE[]={
    {"scatter",do_scatter,1,
        "scatter yvar xvar [if] [in] [, title() xtitle() ytitle() saving() noview]\n"
        "      SVG scatter plot; saving(FILE) writes to FILE instead of tea_graph.svg\n"
        "      e.g.  scatter gdp_growth inflation if year>2000, title(\"Growth vs inflation\")"},
    {"line",do_lineplot,1,
        "line yvar xvar [if] [in] [, sort title() xtitle() ytitle() saving() noview]\n"
        "      SVG line plot; connects points in data order (use sort to order by x)\n"
        "      e.g.  line gdp year if country==\"US\", sort"},
    {"histogram",do_histogram,1,
        "histogram var [if] [in] [, bins(#) freq title() saving() noview]\n"
        "      SVG histogram; density by default, freq for counts, auto bins = min(ceil(sqrt(N)),50)\n"
        "      e.g.  histogram wage, bins(30) freq"},
    {"hist",do_histogram,1,NULL},
    {"generate",wrap_gen,1,
        "generate [type] newvar = exp [if] [in]      create a new variable\n"
        "      e.g.  gen logy = log(income) if year >= 2000"},
    {"gen",wrap_gen,1,NULL},{"g",wrap_gen,1,NULL},
    {"replace",wrap_rep,1,
        "replace var = exp [if] [in]                  modify existing variable\n"
        "      e.g.  replace income = . if income < 0"},
    {"egen",do_egen,1,
        "egen newvar = fn(args) [, by(varlist)]       extended generate (mean/sum/group/...)\n"
        "      e.g.  egen meanY = mean(y), by(country)"},
    {"list",do_list,1,
        "list [varlist] [if] [in]                     print observations\n"
        "      e.g.  list country year gdp if year>2020 in 1/10"},
    {"l",do_list,1,NULL},
    {"summarize",do_summarize,1,
        "summarize [varlist] [if] [in] [weight] [, detail]      N/mean/sd/min/max; sets r()\n"
        "      e.g.  summarize gdp_growth, detail"},
    {"sum",do_summarize,1,NULL},{"su",do_summarize,1,NULL},
    {"tabstat",do_tabstat,1,
        "tabstat varlist [if] [in], statistics(stat ...) [by(var)] [columns(stat|var)]\n"
        "      stats: mean sd var cv min max range sum count median p1-p99 iqr\n"
        "      e.g.  tabstat gdp pop, stats(mean sd p25 p50 p75) by(region)"},
    {"count",do_count,1,"count [if] [in] [fw=]                        count matching observations"},
    {"describe",do_describe,1,"describe [varlist]                           show variables, types, formats"},
    {"desc",do_describe,1,NULL},{"des",do_describe,1,NULL},{"d",do_describe,1,NULL},
    {"drop",wrap_drop,1,
        "drop varlist | if exp | in range             remove vars or observations\n"
        "      e.g.  drop if gdp == .   |   drop tempvar1 tempvar2"},
    {"keep",wrap_keep,1,
        "keep varlist | if exp | in range             complement of drop\n"
        "      e.g.  keep country year gdp"},
    {"rename",do_rename,1,
        "rename oldname newname                       rename a variable\n"
        "      e.g.  rename _C* C*   (wildcard rename: strip leading underscore)"},
    {"ren",do_rename,1,NULL},
    {"order",do_order,1,
        "order varlist                                reorder variables (listed ones go first)\n"
        "      e.g.  order country year"},
    {"aorder",do_aorder,1,"aorder [varlist]                             alphabetize variable order"},
    {"label",do_label,1,
        "label variable v \"text\"                      attach a description to a variable\n"
        "    label define lblname # \"text\" # \"text\"   define a value-label set\n"
        "    label values varlist lblname             attach value labels to variables"},
    {"format",do_format,1,
        "format varlist %fmt                          set display format\n"
        "      e.g.  format date %td   |   format gdp %12.2f"},
    {"sort",do_sort,1,
        "sort varlist                                 ascending stable sort\n"
        "      e.g.  sort country year"},
    {"gsort",do_gsort,1,
        "gsort [+-]var [+-]var ...                    sort with per-key direction\n"
        "      e.g.  gsort -gdp +country  (gdp desc, then country asc)"},
    {"tabulate",do_tabulate,1,
        "tabulate var [if] [in] [weight]              one-way frequency table\n"
        "      e.g.  tabulate region if year == 2020"},
    {"tab",do_tabulate,1,NULL},
    {"tsset",wrap_ts,1,
        "tsset timevar [, format(%fmt)]               declare time series\n"
        "      e.g.  tsset year"},
    {"xtset",wrap_xt,1,
        "xtset panelvar timevar                       declare panel (gap-aware L./F./D./S.)\n"
        "      e.g.  xtset country year"},
    {"xtdescribe",do_xtdescribe,1,
        "xtdescribe                                   summarize panel structure\n"
        "      e.g.  xtdescribe   (requires xtset first; shows n, T, balance)"},
    {"xtdes",do_xtdescribe,1,NULL},
    {"xtreg",do_xtreg,1,
        "xtreg y x1 x2 ... [if] [in], fe [vce(robust|cluster v)]   panel FE OLS\n"
        "      e.g.  xtreg growth L.growth pop, fe\n"
        "            (requires xtset; within estimator; reports R-w/b/o, sigma_u/e, rho)"},
    {"hausman",do_hausman,1,
        "hausman                                       FE vs RE specification test\n"
        "      e.g.  xtreg y x, fe\n"
        "            xtreg y x, re\n"
        "            hausman   (Ho: cov(u_i, x) = 0; rejection means use FE)"},
    {"logit",do_logit,1,
        "logit y x1 x2 ... [if] [in] [weight], [vce(robust|cluster v)]   logistic regression\n"
        "      e.g.  logit voted income education, vce(cluster county)"},
    {"probit",do_probit,1,
        "probit y x1 x2 ... [if] [in] [weight], [vce(robust|cluster v)]   probit regression\n"
        "      e.g.  probit voted income education, vce(robust)"},
    {"ivregress",do_ivregress,1,
        "ivregress 2sls y [exog] (endog = instruments) [if] [in] [weight] [, vce(robust|cluster v)]\n"
        "      Two-stage least squares with first-stage F diagnostic.\n"
        "      e.g.  ivregress 2sls wage educ (exper = age momeduc daddyEduc)"},
    {"poisson",do_poisson,1,
        "poisson y x1 x2 ... [if] [in] [weight] [, vce(robust|cluster v)]   poisson regression\n"
        "      e.g.  poisson accidents speed_limit population, vce(cluster state)"},
    {"arima",do_arima,1,
        "arima y [exog] [if] [in], arima(p d q) [noconstant]   ARIMA(p,d,q) via conditional ML\n"
        "      e.g.  arima gdp, arima(1 1 1)              ARIMA(1,1,1) on gdp\n"
        "            arima cpi unemp, arima(2 0 1)        ARMAX(2,1) with exog regressor"},
    {"margins",do_margins,1,
        "margins , dydx(*|varlist) [atmeans]            average marginal effects\n"
        "      e.g.  logit y x1 x2\n"
        "            margins, dydx(*)              AME for all regressors\n"
        "            margins, dydx(x1) atmeans     MEM for x1 at sample means"},
    {"estimates",do_estimates,1,
        "estimates store|restore|dir|drop|table          named estimates ledger\n"
        "      e.g.  regress y x1 x2\n"
        "            estimates store m1\n"
        "            regress y x1 x2 x3\n"
        "            estimates store m2\n"
        "            estimates table m1 m2, se star  stats(N r2 rmse)"},
    {"est",do_estimates,1,
        "est = estimates  (short form)                   see `help estimates`"},
    {"estout",do_estout,1,
        "estout [names] [, format(latex|markdown|plain) se|t|p stars stats(...)]\n"
        "      Side-by-side LaTeX/markdown/plain table of stored estimates.\n"
        "      e.g.  estout m1 m2, stats(N r2 rmse) using table.tex"},
    {"collapse",do_collapse,1,
        "collapse (stat) v1 v2 ... , by(g) [weight]   aggregate to groups\n"
        "      e.g.  collapse (mean) gdp pop, by(region year)"},
    {"merge",do_merge,1,
        "merge 1:1|m:1|1:m keyvars using FILE [, ...] join master with using file\n"
        "      e.g.  merge 1:1 country year using gdp.tea   (m:m is rejected by design)"},
    {"reshape",do_reshape,1,
        "reshape long|wide stubs , i(idvars) j(jvar)  pivot wide<->long (in-place)\n"
        "      e.g.  reshape long v, i(country) j(year)\n"
        "            takes v1980 v1981 ... v2031 -> rows of (country, year, v)"},
    {"recode",do_recode,1,
        "recode varlist (rules) [, gen(stub)]         map values\n"
        "      e.g.  recode score (1/3=1)(4/6=2)(7/10=3) (missing=.), gen(score_g)"},
    {"encode",do_encode,1,
        "encode strvar, generate(newvar) [label(name)]  map strings to integers + value label"},
    {"decode",do_decode,1,
        "decode intvar, generate(newvar)              inverse of encode (codes back to strings)"},
    {"destring",do_destring,1,
        "destring varlist, {replace|generate(stub)} [force] [ignore(\"chars\")]   str -> num\n"
        "      e.g.  destring z4, replace force   (junk values become missing)"},
    {"tostring",do_tostring,1,
        "tostring varlist, {replace|generate(stub)} [force] [format(%fmt)]      num -> str"},
    {"codebook",do_codebook,1,"codebook [varlist]                           type, uniques, missing, range"},
    {"import",do_import,0,
        "import delimited|excel|ods using FILE [, sheet(name) clear]   read CSV/TSV/XLSX/ODS\n"
        "      e.g.  import delimited \"data/WEO.csv\", clear"},
    {"export",do_export,1,
        "export delimited using FILE                  write CSV/TSV\n"
        "      e.g.  export delimited using out.csv"},
    {"save",do_save,1,
        "save FILE [, replace]                        write Stata .dta (default) or .tea\n"
        "      e.g.  save mydata.dta, replace        — emits Stata-compatible .dta\n"
        "      e.g.  save mydata.tea, replace        — native tea binary"},
    {"confirm",do_confirm,0,
        "confirm [new] file FILENAME | confirm [new|numeric|string] variable NAME\n"
        "      error (601/602/111/110/7) unless the condition holds; use with\n"
        "      capture:  capture confirm file f.dta\n"
        "                if _rc { <create it> }"},
    {"history",do_history,0,
        "history [N] | history save FILE [, replace] | history clear\n"
        "      list, export, or clear this session's interactive commands\n"
        "      (native REPL arrow-key history persists in ~/.tea_history;\n"
        "       the browser edition persists history in the browser)"},
    {"sysuse",do_sysuse,0,
        "sysuse dir | sysuse NAME [, clear]\n"
        "      load a practice dataset bundled inside the tea binary\n"
        "      e.g.  sysuse grunfeld, clear\n"
        "            xtset firm year\n"
        "            xtreg invest value capital, fe"},
    {"use",do_use,0,
        "use FILE [, clear]                           read Stata .dta, .tea, .csv, or .tsv\n"
        "      e.g.  use mydata.dta, clear            — extension dispatch decides format\n"
        "      'use foo' with no extension looks for foo.dta (Stata default)."},
    {"clear",do_clear,0,"clear                                        drop all data in current frame"},
    {"frame",do_frame,0,
        "frame create|change|copy|rename|put|drop|dir  multiple datasets\n"
        "      e.g.  frame create alt   |   frame change alt   |   frame put x y, into(alt)"},
    {"frames",do_frame,0,NULL},
    {"regress",do_regress,1,
        "regress y x1 x2 ... [if] [in] [weight] [, noconstant robust cluster(var)]   OLS\n"
        "      e.g.  regress gdp_growth investment trade if year>=2000, cluster(country)"},
    {"reg",do_regress,1,NULL},
    {"predict",do_predict,1,"predict newvar [, xb residuals]                                          predict from last fit"},
    {"test",do_test,1,"test v1 v2 ...                                                           Wald F-test (joint zero)"},
    {"lincom",do_lincom,1,"lincom <linear combo of coefs>                                          point estimate + SE of L'b"},
    {"help",do_help,0,"help [cmd]                                   list commands or show one's syntax"},
    {"pwd",do_pwd,0,"pwd                                          print working directory"},
    {"cd",do_cd,0,"cd DIR                                       change working directory"},
    {"mkdir",do_mkdir,0,"mkdir DIR [, recursive]                      create directory (recursive = mkdir -p)"},
    {"dir",do_dir,0,"dir [pattern]                                list files in current directory"},
    {"ls",do_dir,0,NULL},
    {"rmdir",do_rmdir,0,"rmdir DIR                                    remove empty directory"},
    {"erase",do_erase,0,"erase FILE  |  rm FILE                       delete a file"},
    {"rm",do_erase,0,NULL},
    {"copy",do_copy,0,"copy SRC DST [, replace]                     copy a file"},
    {"do",do_dofile,0,"do FILENAME                                  run another do-file"},
    {"version",do_version,0,"version                                      tea version & build info"},
    {"about",do_version,0,NULL},
    {"log",do_log,0,"log using FILE [, replace append] | log close   tee output to a file"},
    {"exit",do_exit,0,"exit [, clear]                               leave tea (clear drops data first)"},
    {"quit",do_exit,0,NULL},
    {NULL,NULL,0,NULL}
};

int run_command(Cmd *c){
    g_ws=c->ws;
    for(Disp *d=TABLE;d->name;d++) if(!strcmp(c->cmd,d->name)){
        if(d->needs_data && c->f->nvar==0 && strcmp(c->cmd,"set")){ }
        return d->fn(c);
    }
    if(!strcmp(c->cmd,"set")){ char w[16]; sscanf(c->args,"%15s",w);
        if(!strcmp(w,"obs")){ Cmd cc=*c; snprintf(cc.args,sizeof cc.args,"%s",c->args+3); return do_setobs(&cc);}
        if(!strcmp(w,"seed")){
            const char *p = c->args + 4; while(*p == ' ') p++;
            unsigned long seed = strtoul(p, NULL, 10);
            tea_srand(seed);
            return 0;
        }
        return 0; }
    extern int g_current_line;
    if(g_current_line) tea_err("line %d: unrecognized command: %s\n",g_current_line,c->cmd);
    else tea_err("unrecognized command: %s\n",c->cmd);
    return 199;
}

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
#include "expr.h"
#include "value.h"
#include "lex.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdint.h>

/* ============================================================ MODULE
 *
 * eval.c — expression evaluator and built-in function library.
 *
 * This module turns an AST (built by parse.c) into a value.  The two
 * main entry points are:
 *
 *   expr_eval(node, ctx)        — evaluate any AST node, return EVal
 *   call_builtin(fn, args, ctx) — dispatch a function-call node
 *
 * The EvalCtx (ec) carries everything an expression might need:
 *   - the current frame (for variable lookup)
 *   - _n and _N (current row index, total rows)
 *   - the sort/group window (for x[1], x[_N], sum())
 *   - the macro table (for `local' / $global expansion happens earlier,
 *     in interp.c — but r() and e() lookups happen here)
 *
 * Missing-value algebra is enforced uniformly: any operand that's
 * missing makes the result missing, and comparisons treat missing as
 * +infinity-with-code (so `if x > 5` includes missing — Stata's
 * intentional footgun, preserved).
 *
 * The function library (`call_builtin`) is a single long if-else
 * chain of strcmp tests.  Each branch validates argument count and
 * types, then computes.  When adding a function:
 *   1. add a branch in the appropriate section (math/string/calendar/...)
 *   2. update the inventory in the manual's Function reference appendix
 *   3. add a quick test in tests/regression/
 *
 * ==================================================================== */

/* ---- PRNG ----------------------------------------------------------
 *
 * 64-bit PCG-style RNG.  Deterministic from a seed (set via `set seed`)
 * so Monte Carlo results are reproducible across tea sessions.  The
 * default seed is fixed (12345); call `set seed N` to change it.
 *
 * Why PCG: small, fast, statistically excellent, and easy to seed.
 * One drawback: not cryptographically secure (don't use for crypto).
 */
static uint64_t g_rng_state = 12345ULL;
static uint64_t g_rng_inc   = 1ULL;

void tea_srand(unsigned long seed);   /* prototype: defined just below */

void tea_srand(unsigned long seed)
{
    g_rng_state = (uint64_t)seed;
    g_rng_inc   = (((uint64_t)seed) << 1u) | 1u;
    /* Burn one to mix */
    g_rng_state = g_rng_state * 6364136223846793005ULL + g_rng_inc;
}

static uint32_t pcg32(void)
{
    uint64_t old = g_rng_state;
    g_rng_state = old * 6364136223846793005ULL + g_rng_inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

/* Uniform[0,1) — divide by 2^32. */
static double rng_uniform(void)
{
    return (double)pcg32() / 4294967296.0;
}

/* Standard normal via Box-Muller.  We cache the second draw. */
static double rng_normal(void)
{
    static double cached = 0;
    static int has_cache = 0;
    if(has_cache){ has_cache = 0; return cached; }
    double u1, u2;
    do { u1 = rng_uniform(); } while(u1 <= 1e-300);
    u2 = rng_uniform();
    double r = sqrt(-2.0 * log(u1));
    double theta = 2.0 * M_PI * u2;
    cached = r * sin(theta);
    has_cache = 1;
    return r * cos(theta);
}


/* Submatch buffer shared between regexm (writer) and regexs (reader).
 * Stata's regexs(N) returns the N-th captured group from the most
 * recent regexm.  Index 0 is the whole match.  This is intentionally
 * not thread-safe; tea is single-threaded by design. */
char *tea_regex_submatch[20] = {0};

/* ---------- helpers ------------------------------------------------------ */
static EVal vnum(double d){ EVal v={0}; v.is_str=false; v.num=d; return v; }
static EVal vstr(const char*s){ EVal v={0}; v.is_str=true; v.str=strdup(s?s:""); return v; }
void eval_free(EVal *v){ if(v->is_str) free(v->str); v->str=NULL; }

/* running-sum accumulator nodes, keyed by the sum() AST node */
typedef struct AccNode { Node*key; double acc; struct AccNode*next; } AccNode;

/* ---------- (panel,time) -> row index for TS operators ------------------- */
typedef struct { int used; double pkey; double t; size_t row; } TsSlot;
typedef struct { TsSlot *s; size_t cap; int pnum; } TsIdx;

static unsigned long hsh(double a,double b){
    unsigned long x; double ab[2]={a,b}; unsigned long h=1469598103934665603UL;
    unsigned char *q=(unsigned char*)ab;
    for(size_t i=0;i<sizeof ab;i++){ h^=q[i]; h*=1099511628211UL; }
    (void)x; return h;
}
static double panel_key(Frame*f,size_t row){
    if(f->ts_panel<0) return 0;
    Variable*pv=&f->vars[f->ts_panel];
    if(pv->type==VT_NUM) return pv->num[row];
    /* hash string panel id into a double */
    const char*s=pv->str[row]; unsigned long h=1469598103934665603UL;
    for(;*s;s++){ h^=(unsigned char)*s; h*=1099511628211UL; }
    return (double)(h & 0x1fffffffffffffUL);
}
static TsIdx *tsidx_build(Frame*f){
    TsIdx*x=calloc(1,sizeof*x);
    x->cap=1; while(x->cap < f->nobs*2+8) x->cap<<=1;
    x->s=calloc(x->cap,sizeof*x->s);
    Variable*tv=&f->vars[f->ts_time];
    for(size_t r=0;r<f->nobs;r++){
        double pk=panel_key(f,r), tt=tv->num[r];
        if(sv_is_miss(tt)) continue;
        size_t h=hsh(pk,tt)&(x->cap-1);
        while(x->s[h].used) h=(h+1)&(x->cap-1);
        x->s[h].used=1; x->s[h].pkey=pk; x->s[h].t=tt; x->s[h].row=r;
    }
    return x;
}
static long tsidx_lookup(TsIdx*x,double pk,double t){
    size_t h=hsh(pk,t)&(x->cap-1);
    while(x->s[h].used){
        if(x->s[h].pkey==pk && x->s[h].t==t) return (long)x->s[h].row;
        h=(h+1)&(x->cap-1);
    }
    return -1;
}
void tsidx_free(void*p){ if(!p)return; TsIdx*x=p; free(x->s); free(x); }

void accs_free(void**ap){
    AccNode*p=(AccNode*)*ap;
    while(p){ AccNode*q=p->next; free(p); p=q; }
    *ap=NULL;
}

/* ---------- date suite (civil <-> serial, Stata epoch 1960-01-01) -------- */
static long days_from_civil(long y,unsigned m,unsigned d){
    y -= m <= 2;
    long era = (y>=0?y:y-399)/400;
    unsigned yoe = (unsigned)(y-era*400);
    unsigned doy = (153*(m+(m>2?-3:9))+2)/5 + d-1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return era*146097 + (long)doe - 719468; /* days since 1970-01-01 */
}
#define STATA_EPOCH 3653            /* days from 1970-01-01 to 1960-01-01 */
static long sdate_from_ymd(long y,unsigned m,unsigned d){
    return days_from_civil(y,m,d) + STATA_EPOCH;
}
static void civil_from_sdate(long sd,long*y,unsigned*m,unsigned*d){
    long z = sd - STATA_EPOCH + 719468;
    long era=(z>=0?z:z-146096)/146097;
    unsigned doe=(unsigned)(z-era*146097);
    unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    long yy=(long)yoe+era*400;
    unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    unsigned mp=(5*doy+2)/153;
    *d=doy-(153*mp+2)/5+1;
    *m=mp+(mp<10?3:-9);
    *y=yy+(*m<=2);
}

/* ---------- forward ------------------------------------------------------ */
static EVal ev(Node*n,EvalCtx*c);

static double as_num(EVal v){ return v.is_str ? SV_MISS : v.num; }

static size_t subscript_row(EvalCtx*c,long k1){       /* k1 is 1-based */
    long lo=0,hi=(long)c->f->nobs-1;
    if(c->grouped){ lo=(long)c->grp_lo; hi=(long)c->grp_hi; }
    long slot=lo+(k1-1);
    if(slot<lo||slot>hi) return (size_t)-1;
    return c->order ? c->order[slot] : (size_t)slot;
}

static EVal var_at(Frame*f,int vi,size_t row){
    Variable*v=&f->vars[vi];
    if(row==(size_t)-1) return v->type==VT_STR? vstr("") : vnum(SV_MISS);
    return v->type==VT_STR? vstr(v->str[row]) : vnum(v->num[row]);
}

/* ---------- function library -------------------------------------------- */
static EVal call_fn(Node*n,EvalCtx*c){
    /* collect args */
    EVal a[8]; int na=0;
    for(Node*p=n->a;p&&na<8;p=p->next) a[na++]=ev(p,c);
    const char*fn=n->text;
    #define A(i) (a[i].is_str?SV_MISS:a[i].num)
    #define NEED(k) if(na<(k)){ for(int q=0;q<na;q++)eval_free(&a[q]); c->err="too few args"; return vnum(SV_MISS);} 
    EVal r=vnum(SV_MISS);

    /* -------------------------------------------------------------
     * Built-in function dispatch.
     *
     * Each branch tests fn (the function name) and computes r.
     * `A(i)` returns argument i as a double (SV_MISS if string).
     * `NEED(k)` errors out if fewer than k args were given.
     * Missing-value propagation: most branches return SV_MISS if
     * any input is missing (`sv_is_miss(A(i)) ? SV_MISS : ...`).
     *
     * Sections below: arithmetic & rounding -> exp/log -> trig ->
     * distributions -> random -> missing/range tests -> running ->
     * strings -> regex -> numeric<->string -> calendar -> egen/r() refs.
     * ------------------------------------------------------------- */

    /* --- Arithmetic and rounding --- */
    if(!strcmp(fn,"abs")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:fabs(A(0)));}
    else if(!strcmp(fn,"runiform")){ r = vnum(rng_uniform()); }
    else if(!strcmp(fn,"rnormal")){
        if(na == 0)      r = vnum(rng_normal());
        else if(na == 1) r = vnum(A(0) + rng_normal());
        else             r = vnum(A(0) + A(1) * rng_normal());
    }
    else if(!strcmp(fn,"int")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:trunc(A(0)));}
    else if(!strcmp(fn,"round")){NEED(1) double u=na>1?A(1):1.0; r=vnum(sv_is_miss(A(0))?SV_MISS:round(A(0)/u)*u);}
    else if(!strcmp(fn,"floor")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:floor(A(0)));}
    else if(!strcmp(fn,"ceil")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:ceil(A(0)));}
    else if(!strcmp(fn,"sqrt")){NEED(1) r=vnum(sv_is_miss(A(0))||A(0)<0?SV_MISS:sqrt(A(0)));}
    else if(!strcmp(fn,"exp")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:exp(A(0)));}
    else if(!strcmp(fn,"ln")||!strcmp(fn,"log")){NEED(1) r=vnum(sv_is_miss(A(0))||A(0)<=0?SV_MISS:log(A(0)));}
    else if(!strcmp(fn,"log10")){NEED(1) r=vnum(sv_is_miss(A(0))||A(0)<=0?SV_MISS:log10(A(0)));}
    /* trigonometric (Stata: sin, cos, tan, asin, acos, atan, atan2) */
    else if(!strcmp(fn,"sin")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:sin(A(0)));}
    else if(!strcmp(fn,"cos")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:cos(A(0)));}
    else if(!strcmp(fn,"tan")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:tan(A(0)));}
    else if(!strcmp(fn,"asin")){NEED(1) double x=A(0); r=vnum(sv_is_miss(x)||x<-1||x>1?SV_MISS:asin(x));}
    else if(!strcmp(fn,"acos")){NEED(1) double x=A(0); r=vnum(sv_is_miss(x)||x<-1||x>1?SV_MISS:acos(x));}
    else if(!strcmp(fn,"atan")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:atan(A(0)));}
    else if(!strcmp(fn,"atan2")){NEED(2) r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))?SV_MISS:atan2(A(0),A(1)));}
    /* hyperbolic */
    else if(!strcmp(fn,"sinh")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:sinh(A(0)));}
    else if(!strcmp(fn,"cosh")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:cosh(A(0)));}
    else if(!strcmp(fn,"tanh")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:tanh(A(0)));}
    /* distribution functions (Stata names; route through stats.h/GSL) */
    else if(!strcmp(fn,"normal")){NEED(1) r=vnum(sv_is_miss(A(0))?SV_MISS:0.5*(1.0+erf(A(0)/M_SQRT2)));}
    else if(!strcmp(fn,"normalden")){NEED(1) /* φ(x), or normalden(x,μ,σ) */
        double x=A(0);
        if(sv_is_miss(x)){ r=vnum(SV_MISS); }
        else if(na==1){ r=vnum(0.3989422804014327 * exp(-0.5*x*x)); }
        else if(na==2){ /* normalden(x,σ) */
            double s=A(1);
            if(sv_is_miss(s)||s<=0) r=vnum(SV_MISS);
            else r=vnum(0.3989422804014327/s * exp(-0.5*(x/s)*(x/s)));
        }
        else { /* normalden(x,μ,σ) */
            double mu=A(1),s=A(2);
            if(sv_is_miss(mu)||sv_is_miss(s)||s<=0) r=vnum(SV_MISS);
            else { double z=(x-mu)/s; r=vnum(0.3989422804014327/s * exp(-0.5*z*z)); }
        }
    }
    else if(!strcmp(fn,"lnnormal")){NEED(1) /* log Φ(x), numerically stable for large negative x */
        extern double tea_log_normal_cdf(double);
        r=vnum(sv_is_miss(A(0))?SV_MISS:tea_log_normal_cdf(A(0)));
    }
    else if(!strcmp(fn,"lnnormalden")){NEED(1) /* log φ(x) */
        double x=A(0);
        r=vnum(sv_is_miss(x)?SV_MISS:(-0.918938533204672742 - 0.5*x*x));
    }
    else if(!strcmp(fn,"invnormal")){NEED(1) extern double tea_invnormal(double); r=vnum(sv_is_miss(A(0))?SV_MISS:tea_invnormal(A(0)));}
    else if(!strcmp(fn,"ttail")){NEED(2) extern double tea_ttail(double,double); r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))?SV_MISS:tea_ttail(A(0),A(1)));}
    else if(!strcmp(fn,"invttail")){NEED(2) extern double tea_invttail(double,double); r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))?SV_MISS:tea_invttail(A(0),A(1)));}
    else if(!strcmp(fn,"Ftail")||!strcmp(fn,"F")){NEED(3) extern double tea_pval_f(double,double,double); r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))||sv_is_miss(A(2))?SV_MISS:tea_pval_f(A(2),A(0),A(1)));}
    else if(!strcmp(fn,"chi2tail")){NEED(2) extern double tea_pval_chi2(double,double); r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))?SV_MISS:tea_pval_chi2(A(1),A(0)));}
    else if(!strcmp(fn,"chi2")){NEED(2) extern double tea_pval_chi2(double,double); double v=tea_pval_chi2(A(1),A(0)); r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))?SV_MISS:1.0-v);}
    else if(!strcmp(fn,"sign")){NEED(1) double x=A(0); r=vnum(sv_is_miss(x)?SV_MISS:(x>0)-(x<0));}
    else if(!strcmp(fn,"mod")){NEED(2) r=vnum(sv_is_miss(A(0))||sv_is_miss(A(1))||A(1)==0?SV_MISS:fmod(A(0),A(1)));}
    else if(!strcmp(fn,"max")){double m=-INFINITY;int any=0;for(int i=0;i<na;i++){double x=A(i);if(!sv_is_miss(x)){if(x>m)m=x;any=1;}}r=vnum(any?m:SV_MISS);}
    else if(!strcmp(fn,"min")){double m=INFINITY;int any=0;for(int i=0;i<na;i++){double x=A(i);if(!sv_is_miss(x)){if(x<m)m=x;any=1;}}r=vnum(any?m:SV_MISS);}
    else if(!strcmp(fn,"missing")){NEED(1) r=vnum(a[0].is_str? (a[0].str[0]==0):sv_is_miss(a[0].num));}
    else if(!strcmp(fn,"cond")){NEED(3) int t=!sv_is_miss(A(0))&&A(0)!=0; r= t? a[1]:a[2]; if(t){eval_free(&a[2]);}else{eval_free(&a[1]);} eval_free(&a[0]); for(int q=3;q<na;q++)eval_free(&a[q]); return r;}
    else if(!strcmp(fn,"inrange")){NEED(3) double x=A(0); r=vnum(!sv_is_miss(x)&&x>=A(1)&&x<=A(2));}
    else if(!strcmp(fn,"inlist")){NEED(2) int hit=0; if(a[0].is_str){for(int i=1;i<na;i++)if(a[i].is_str&&!strcmp(a[0].str,a[i].str))hit=1;}else{for(int i=1;i<na;i++)if(!a[i].is_str&&a[i].num==a[0].num)hit=1;} r=vnum(hit);}
    else if(!strcmp(fn,"sum")){ /* running sum, missing treated as 0 */
        NEED(1) double add=sv_is_miss(A(0))?0.0:A(0);
        AccNode*p=(AccNode*)c->accs; while(p&&p->key!=n)p=p->next;
        if(!p){ p=calloc(1,sizeof*p); p->key=n; p->next=(AccNode*)c->accs; c->accs=p; }
        p->acc+=add; r=vnum(p->acc);
    }
    else if(!strcmp(fn,"length")||!strcmp(fn,"strlen")){NEED(1) r=vnum(a[0].is_str?(double)strlen(a[0].str):SV_MISS);}
    else if(!strcmp(fn,"upper")||!strcmp(fn,"toupper")||!strcmp(fn,"strupper")){NEED(1) char*s=strdup(a[0].is_str?a[0].str:"");for(char*q=s;*q;q++)*q=toupper((unsigned char)*q);r=vstr(s);free(s);}
    else if(!strcmp(fn,"lower")||!strcmp(fn,"tolower")||!strcmp(fn,"strlower")){NEED(1) char*s=strdup(a[0].is_str?a[0].str:"");for(char*q=s;*q;q++)*q=tolower((unsigned char)*q);r=vstr(s);free(s);}
    else if(!strcmp(fn,"proper")||!strcmp(fn,"strproper")){NEED(1) /* Title Case: capitalize first letter of each word */
        char*s=strdup(a[0].is_str?a[0].str:""); int newword=1;
        for(char*q=s;*q;q++){
            if(isspace((unsigned char)*q)||*q=='-'||*q=='/'||*q=='\''){newword=1;}
            else if(newword){*q=toupper((unsigned char)*q);newword=0;}
            else{*q=tolower((unsigned char)*q);}
        }
        r=vstr(s);free(s);
    }
    else if(!strcmp(fn,"trim")||!strcmp(fn,"strtrim")){NEED(1) const char*s=a[0].is_str?a[0].str:""; while(*s==' '||*s=='\t')s++; const char*e=s+strlen(s); while(e>s&&(e[-1]==' '||e[-1]=='\t'))e--; char*o=strndup(s,e-s);r=vstr(o);free(o);}
    else if(!strcmp(fn,"ltrim")){NEED(1) const char*s=a[0].is_str?a[0].str:""; while(*s==' '||*s=='\t')s++; char*o=strdup(s); r=vstr(o); free(o);}
    else if(!strcmp(fn,"rtrim")){NEED(1) const char*s=a[0].is_str?a[0].str:""; const char*e=s+strlen(s); while(e>s&&(e[-1]==' '||e[-1]=='\t'))e--; char*o=strndup(s,e-s); r=vstr(o); free(o);}
    else if(!strcmp(fn,"itrim")){NEED(1) /* collapse consecutive whitespace to single space */
        const char*s=a[0].is_str?a[0].str:""; char*o=malloc(strlen(s)+1); size_t w=0; int lastws=0;
        for(;*s;s++){ if(*s==' '||*s=='\t'){ if(!lastws){o[w++]=' ';lastws=1;} } else {o[w++]=*s;lastws=0;} }
        o[w]=0; r=vstr(o); free(o);
    }
    else if(!strcmp(fn,"strreverse")){NEED(1) char*s=strdup(a[0].is_str?a[0].str:""); size_t L=strlen(s);
        for(size_t i=0;i<L/2;i++){char t=s[i];s[i]=s[L-1-i];s[L-1-i]=t;} r=vstr(s); free(s);}
    else if(!strcmp(fn,"strpos")){NEED(2) if(a[0].is_str&&a[1].is_str){char*p=strstr(a[0].str,a[1].str);r=vnum(p?(double)(p-a[0].str+1):0);}}
    else if(!strcmp(fn,"strrpos")){NEED(2) /* last occurrence, 1-indexed */
        if(a[0].is_str&&a[1].is_str){
            const char*hay=a[0].str,*ndl=a[1].str; size_t nl=strlen(ndl);
            if(nl==0) r=vnum((double)(strlen(hay)+1));
            else { const char*last=NULL,*p=hay;
                while((p=strstr(p,ndl))){ last=p; p++; }
                r=vnum(last?(double)(last-hay+1):0);
            }
        }
    }
    else if(!strcmp(fn,"substr")){NEED(3) const char*s=a[0].is_str?a[0].str:""; long L=(long)strlen(s); long st=(long)A(1),ln=(long)A(2); if(st<0)st=L+st+1; long b=st-1; if(b<0)b=0; if(b>L)b=L; if(ln<0||b+ln>L)ln=L-b; char*o=strndup(s+b,ln);r=vstr(o);free(o);}
    else if(!strcmp(fn,"subinstr")){NEED(3) /* subinstr(s,from,to[,n]) */
        const char*s=a[0].is_str?a[0].str:"",*from=a[1].is_str?a[1].str:"",*to=a[2].is_str?a[2].str:"";
        long lim=na>3? (long)A(3) : -1; size_t fl=strlen(from);
        char*out=calloc(1,1); size_t ol=0; const char*p=s; long cnt=0;
        while(*p){ if(fl&&!strncmp(p,from,fl)&&(lim<0||cnt<lim)){ size_t tl=strlen(to); out=realloc(out,ol+tl+1); memcpy(out+ol,to,tl); ol+=tl; out[ol]=0; p+=fl; cnt++; } else { out=realloc(out,ol+2); out[ol++]=*p++; out[ol]=0; } }
        r=vstr(out); free(out);
    }
    else if(!strcmp(fn,"subinword")){NEED(3) /* like subinstr but only on word boundaries */
        const char*s=a[0].is_str?a[0].str:"",*from=a[1].is_str?a[1].str:"",*to=a[2].is_str?a[2].str:"";
        long lim=na>3? (long)A(3) : -1; size_t fl=strlen(from);
        char*out=calloc(1,1); size_t ol=0; const char*p=s; long cnt=0;
        while(*p){
            /* careful: only safe to read p[fl] AFTER we know strncmp matched
             * (which means p has at least fl bytes of content before NUL).
             * After that, p[fl] is either another char or the NUL terminator. */
            int at_word_start = (p==s) || (!isalnum((unsigned char)p[-1]) && p[-1]!='_');
            int matched = fl && at_word_start && !strncmp(p,from,fl);
            int at_word_end = matched && (!isalnum((unsigned char)p[fl]) && p[fl]!='_');
            if(matched && at_word_end && (lim<0||cnt<lim)){
                size_t tl=strlen(to); out=realloc(out,ol+tl+1); memcpy(out+ol,to,tl); ol+=tl; out[ol]=0; p+=fl; cnt++;
            } else {
                out=realloc(out,ol+2); out[ol++]=*p++; out[ol]=0;
            }
        }
        r=vstr(out); free(out);
    }
    else if(!strcmp(fn,"word")){NEED(2) /* nth word (1-indexed), -1 = last */
        const char*s=a[0].is_str?a[0].str:""; long cnt=(long)A(1);
        if(cnt==0){ r=vstr(""); }
        else {
            /* tokenize on whitespace, count words, return the n-th */
            const char*words[1024]; int wlens[1024]; int nw=0;
            const char*p=s;
            while(*p && nw<1024){
                while(*p&&isspace((unsigned char)*p))p++;
                if(!*p)break;
                const char*st=p;
                while(*p&&!isspace((unsigned char)*p))p++;
                words[nw]=st; wlens[nw]=(int)(p-st); nw++;
            }
            long idx = cnt<0 ? nw+cnt : cnt-1;
            if(idx<0||idx>=nw){ r=vstr(""); }
            else { char*o=strndup(words[idx],wlens[idx]); r=vstr(o); free(o); }
        }
    }
    else if(!strcmp(fn,"wordcount")){NEED(1) const char*s=a[0].is_str?a[0].str:""; int nw=0; int inword=0;
        for(;*s;s++){ if(isspace((unsigned char)*s)){inword=0;} else if(!inword){nw++;inword=1;} }
        r=vnum((double)nw);
    }
    else if(!strcmp(fn,"abbrev")){NEED(2) /* truncate to n chars, mark with ~ if shortened */
        const char*s=a[0].is_str?a[0].str:""; long cnt=(long)A(1); long L=(long)strlen(s);
        if(cnt<=0||L<=cnt){r=vstr(s);} else { char*o=malloc(cnt+1); memcpy(o,s,cnt-1); o[cnt-1]='~'; o[cnt]=0; r=vstr(o); free(o); }
    }
    else if(!strcmp(fn,"strdup")){NEED(2) /* concat n copies */
        const char*s=a[0].is_str?a[0].str:""; long cnt=(long)A(1); if(cnt<0)cnt=0;
        size_t L=strlen(s); char*o=malloc(L*(size_t)cnt+1);
        for(long i=0;i<cnt;i++) memcpy(o+i*L,s,L); o[L*cnt]=0; r=vstr(o); free(o);
    }
    else if(!strcmp(fn,"char")){NEED(1) /* codepoint -> single-char string (ASCII only here) */
        long code=(long)A(0); char buf[2]; if(code>=0&&code<=255){buf[0]=(char)code;buf[1]=0;r=vstr(buf);} else r=vstr("");
    }
    else if(!strcmp(fn,"string")||!strcmp(fn,"strofreal")){NEED(1)
        char buf[128]; double x=A(0);
        if(sv_is_miss(x)){ strcpy(buf,"."); }
        else if(na>=2 && a[1].is_str){
            /* Stata format spec: route to printf-compatible cases */
            const char*f=a[1].str;
            /* simple translate of Stata format → printf */
            if(strstr(f,"f")||strstr(f,"e")||strstr(f,"g")) snprintf(buf,sizeof buf,f,x);
            else if(strstr(f,"d")) snprintf(buf,sizeof buf,f,(long)x);
            else snprintf(buf,sizeof buf,"%g",x);
        }
        else if(x==floor(x)&&fabs(x)<1e15) snprintf(buf,sizeof buf,"%.0f",x);
        else snprintf(buf,sizeof buf,"%g",x);
        r=vstr(buf);
    }
    else if(!strcmp(fn,"real")){NEED(1) if(a[0].is_str){char*e;double x=strtod(a[0].str,&e); r=vnum(e==a[0].str?SV_MISS:x);}}
    else if(!strcmp(fn,"strmatch")){NEED(2) /* glob with * and ? */
        const char*s=a[0].is_str?a[0].str:"",*pat=a[1].is_str?a[1].str:"";
        const char*str=s,*p=pat,*star=NULL,*ss=NULL;
        while(*str){
            if(*p=='?'||(*p&&*p==*str)){ str++; p++; }
            else if(*p=='*'){ star=p++; ss=str; }
            else if(star){ p=star+1; str=++ss; }
            else break;
        }
        while(*p=='*') p++;
        r=vnum((*str==0 && *p==0) ? 1 : 0);
    }
    else if(!strcmp(fn,"regexm")){NEED(2) /* match regex; populate static
         * submatch buffer for subsequent regexs(N) calls.  Stata's behaviour:
         * regexs(0) is the whole match; regexs(N>=1) is the N-th capture. */
        extern char *tea_regex_submatch[20];   /* index 0..19, NULL if absent */
        for(int q=0;q<20;q++){ free(tea_regex_submatch[q]); tea_regex_submatch[q]=NULL; }
        regex_t re;
        if(a[0].is_str && a[1].is_str && !regcomp(&re, a[1].str, REG_EXTENDED)){
            regmatch_t m[20];
            int rc = regexec(&re, a[0].str, 20, m, 0);
            r = vnum(rc == 0 ? 1 : 0);
            if(rc == 0){
                for(int q=0; q<20; q++){
                    if(m[q].rm_so < 0) continue;
                    size_t L = (size_t)(m[q].rm_eo - m[q].rm_so);
                    tea_regex_submatch[q] = strndup(a[0].str + m[q].rm_so, L);
                }
            }
            regfree(&re);
        } else r=vnum(0);
    }
    else if(!strcmp(fn,"regexs")){NEED(1) /* extract n-th submatch from last regexm */
        extern char *tea_regex_submatch[20];
        long nidx = (long)A(0);
        if(nidx < 0 || nidx >= 20 || !tea_regex_submatch[nidx]) r = vstr("");
        else r = vstr(tea_regex_submatch[nidx]);
    }
    else if(!strcmp(fn,"regexr")){NEED(3) /* substring replace using regex */
        regex_t re;
        const char *s   = a[0].is_str ? a[0].str : "";
        const char *pat = a[1].is_str ? a[1].str : "";
        const char *rep = a[2].is_str ? a[2].str : "";
        if(!regcomp(&re, pat, REG_EXTENDED)){
            regmatch_t m[1];
            if(regexec(&re, s, 1, m, 0) == 0){
                size_t pre = (size_t)m[0].rm_so;
                size_t post_start = (size_t)m[0].rm_eo;
                size_t slen = strlen(s); size_t rlen = strlen(rep);
                char *out = malloc(pre + rlen + (slen - post_start) + 1);
                memcpy(out, s, pre);
                memcpy(out + pre, rep, rlen);
                memcpy(out + pre + rlen, s + post_start, slen - post_start);
                out[pre + rlen + slen - post_start] = 0;
                r = vstr(out); free(out);
            } else r = vstr(s);
            regfree(&re);
        } else r = vstr(s);
    }
    /* ---- date constructors ---- */
    else if(!strcmp(fn,"mdy")){NEED(3) r=vnum((double)sdate_from_ymd((long)A(2),(unsigned)A(0),(unsigned)A(1)));}
    else if(!strcmp(fn,"td")){NEED(1) /* td(01jan2020) | td(2020-01-15) */
        const char *s=a[0].is_str?a[0].str:""; int dd=0,yy=0; unsigned mm=0;
        static const char *M="janfebmaraprmayjunjulaugsepoctnovdec";
        if(strchr(s,'-')){ sscanf(s,"%d-%u-%d",&yy,&mm,&dd); }
        else { char mon[4]={0}; sscanf(s,"%d%3[a-zA-Z]%d",&dd,mon,&yy);
            for(char *q=mon;*q;q++)*q=tolower((unsigned char)*q);
            const char *f=strstr(M,mon); mm = f? (unsigned)((f-M)/3+1):1; }
        r=vnum((double)sdate_from_ymd(yy,mm?mm:1,(unsigned)(dd?dd:1)));}
    else if(!strcmp(fn,"tm")){NEED(1) int y=0,m=1; sscanf(a[0].is_str?a[0].str:"","%dm%d",&y,&m); r=vnum((double)((y-1960)*12+(m-1)));}
    else if(!strcmp(fn,"tq")){NEED(1) int y=0,q=1; sscanf(a[0].is_str?a[0].str:"","%dq%d",&y,&q); r=vnum((double)((y-1960)*4+(q-1)));}
    else if(!strcmp(fn,"tw")){NEED(1) int y=0,w=1; sscanf(a[0].is_str?a[0].str:"","%dw%d",&y,&w); r=vnum((double)((y-1960)*52+(w-1)));}
    else if(!strcmp(fn,"th")){NEED(1) int y=0,h=1; sscanf(a[0].is_str?a[0].str:"","%dh%d",&y,&h); r=vnum((double)((y-1960)*2+(h-1)));}
    else if(!strcmp(fn,"ty")){NEED(1) int y=0; sscanf(a[0].is_str?a[0].str:"","%d",&y); r=vnum((double)y);}
    else if(!strcmp(fn,"ym")){NEED(2) r=vnum(((long)A(0)-1960)*12 + ((long)A(1)-1));}
    else if(!strcmp(fn,"yq")){NEED(2) r=vnum(((long)A(0)-1960)*4 + ((long)A(1)-1));}
    else if(!strcmp(fn,"yh")){NEED(2) r=vnum(((long)A(0)-1960)*2 + ((long)A(1)-1));}
    else if(!strcmp(fn,"yw")){NEED(2) r=vnum(((long)A(0)-1960)*52 + ((long)A(1)-1));}
    else if(!strcmp(fn,"yofd")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)y);}
    else if(!strcmp(fn,"year")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)y);}
    else if(!strcmp(fn,"month")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)m);}
    else if(!strcmp(fn,"day")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)d);}
    else if(!strcmp(fn,"dow")){NEED(1) long sd=(long)A(0); long wd=((sd%7)+5)%7; if(wd<0)wd+=7; r=vnum((double)wd);}
    else if(!strcmp(fn,"quarter")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((m-1)/3+1));}
    else if(!strcmp(fn,"halfyear")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((m-1)/6+1));}
    else if(!strcmp(fn,"doy")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((long)A(0)-sdate_from_ymd(y,1,1)+1));}
    else if(!strcmp(fn,"week")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);long doy=(long)A(0)-sdate_from_ymd(y,1,1);r=vnum((double)(doy/7+1));}
    /* converters: serial-of-X */
    else if(!strcmp(fn,"mofd")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((y-1960)*12+(m-1)));}
    else if(!strcmp(fn,"qofd")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((y-1960)*4+((m-1)/3)));}
    else if(!strcmp(fn,"hofd")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);r=vnum((double)((y-1960)*2+((m-1)/6)));}
    else if(!strcmp(fn,"wofd")){NEED(1) long y;unsigned m,d;civil_from_sdate((long)A(0),&y,&m,&d);long doy=(long)A(0)-sdate_from_ymd(y,1,1);r=vnum((double)((y-1960)*52+doy/7));}
    else if(!strcmp(fn,"dofm")){NEED(1) long mm=(long)A(0);long y=1960+mm/12;long m=mm%12; if(m<0){m+=12;y--;} r=vnum((double)sdate_from_ymd(y,(unsigned)(m+1),1));}
    else if(!strcmp(fn,"dofq")){NEED(1) long qq=(long)A(0);long y=1960+qq/4;long q=qq%4; if(q<0){q+=4;y--;} r=vnum((double)sdate_from_ymd(y,(unsigned)(q*3+1),1));}
    else if(!strcmp(fn,"dofh")){NEED(1) long hh=(long)A(0);long y=1960+hh/2;long h=hh%2; if(h<0){h+=2;y--;} r=vnum((double)sdate_from_ymd(y,(unsigned)(h*6+1),1));}
    else if(!strcmp(fn,"dofy")){NEED(1) r=vnum((double)sdate_from_ymd((long)A(0),1,1));}
    else if(!strcmp(fn,"date")){NEED(2) /* date(s,"DMY"|"MDY"|"YMD") */
        if(a[0].is_str&&a[1].is_str){ int nums[3],ni=0; const char*s=a[0].str; while(*s&&ni<3){ if(isdigit((unsigned char)*s)){ nums[ni++]=(int)strtol(s,(char**)&s,10);} else s++; }
            if(ni>=3){ const char*o=a[1].str; int y=0,m=0,d=0; for(int i=0;i<3;i++){ char ch=toupper((unsigned char)o[i]); if(ch=='Y')y=nums[i]; else if(ch=='M')m=nums[i]; else d=nums[i]; } if(y<100)y+=2000; r=vnum((double)sdate_from_ymd(y,(unsigned)m,(unsigned)d)); } }
    }
    else { c->err="unknown function"; }

    for(int q=0;q<na;q++) eval_free(&a[q]);
    #undef A
    #undef NEED
    return r;
}

/* ---------- core eval ---------------------------------------------------- */
static EVal ev(Node*n,EvalCtx*c){
    switch(n->kind){
    case N_NUM: return vnum(n->num);
    case N_STR: return vstr(n->text);
    case N_VAR: {
        if(!strcmp(n->text,"_n")) return vnum((double)c->n);
        if(!strcmp(n->text,"_N")) return vnum((double)c->N);
        if(!strcmp(n->text,"_pi")) return vnum(M_PI);
        int vi=var_find(c->f,n->text);
        if(vi<0){ c->err="variable not found"; return vnum(SV_MISS); }
        if(n->a){ EVal k=ev(n->a,c); long ki=(long)as_num(k); eval_free(&k);
            return var_at(c->f,vi,subscript_row(c,ki)); }
        return var_at(c->f,vi,c->i);
    }
    case N_TSOP: {
        if(c->f->ts_time<0){ c->err="data not tsset"; return vnum(SV_MISS); }
        int vi=var_find(c->f,n->text);
        if(vi<0){ c->err="variable not found"; return vnum(SV_MISS); }
        if(!c->tsidx) c->tsidx=tsidx_build(c->f);
        TsIdx*x=c->tsidx;
        Variable*tv=&c->f->vars[c->f->ts_time];
        double pk=panel_key(c->f,c->i), t=tv->num[c->i];
        if(sv_is_miss(t)) return vnum(SV_MISS);
        double dl=c->f->ts_delta;
        if(n->tskind=='L'||n->tskind=='F'){
            long row=tsidx_lookup(x,pk,t + n->tslag*dl);  /* L2->-2,F1->+1 */
            return var_at(c->f,vi,row<0?(size_t)-1:(size_t)row);
        }
        if(n->tskind=='S'){
            /* S#.x = x[t] - x[t-#]: simple gap-aware seasonal difference. */
            long back = n->tslag;                            /* negative */
            long rprev=tsidx_lookup(x,pk,t + back*dl);
            long rcur =tsidx_lookup(x,pk,t);
            EVal cur=var_at(c->f,vi,rcur<0?(size_t)-1:(size_t)rcur);
            EVal prv=var_at(c->f,vi,rprev<0?(size_t)-1:(size_t)rprev);
            double res=sv_sub(as_num(cur),as_num(prv));
            eval_free(&cur);eval_free(&prv);
            return vnum(res);
        }
        /* D#.x = iterated #-th difference (Stata convention):
         *   D.x  = x[t] - x[t-1]
         *   D2.x = D.(D.x) = x[t] - 2*x[t-1] + x[t-2]
         *   D3.x = x[t] - 3*x[t-1] + 3*x[t-2] - x[t-3]
         *   D^k x[t] = sum_{j=0..k} (-1)^j * C(k,j) * x[t-j]
         * Any missing value in the lag window propagates to missing. */
        int k = -n->tslag;                /* tslag is negative; k = order */
        if(k <= 0) return vnum(SV_MISS);
        double sum = 0.0;
        long binom = 1;                   /* C(k,0) = 1 */
        int sign = 1;
        for(int j = 0; j <= k; j++){
            long row = tsidx_lookup(x, pk, t + (double)(-j) * dl);
            if(row < 0) return vnum(SV_MISS);
            EVal v = var_at(c->f, vi, (size_t)row);
            double xv = as_num(v);
            eval_free(&v);
            if(sv_is_miss(xv)) return vnum(SV_MISS);
            sum += (double)(sign * binom) * xv;
            /* recurrence: C(k, j+1) = C(k, j) * (k - j) / (j + 1) */
            if(j < k) binom = binom * (k - j) / (j + 1);
            sign = -sign;
        }
        return vnum(sum);
    }
    case N_UNARY: {
        EVal x=ev(n->a,c);
        double d=as_num(x); eval_free(&x);
        if(n->op==T_MINUS) return vnum(sv_neg(d));
        return vnum(sv_is_miss(d)?SV_MISS:(d==0?1:0));
    }
    case N_BINARY: {
        EVal L=ev(n->a,c), R=ev(n->b,c);
        EVal r=vnum(SV_MISS);
        if(n->op==T_PLUS && L.is_str && R.is_str){
            size_t la=strlen(L.str),lb=strlen(R.str);
            char*o=malloc(la+lb+1); memcpy(o,L.str,la); memcpy(o+la,R.str,lb+1);
            r=vstr(o); free(o);
        } else if(n->op==T_EQEQ||n->op==T_NE){
            int eq;
            if(L.is_str||R.is_str) eq=(L.is_str&&R.is_str)? strcmp(L.str,R.str)==0 : 0;
            else eq = sv_cmp(L.num,R.num)==0;
            r=vnum(n->op==T_EQEQ? eq : !eq);
        } else if(n->op==T_LT||n->op==T_LE||n->op==T_GT||n->op==T_GE){
            double cmp;
            if(L.is_str&&R.is_str) cmp=strcmp(L.str,R.str);
            else cmp=sv_cmp(as_num(L),as_num(R));
            int res = n->op==T_LT? cmp<0 : n->op==T_LE? cmp<=0 :
                      n->op==T_GT? cmp>0 : cmp>=0;
            r=vnum(res);
        } else if(n->op==T_AND||n->op==T_OR){
            double a=as_num(L),b=as_num(R);
            int av=!sv_is_miss(a)&&a!=0, bv=!sv_is_miss(b)&&b!=0;
            r=vnum(n->op==T_AND? (av&&bv):(av||bv));
        } else {
            double a=as_num(L),b=as_num(R),v;
            switch(n->op){
                case T_PLUS:  v=sv_add(a,b); break;
                case T_MINUS: v=sv_sub(a,b); break;
                case T_STAR:  v=sv_mul(a,b); break;
                case T_SLASH: v=sv_div(a,b); break;
                case T_CARET: v=(sv_is_miss(a)||sv_is_miss(b))?SV_MISS:pow(a,b); break;
                default: v=SV_MISS;
            }
            r=vnum(v);
        }
        eval_free(&L); eval_free(&R);
        return r;
    }
    case N_CALL: return call_fn(n,c);
    }
    return vnum(SV_MISS);
}

EVal expr_eval(Node*n,EvalCtx*c){ c->err=NULL; return ev(n,c); }

bool expr_eval_bool(Node*n,EvalCtx*c){
    EVal v=expr_eval(n,c);
    bool t = v.is_str ? (v.str[0]!=0) : (!sv_is_miss(v.num)&&v.num!=0);
    eval_free(&v);
    return t;
}

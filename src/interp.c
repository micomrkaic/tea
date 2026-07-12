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
#include "interp.h"
#include "cmd.h"
#include "expr.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

Interp *g_tea_interp = NULL;   /* bridge for commands needing the interp (history) */
#include "estimates.h"
Estimates *tea_last_estimates(void){
    return g_tea_interp ? g_tea_interp->ws->last_est : NULL;
}
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>

Interp *interp_new(Workspace *ws){
    Interp *ip=calloc(1,sizeof*ip);
    ip->ws=ws;
    ip->strict_stata = true;  /* tea is strict-Stata by default */
    g_tea_interp = ip;
    return ip;
}

/* set by 'exit' command, polled by run_stream and main */
int g_exit_requested = 0;
int g_exit_code      = 0;
int g_current_line   = 0;     /* do-file line number, 0 = REPL/unknown */

/* defined in commands.c — echo a command to the open log, if any */
extern void tea_log_command(const char *line);
static void free_tbl(MacroKV *m){ while(m){ MacroKV*n=m->next; free(m->name);free(m->val);free(m); m=n; } }
void interp_free(Interp *ip){ if(!ip)return; free_tbl(ip->locals);free_tbl(ip->globals);free_tbl(ip->rret); free(ip); }

void mac_set(MacroKV **tbl,const char *name,const char *val){
    for(MacroKV*m=*tbl;m;m=m->next) if(!strcmp(m->name,name)){ free(m->val); m->val=strdup(val); return; }
    MacroKV*m=calloc(1,sizeof*m); m->name=strdup(name); m->val=strdup(val); m->next=*tbl; *tbl=m;
}
const char *mac_get(MacroKV *tbl,const char *name){
    for(MacroKV*m=tbl;m;m=m->next) if(!strcmp(m->name,name)) return m->val;
    return NULL;
}
void mac_clear(MacroKV **tbl){ free_tbl(*tbl); *tbl=NULL; }

/* evaluate a constant/scalar expression (may reference r() via pre-expand). */
static int eval_scalar(Interp *ip,const char *e,char *out,size_t n){
    /* Use the active frame so expressions can reference _N, _n, and
     * variables.  Fall back to an empty scratch frame only when there's
     * no workspace (shouldn't happen in practice, but defensive). */
    Frame scratch; Frame *f;
    if(ip && ip->ws && ip->ws->cur){
        f = ip->ws->cur;
    } else {
        memset(&scratch,0,sizeof scratch); scratch.ts_panel=scratch.ts_time=-1;
        f = &scratch;
    }
    const char *perr; Node *a=expr_parse(e,f,&perr);
    if(!a){ snprintf(out,n,"%s",e); return -1; }
    EvalCtx ec={0}; ec.f=f;
    /* In display/macro context there is no current row; set _n=1 (Stata
     * convention) and _N to the actual frame size. */
    ec.n = 1;
    ec.N = (long)f->nobs;
    ec.i = 0;
    EVal v=expr_eval(a,&ec); node_free(a);
    if(v.is_str) snprintf(out,n,"%s",v.str);
    else if(sv_is_miss(v.num)) snprintf(out,n,".");
    else if(v.num==(long long)v.num) snprintf(out,n,"%lld",(long long)v.num);
    else snprintf(out,n,"%.10g",v.num);
    eval_free(&v); return 0;
}

/* macro expansion: `local' , $glob/${glob}, r(name)/e(name), `=expr'
 *
 * Stata's rules for what gets substituted inside double-quoted strings:
 *   - `local'   : YES (macro reference is always substituted)
 *   - $global   : YES
 *   - e(name)   : NO  — Stata treats this as a stored-result *function*,
 *                 not a macro.  Inside quotes it's literal text.
 *                 To embed e(N) in a string, write "...`=e(N)'..."
 *   - r(name)   : NO (same logic)
 * So we track whether we're inside a double-quoted string and skip
 * the e()/r() form there.  */
char *macro_expand(Interp *ip,const char *line){
    size_t cap=strlen(line)*2+64,len=0; char *out=malloc(cap);
    #define PUT(s) do{ const char*_s=(s); size_t _l=strlen(_s); \
        while(len+_l+1>cap){cap*=2;out=realloc(out,cap);} memcpy(out+len,_s,_l); len+=_l; out[len]=0; }while(0)
    bool in_dquote = false;
    for(const char *p=line;*p;){
        if(*p == '"'){ in_dquote = !in_dquote; char c2[2]={*p,0}; PUT(c2); p++; continue; }
        if(*p=='`'){
            const char *q=p+1; int d=1; while(*q&&d){ if(*q=='`')d++; else if(*q=='\'')d--; if(d)q++; }
            char inner[1024]; snprintf(inner,sizeof inner,"%.*s",(int)(q-p-1),p+1);
            char *ix=macro_expand(ip,inner);   /* nested */
            if(ix[0]=='='){ char vb[256]; eval_scalar(ip,ix+1,vb,sizeof vb); PUT(vb); }
            else { const char *v=mac_get(ip->locals,ix); PUT(v?v:""); }
            free(ix); p=(*q?q+1:q);
        } else if(*p=='$'){
            /* Stata global syntax: $name or ${name}, where name is a Stata
             * identifier (letter or underscore first, then alphanumerics/
             * underscores).  $1, $2, etc are NOT macro references — the $
             * stays literal (this matters for currency strings like
             * "$1,234.56"). */
            const char *save=p; p++;
            int brace=0; if(*p=='{'){brace=1;p++;}
            char nm[128]; int n=0;
            if(*p=='_' || isalpha((unsigned char)*p)){
                while(*p && (isalnum((unsigned char)*p)||*p=='_') && n<127) nm[n++]=*p++;
            }
            nm[n]=0;
            if(n == 0){
                p = save + 1;
                char c2[2]={'$',0}; PUT(c2);
            } else {
                if(brace && *p=='}') p++;
                const char *v=mac_get(ip->globals,nm); PUT(v?v:"");
            }
        } else if(!in_dquote && (p[0]=='r'||p[0]=='e')&&p[1]=='('&&strchr(p,')')){
            const char *e=strchr(p,')'); char key[64];
            snprintf(key,sizeof key,"%.*s",(int)(e-p+1),p);
            const char *v=mac_get(ip->rret,key);
            if(v){ PUT(v); p=e+1; } else { char c2[2]={*p,0}; PUT(c2); p++; }
        } else if(!in_dquote && p[0]=='_'
                  && (p[1]=='b' || (p[1]=='s' && p[2]=='e'))
                  && (p[1]=='b' ? p[2]=='[' : p[3]=='[')){
            /* _b[name] or _se[name]: post-estimation coefficient/SE access.
             * The estimators store these in c->ip->rret keyed by exactly
             * "_b[varname]" / "_se[varname]" so we just look them up.
             * The bracketed name can contain dots (e.g. L.growth) and
             * crosses (factor-variable interactions). */
            const char *lb = strchr(p, '[');
            const char *rb = lb ? strchr(lb, ']') : NULL;
            if(lb && rb){
                char key[96];
                snprintf(key, sizeof key, "%.*s", (int)(rb - p + 1), p);
                const char *v = mac_get(ip->rret, key);
                if(v){ PUT(v); p = rb + 1; }
                else  { PUT("."); p = rb + 1; }   /* missing if absent */
            } else { char c2[2]={*p,0}; PUT(c2); p++; }
        } else { char c2[2]={*p,0}; PUT(c2); p++; }
    }
    #undef PUT
    return out;
}

/* ---- single line execution -------------------------------------------- */
/* ---- capture support ----------------------------------------------------
 * Stata's capture suppresses ALL output of the captured command (stdout
 * and stderr) and records the return code in _rc.  We mute at the file-
 * descriptor level so every printf/fprintf in every command is covered,
 * and mirror ip->rc into g_tea_last_rc for the expression evaluator. */
int g_tea_last_rc = 0;
static void mute_begin(int sv[2]){
    fflush(stdout); fflush(stderr);
    sv[0]=dup(1); sv[1]=dup(2);
    int nul=open("/dev/null",O_WRONLY);
    if(nul>=0){ dup2(nul,1); dup2(nul,2); close(nul); }
}
static void mute_end(int sv[2]){
    fflush(stdout); fflush(stderr);
    if(sv[0]>=0){ dup2(sv[0],1); close(sv[0]); }
    if(sv[1]>=0){ dup2(sv[1],2); close(sv[1]); }
}

int run_line(Interp *ip,const char *raw){
    char line[8192]; snprintf(line,sizeof line,"%s",raw);
    char *s=line; while(*s==' '||*s=='\t')s++;
    if(!*s) return 0;

    /* '!' at start of line is the shell escape — handle before everything else */
    if(*s=='!'){
        const char *cmd=s+1; while(*cmd==' ')cmd++;
        if(!*cmd) return 0;
        const char *sh=getenv("SHELL"); if(!sh||!sh[0]) sh="/bin/sh";
        fflush(stdout);
#ifdef __EMSCRIPTEN__
        /* no fork() in WASM; Emscripten implements system() (node: spawnSync,
         * browser: fails cleanly with ENOSYS) */
        { int st = system(cmd); ip->rc = st < 0 ? 128 : ((st >> 8) & 0xff); }
#else
        pid_t pid=fork();
        if(pid==0){ execl(sh,sh,"-c",cmd,(char*)NULL); _exit(127); }
        int st=0; waitpid(pid,&st,0);
        ip->rc = WIFEXITED(st)? WEXITSTATUS(st) : 128;
#endif
        return 0;
    }

    int cap=0; bool quiet=false;
    /* prefixes: capture / quietly / noisily (repeatable) */
    for(;;){
        if(!strncmp(s,"capture ",8)||!strncmp(s,"cap ",4)){ cap=1; s+=(s[3]==' '?4:8); }
        else if(!strncmp(s,"quietly ",8)||!strncmp(s,"qui ",4)){ quiet=1; s+=(s[3]==' '?4:8); }
        else if(!strncmp(s,"noisily ",8)||!strncmp(s,"noi ",4)){ s+=(s[3]==' '?4:8); }
        else break;
        while(*s==' ')s++;
    }
    int _mfd[2] = {-1,-1};
    if(cap){ mute_begin(_mfd); g_tea_last_rc = 0; }  /* success leaves _rc==0 */
    #define RL_DONE() do{ if(cap) mute_end(_mfd); }while(0)

    /* by / bysort prefix:  by[sort] groupvars [(sortvars)] : cmd */
    int *byv=NULL,nby=0; int *sortk=NULL,nsk=0; bool bysort=false;
    if(!strncmp(s,"by ",3)||!strncmp(s,"bysort ",7)||!strncmp(s,"bys ",4)){
        bysort = (s[2]!=' ');
        char *colon=strchr(s,':');
        if(colon){
            char *vp=strchr(s,' ')+1; char vspec[512];
            snprintf(vspec,sizeof vspec,"%.*s",(int)(colon-vp),vp);
            char *xp=macro_expand(ip,vspec);
            char grp[512]={0},srt[512]={0};
            char *lp=strchr(xp,'(');
            if(lp){ char *rp=strchr(lp,')');
                snprintf(grp,sizeof grp,"%.*s",(int)(lp-xp),xp);
                if(rp) snprintf(srt,sizeof srt,"%.*s",(int)(rp-lp-1),lp+1);
            } else snprintf(grp,sizeof grp,"%s",xp);
            free(xp);
            nby=varlist_expand(ip->ws->cur,grp,&byv);
            /* sort keys = group vars then within-sort vars */
            char allk[1024]; snprintf(allk,sizeof allk,"%s %s",grp,srt);
            nsk=varlist_expand(ip->ws->cur,allk,&sortk);
            /* SILENT NO-OP GUARD: -1 (unknown var) must not degrade into
             * "no by-groups" — the command would run UNGROUPED over the
             * full data and produce silently wrong results. */
            if(nby<0 || nsk<0){
                fprintf(stderr,"by: variable not found\n");
                free(byv); free(sortk); RL_DONE();
                g_tea_last_rc=ip->rc=111;
                return cap?0:111;
            }
            s=colon+1; while(*s==' ')s++;
        }
    }

    char *ex=macro_expand(ip,s);
    char *t=ex; while(*t==' ')t++;
    if(!*t){ free(ex); free(byv); free(sortk); RL_DONE(); return 0; }

    /* interpreter-native statements */

    /* 'assert exp [if] [in]' — fail loud if exp is false anywhere selected */
    if(!strncmp(t,"assert ",7) || !strcmp(t,"assert")){
        const char *e = t+6; while(*e==' ') e++;
        if(!*e){ fprintf(stderr,"assert: expression required\n"); free(ex);free(byv);free(sortk); RL_DONE(); g_tea_last_rc=ip->rc=198; return cap?0:198; }
        /* split out optional 'if'/'in' so they constrain assertion scope */
        char esep[2048]; snprintf(esep,sizeof esep,"%s",e);
        Frame *f = ip->ws->cur;
        Node *ifn=NULL; const char *perr;
        char *ifp = strstr(esep," if ");
        if(ifp){ *ifp=0; ifn = expr_parse(ifp+4, f, &perr); }
        Node *en = expr_parse(esep, f, &perr);
        if(!en){ fprintf(stderr,"assert: %s\n", perr?perr:"parse error"); free(ex);free(byv);free(sortk); RL_DONE(); g_tea_last_rc=ip->rc=198; return cap?0:198; }
        EvalCtx ec={0}; ec.f=f;
        long bad=0, ok=0, miss=0;
        size_t N = f->nobs ? f->nobs : 1;
        for(size_t i=0;i<N;i++){
            if(ifn){ ec.i=i;ec.n=(long)i+1;ec.N=(long)N; if(!expr_eval_bool(ifn,&ec)){ continue; } }
            ec.i=i; ec.n=(long)i+1; ec.N=(long)N;
            EVal v=expr_eval(en,&ec);
            if(v.is_str){ if(v.str[0]) ok++; else bad++; }
            else if(sv_is_miss(v.num)) miss++;
            else if(v.num!=0) ok++;
            else bad++;
            eval_free(&v);
        }
        node_free(en); node_free(ifn);
        if(bad){
            fprintf(stderr,"assertion is false (%ld failed, %ld true, %ld missing)\n",bad,ok,miss);
            g_tea_last_rc=ip->rc=9; free(ex);free(byv);free(sortk); RL_DONE(); return cap?0:9;
        }
        if(!ip->quiet) printf("assertion is true\n");
        free(ex);free(byv);free(sortk); RL_DONE(); return 0;
    }

    /* 'shell cmd' — run via $SHELL -c, return code in rc.  '!' handled earlier. */
    if(!strncmp(t,"shell ",6) || !strcmp(t,"shell")){
        const char *cmd = t+5; while(*cmd==' ') cmd++;
        if(!*cmd){ free(ex); free(byv); free(sortk); RL_DONE(); return 0; }
        const char *sh = getenv("SHELL"); if(!sh||!sh[0]) sh="/bin/sh";
        fflush(stdout);
#ifdef __EMSCRIPTEN__
        { int st = system(cmd); ip->rc = st < 0 ? 128 : ((st >> 8) & 0xff); }
#else
        pid_t pid=fork();
        if(pid==0){ execl(sh,sh,"-c",cmd,(char*)NULL); _exit(127); }
        int st=0; waitpid(pid,&st,0);
        ip->rc = WIFEXITED(st)? WEXITSTATUS(st) : 128;
#endif
        free(ex);free(byv);free(sortk); RL_DONE(); return 0;
    }

    if(!strncmp(t,"local ",6)||!strncmp(t,"loc ",4)){
        char *p=t+(t[3]==' '?4:6); while(*p==' ')p++;
        /* Stata: `local ++x` / `local --x` increment/decrement macro x by 1.
         * SILENT NO-OP GUARD: previously "++yr" was parsed as the macro NAME,
         * silently defining a macro literally called "++yr" while `yr' never
         * changed — every loop iteration reused the same value. */
        if((p[0]=='+'&&p[1]=='+') || (p[0]=='-'&&p[1]=='-')){
            int delta = p[0]=='+' ? 1 : -1;
            p+=2; while(*p==' ')p++;
            char inm[128]; int in_=0;
            while(*p&&!isspace((unsigned char)*p)&&in_<127)inm[in_++]=*p++; inm[in_]=0;
            const char *cur = inm[0]? mac_get(ip->locals,inm) : NULL;
            char *endp=NULL; long v = cur&&*cur ? strtol(cur,&endp,10) : 0;
            if(!inm[0] || !cur || !*cur || (endp&&*endp)){
                fprintf(stderr,"local %s%s: macro is not defined as a number\n",
                        delta>0?"++":"--", inm[0]?inm:"?");
                free(ex);free(byv);free(sortk); RL_DONE();
                g_tea_last_rc=ip->rc=198; return cap?0:198;
            }
            char val[64]; snprintf(val,sizeof val,"%ld",v+delta);
            mac_set(&ip->locals,inm,val);
            free(ex);free(byv);free(sortk); RL_DONE(); return 0;
        }
        char nm[128]; int n=0; while(*p&&!isspace((unsigned char)*p)&&*p!='='&&n<127)nm[n++]=*p++; nm[n]=0;
        while(*p==' ')p++; int isexp=0; if(*p=='='){isexp=1;p++;while(*p==' ')p++;}
        char val[2048];
        if(isexp){ eval_scalar(ip,p,val,sizeof val); }
        else { snprintf(val,sizeof val,"%s",p);
            for(int i=(int)strlen(val)-1;i>=0&&val[i]==' ';i--)val[i]=0;
            /* strip surrounding double quotes: `local v "x y"` -> v = `x y` */
            size_t L=strlen(val);
            if(L>=2 && val[0]=='"' && val[L-1]=='"'){
                memmove(val, val+1, L-2); val[L-2]=0; }
        }
        mac_set(&ip->locals,nm,val); free(ex);free(byv);free(sortk); RL_DONE(); return 0;
    }
    if(!strncmp(t,"global ",7)){
        char *p=t+7; while(*p==' ')p++; char nm[128]; int n=0;
        while(*p&&!isspace((unsigned char)*p)&&*p!='='&&n<127)nm[n++]=*p++; nm[n]=0;
        while(*p==' ')p++; int isexp=0; if(*p=='='){isexp=1;p++;while(*p==' ')p++;}
        char val[2048]; if(isexp)eval_scalar(ip,p,val,sizeof val);
        else { snprintf(val,sizeof val,"%s",p); for(int i=(int)strlen(val)-1;i>=0&&val[i]==' ';i--)val[i]=0;
            size_t L=strlen(val);
            if(L>=2 && val[0]=='"' && val[L-1]=='"'){ memmove(val, val+1, L-2); val[L-2]=0; } }
        mac_set(&ip->globals,nm,val); free(ex);free(byv);free(sortk); RL_DONE(); return 0;
    }
    if(!strncmp(t,"display ",8)||!strncmp(t,"di ",3)||!strcmp(t,"display")){
        char *p=t+(t[2]==' '?3:8); while(*p==' ')p++;
        char outb[4096]; int ob=0;
        while(*p){
            while(*p==' ')p++;
            if(!*p) break;
            if(*p=='"'){ p++; while(*p&&*p!='"'&&ob<4090) outb[ob++]=*p++; if(*p=='"')p++; }
            else { char seg[1024]; int n=0; int paren=0; int inq=0;
                while(*p && ob<4090 && n<1023){
                    if(*p=='"' && paren==0) break;  /* end of bareword segment */
                    if(*p=='"') inq=!inq;
                    else if(!inq){
                        if(*p=='(') paren++;
                        else if(*p==')'){ if(paren==0) break; paren--; }
                    }
                    seg[n++]=*p++;
                }
                seg[n]=0;
                /* trim trailing spaces of segment */
                for(int z=n-1;z>=0&&seg[z]==' ';z--)seg[z]=0;
                char vb[512];
                if(seg[0]&&eval_scalar(ip,seg,vb,sizeof vb)==0){ int l=strlen(vb); if(ob+l<4090){memcpy(outb+ob,vb,l);ob+=l;} }
                else { int l=strlen(seg); if(ob+l<4090){memcpy(outb+ob,seg,l);ob+=l;} }
            }
        }
        outb[ob]=0;
        printf("%s\n",outb);
        /* Tee display output to log file if one is open. */
        extern FILE *g_logfp;
        if(g_logfp){ fprintf(g_logfp, "%s\n", outb); fflush(g_logfp); }
        free(ex);free(byv);free(sortk); RL_DONE(); return 0;
    }
    if(!strncmp(t,"scalar ",7)){ char *p=t+7; if(!strncmp(p,"define ",7))p+=7;
        char nm[128]; int n=0; while(*p&&*p!='='&&!isspace((unsigned char)*p)&&n<127)nm[n++]=*p++; nm[n]=0;
        while(*p==' '||*p=='=')p++; char val[512]; eval_scalar(ip,p,val,sizeof val);
        mac_set(&ip->globals,nm,val); free(ex);free(byv);free(sortk); RL_DONE(); return 0; }
    if(!strcmp(t,"describe")||!strncmp(t,"describe ",9)){}

    /* build a Cmd and dispatch */
    Cmd c; memset(&c,0,sizeof c);
    c.ip=ip; c.ws=ip->ws; c.f=ip->ws->cur;
    c.byvars=byv; c.nby=nby; c.bysort=bysort;
    c.quiet = quiet || ip->quiet;
    /* command word: alphanumerics + underscore only — stop at space OR comma */
    int ci=0; const char *cp=t;
    while(*cp&&!isspace((unsigned char)*cp)&&*cp!=','&&ci<31) c.cmd[ci++]=*cp++;
    c.cmd[ci]=0;
    while(*cp==' ')cp++;
    snprintf(c.args,sizeof c.args,"%s",cp);

    /* by: requires sorted; bysort sorts first (by all sort keys) */
    if(nby>0){
        if(bysort) frame_physical_sort(c.f,sortk,nsk,NULL);
        else {
            bool ok = c.f->nsort>=nby; if(ok)for(int k=0;k<nby;k++)if(c.f->sortvars[k]!=byv[k])ok=false;
            if(!ok){ fprintf(stderr,"not sorted (use bysort)\n"); free(ex);free(byv);free(sortk); RL_DONE();
                g_tea_last_rc=ip->rc=5; return cap?0:5; }
        }
    }
    cmd_split(&c);
    int rc=cmd_dispatch(&c);
    free(ex); free(byv); free(sortk); RL_DONE();
    g_tea_last_rc=ip->rc=rc;
    if(cap){ /* capture: swallow the error, expose it via _rc only */ return 0; }
    return rc;
}

/* ---- block-aware execution over a vector of logical lines ------------- */
typedef struct { char **v; int n; } Lines;

static int exec_range(Interp *ip,Lines *L,int from,int to);

/* find matching close brace for an open at line `open` (brace count). */
static int match_brace(Lines *L,int open){
    int d=0;
    for(int i=open;i<L->n;i++){
        for(char *p=L->v[i];*p;p++){ if(*p=='{')d++; else if(*p=='}'){ d--; if(d==0)return i; } }
    }
    return -1;
}

/* evaluate an if/else-if condition (macro-expanded, variable-free scratch
 * frame).  Returns 1/0; on parse error prints and sets *err_rc. */
static int eval_if_cond(Interp *ip, const char *cond, int *err_rc){
    char *xc = macro_expand(ip, cond);
    Frame scratch; memset(&scratch,0,sizeof scratch); scratch.ts_panel=scratch.ts_time=-1;
    const char *pe; Node *a = expr_parse(xc, &scratch, &pe); free(xc);
    if(!a){ fprintf(stderr,"if: %s\n", pe); *err_rc = 111; return 0; }
    EvalCtx ec={0}; ec.f=&scratch; ec.n=1; ec.N=1;
    EVal v = expr_eval(a,&ec);
    int truth = !v.is_str && !sv_is_miss(v.num) && v.num!=0;
    eval_free(&v); node_free(a);
    return truth;
}

static int exec_one(Interp *ip,Lines *L,int *i){
    char *ln=L->v[*i];
    char *s=ln; while(*s==' ')s++;

    /* foreach X in/of LIST { ... } */
    if(!strncmp(s,"foreach ",8)){
        char var[64]={0}; char kind[16]={0};
        const char *r=s+8; while(*r==' ')r++;
        int n=0; while(*r&&!isspace((unsigned char)*r)&&n<63)var[n++]=*r++; var[n]=0;
        while(*r==' ')r++;
        n=0; while(*r&&!isspace((unsigned char)*r)&&n<15)kind[n++]=*r++; kind[n]=0;
        while(*r==' ')r++;
        char list[2048]; snprintf(list,sizeof list,"%s",r);
        char *brace=strchr(list,'{'); if(brace)*brace=0;
        char *xl=macro_expand(ip,list);
        char **items=NULL; int ni=0;
        if(!strcmp(kind,"of")){
            char *sp=xl; char w[32]; sscanf(sp,"%31s",w); sp+=strlen(w); while(*sp==' ')sp++;
            int *vs=NULL,nv=varlist_expand(ip->ws->cur,sp,&vs);
            /* SILENT NO-OP GUARD: an unknown variable makes varlist_expand
             * return -1; treating that as an empty list executes the loop
             * body ZERO times with no error — renames etc. silently don't
             * happen and the script fails much later with a misleading
             * message.  Fail loud, here, with the offending varlist. */
            if(!strcmp(w,"varlist") && nv<0){
                for(char *tz=sp+strlen(sp); tz>sp && tz[-1]==' '; ) *--tz=0;
                fprintf(stderr,"foreach: varlist %s: variable not found\n",sp);
                free(xl);
                g_tea_last_rc=ip->rc=111;
                return 111;
            }
            for(int k=0;k<nv;k++){ items=realloc(items,(ni+1)*sizeof(char*)); items[ni++]=strdup(ip->ws->cur->vars[vs[k]].name); }
            free(vs);
        } else { /* in: literal list */
            char *sv=NULL; for(char *tk=strtok_r(xl," ",&sv);tk;tk=strtok_r(NULL," ",&sv)){
                items=realloc(items,(ni+1)*sizeof(char*)); items[ni++]=strdup(tk); }
        }
        free(xl);
        int open=*i; int close=match_brace(L,open); if(close<0){fprintf(stderr,"unbalanced {\n");return 199;}
        int brc=0;
        for(int it=0;it<ni && brc==0;it++){
            mac_set(&ip->locals,var,items[it]);
            brc=exec_range(ip,L,open+1,close-1);
        }
        for(int k=0;k<ni;k++)free(items[k]); free(items);
        *i=close; return brc;
    }
    /* forvalues i = a/b  or  a(step)b { } */
    if(!strncmp(s,"forvalues ",10)||!strncmp(s,"forv ",5)){
        char *p=s+(s[4]==' '?5:10); while(*p==' ')p++;
        char var[64]; int n=0; while(*p&&*p!='='&&!isspace((unsigned char)*p))var[n++]=*p++; var[n]=0;
        while(*p==' '||*p=='=')p++;
        double a=0,b=0,step=1;
        if(strchr(p,'(')){ sscanf(p,"%lf(%lf)%lf",&a,&step,&b); }
        else sscanf(p,"%lf/%lf",&a,&b);
        int open=*i,close=match_brace(L,open); if(close<0){fprintf(stderr,"unbalanced {\n");return 199;}
        int brc=0;
        for(double x=a;((step>0)?x<=b+1e-9:x>=b-1e-9) && brc==0;x+=step){
            char vb[32]; if(x==(long long)x)snprintf(vb,sizeof vb,"%lld",(long long)x);else snprintf(vb,sizeof vb,"%g",x);
            mac_set(&ip->locals,var,vb);
            brc=exec_range(ip,L,open+1,close-1);
        }
        *i=close; return brc;
    }
    /* if (cond) { } [else if (cond) { }]* [else { }] — also single-line */
    if(!strncmp(s,"if ",3) && (strchr(s,'{')||1)){
        char *brace=strchr(s,'{');
        char cond[1024];
        if(brace){ snprintf(cond,sizeof cond,"%.*s",(int)(brace-(s+3)),s+3); }
        else { snprintf(cond,sizeof cond,"%s",s+3); }
        if(brace){
            int erc=0;
            int truth = eval_if_cond(ip, cond, &erc); if(erc) return erc;
            int open=*i, close=match_brace(L,open); if(close<0) return 199;
            int last=close, brc=0, done=0;
            if(truth){ brc=exec_range(ip,L,open+1,close-1); done=1; }
            /* walk the chain: } else if (c) { ... } else { ... } */
            while(close+1 < L->n){
                char *e=L->v[close+1]; while(*e==' ')e++;
                if(strncmp(e,"else",4) || (e[4] && e[4]!=' ' && e[4]!='{')) break;
                int eo=close+1, ec2=match_brace(L,eo); if(ec2<0) return 199;
                last=ec2;
                e+=4; while(*e==' ')e++;
                if(!strncmp(e,"if ",3)){
                    char *b2=strchr(e,'{'); if(!b2) break;
                    char c2[1024]; snprintf(c2,sizeof c2,"%.*s",(int)(b2-(e+3)),e+3);
                    if(!done){
                        int t2 = eval_if_cond(ip, c2, &erc); if(erc) return erc;
                        if(t2){ brc=exec_range(ip,L,eo+1,ec2-1); done=1; }
                    }
                } else {
                    if(!done){ brc=exec_range(ip,L,eo+1,ec2-1); done=1; }
                }
                close=ec2;
            }
            *i=last; return brc;
        } else {
            int erc=0;
            /* single-line form: `if EXPR COMMAND ...`.  Stata finds the
             * boundary itself; we take the LONGEST token prefix that parses
             * as an expression — `if x > 1 drop ...` splits after `1`,
             * `if _rc ssc install ...` splits after `_rc`. */
            char work[1024]; snprintf(work,sizeof work,"%s",cond);
            char *tok[64]; int nt=0;
            for(char *t=strtok(work," "); t && nt<64; t=strtok(NULL," ")) tok[nt++]=t;
            int split = nt;               /* default: whole thing is the expr */
            char trycond[1024];
            for(int k=nt; k>=1; k--){
                size_t w=0; trycond[0]=0;
                for(int j=0;j<k;j++) w+=(size_t)snprintf(trycond+w,sizeof trycond-w,"%s%s",j?" ":"",tok[j]);
                char *xc2 = macro_expand(ip,trycond);
                Frame sc2; memset(&sc2,0,sizeof sc2); sc2.ts_panel=sc2.ts_time=-1;
                const char *pe2; Node *a2 = expr_parse(xc2,&sc2,&pe2); free(xc2);
                if(a2){ node_free(a2); split=k; break; }
            }
            if(split==nt && nt>0){
                int truth = eval_if_cond(ip, cond, &erc); if(erc) return erc;
                (void)truth;              /* condition with no command: no-op */
                return 0;
            }
            { size_t w=0; trycond[0]=0;
              for(int j=0;j<split;j++) w+=(size_t)snprintf(trycond+w,sizeof trycond-w,"%s%s",j?" ":"",tok[j]); }
            int truth = eval_if_cond(ip, trycond, &erc); if(erc) return erc;
            if(truth){
                /* command = original text after the split tokens */
                const char *body = cond;
                for(int j=0;j<split;j++){ while(*body==' ')body++; while(*body&&*body!=' ')body++; }
                while(*body==' ')body++;
                char cmdline[1024]; snprintf(cmdline,sizeof cmdline,"%s",body);
                return run_line(ip, cmdline);
            }
            return 0;
        }
    }
    if(!strcmp(s,"}")||!strcmp(s,"{")) return 0;
    return run_line(ip,s);
}

static int exec_range(Interp *ip,Lines *L,int from,int to){
    for(int i=from;i<=to && i<L->n;i++){
        int rc=exec_one(ip,L,&i);
        if(rc>0 && rc!=5) return rc;   /* abort do-file on hard error */
    }
    return 0;
}

/* split a raw stream into logical lines: strip comments, join /// */
static void add_line(Lines *L,const char *s){
    while(*s==' '||*s=='\t')s++;
    L->v=realloc(L->v,(L->n+1)*sizeof(char*)); L->v[L->n++]=strdup(s);
}
/* --------------- readline-backed interactive line reader -------------- */
#ifdef __EMSCRIPTEN__
/* No terminal, no readline in the browser build: the xterm.js front-end
 * drives tea_session_feed() directly.  Keep run_stream() linkable with a
 * plain-fgets reader and no-op setup/teardown. */
static void rl_setup(Workspace *ws){ (void)ws; }
static void rl_teardown(void){}
static char *read_one_line(FILE *in, bool interactive, const char *prompt){
    (void)interactive; (void)prompt;
    char buf[8192];
    if(!fgets(buf,sizeof buf,in)) return NULL;
    size_t n=strlen(buf); while(n&&(buf[n-1]=='\n'||buf[n-1]=='\r'))buf[--n]=0;
    return strdup(buf);
}
#else
#include <readline/readline.h>
#include <readline/history.h>

/* Disp is defined in commands.c; we only need name+ for completion */
typedef struct { const char *name; int (*fn)(void*); int needs_data; const char *help; } DispCompat;
extern DispCompat TABLE[];     /* defined in commands.c */

/* completion needs access to the workspace for variable names */
static Workspace *g_compl_ws = NULL;

static char *cmd_generator(const char *text, int state){
    static int i; static size_t len;
    if(!state){ i=0; len=strlen(text); }
    while(TABLE[i].name){
        const char *n=TABLE[i++].name;
        if(!strncmp(n,text,len)) return strdup(n);
    }
    return NULL;
}
static char *var_generator(const char *text, int state){
    static int i; static size_t len;
    if(!state){ i=0; len=strlen(text); }
    if(!g_compl_ws || !g_compl_ws->cur) return NULL;
    Frame *f=g_compl_ws->cur;
    while(i<f->nvar){
        const char *n=f->vars[i++].name;
        if(!strncmp(n,text,len)) return strdup(n);
    }
    return NULL;
}
static char **tea_completer(const char *text, int start, int end){
    (void)end;
    rl_attempted_completion_over = 1;
    const char *L = rl_line_buffer;

    /* leftmost non-blank index of the line */
    int lhs = 0; while(L[lhs]==' '||L[lhs]=='\t') lhs++;

    /* shell escape: !cmd ... → defer to readline's filename completion */
    if(L[lhs]=='!'){
        /* the first word after ! is the command name; everything after is args.
         * For both, filename completion is the right default. */
        rl_attempted_completion_over = 0;
        return NULL;
    }

    /* find the first word of the line */
    int w1s=lhs, w1e=lhs;
    while(L[w1e] && !isspace((unsigned char)L[w1e])) w1e++;
    char w1[32]={0};
    int wl = w1e-w1s; if(wl>31) wl=31;
    memcpy(w1, L+w1s, wl);

    /* command position: nothing before us (start at lhs) */
    if(start==lhs)
        return rl_completion_matches(text,cmd_generator);

    /* second word of 'help' / 'shell' completes commands / filenames */
    if(!strcmp(w1,"help"))  return rl_completion_matches(text,cmd_generator);
    if(!strcmp(w1,"shell")){ rl_attempted_completion_over=0; return NULL; }

    /* filename positions: anywhere following 'using', or as 1st arg of cd/
     * save/use/log, or after 'log using'. */
    /* scan tokens up to 'start' to see what immediately precedes us */
    char prev[64]={0}, prev2[64]={0};
    int p=lhs;
    while(p<start){
        while(p<start && isspace((unsigned char)L[p])) p++;
        int ws=p; while(p<start && !isspace((unsigned char)L[p])) p++;
        if(ws<p){ snprintf(prev2,sizeof prev2,"%s",prev);
                  int n=p-ws; if(n>63)n=63;
                  memcpy(prev,L+ws,n); prev[n]=0; }
    }
    int is_filename_pos = 0;
    if(!strcmp(prev,"using"))             is_filename_pos = 1;
    if(!strcmp(prev2,"log") && !strcmp(prev,"using")) is_filename_pos=1;
    if(!strcmp(w1,"cd"))                  is_filename_pos = 1;
    if((!strcmp(w1,"save")||!strcmp(w1,"use")) && start==w1e+1) is_filename_pos=1;
    /* import excel|delimited|ods using FILE — 'using' check above covers it */

    if(is_filename_pos){
        rl_attempted_completion_over = 0;          /* let readline do filenames */
        return NULL;
    }

    /* default: variable names of the current frame */
    return rl_completion_matches(text,var_generator);
}

static char *history_path(void){
    static char p[1024]; const char *h=getenv("HOME");
    snprintf(p,sizeof p,"%s/.tea_history", h?h:".");
    return p;
}

static void rl_setup(Workspace *ws){
    rl_readline_name = "tea";
    rl_attempted_completion_function = tea_completer;
    rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{(,";
    g_compl_ws = ws;
    using_history();
    read_history(history_path());
    stifle_history(2000);
}
static void rl_teardown(void){
    write_history(history_path());
    history_truncate_file(history_path(),2000);
}

/* read one logical input line.  In interactive mode uses readline with the
 * given prompt; otherwise fgets.  Returns malloc'd string (caller frees)
 * or NULL on EOF. */
static char *read_one_line(FILE *in, bool interactive, const char *prompt){
    if(interactive){
        char *s = readline(prompt);
        if(!s) return NULL;
        if(*s) add_history(s);
        return s;
    }
    char buf[8192];
    if(!fgets(buf,sizeof buf,in)) return NULL;
    size_t n=strlen(buf); while(n&&(buf[n-1]=='\n'||buf[n-1]=='\r'))buf[--n]=0;
    return strdup(buf);
}
#endif /* !__EMSCRIPTEN__ */

/* ---- push-mode session --------------------------------------------------
 * The old run_stream() loop body, inverted into a state machine so that a
 * non-blocking front-end (browser/WASM, or any embedding) can feed lines
 * one at a time.  Behavior is a faithful port:
 *   - `#delimit ;` owns its physical line and resets the accumulator
 *   - `//` comments strip, `///` continues, leading `*` skips the line
 *   - `{`-blocks accumulate physical lines verbatim until braces balance
 *   - block execution results never abort a do-file (historical behavior)
 *   - at EOF an unterminated block executes as-is; partial statements drop
 */
struct TeaSession {
    Interp *ip;
    bool    interactive;
    char    acc[16384];
    int     inblk;
    bool    in_block;    /* accumulating a {...} block into L */
    char    delim;       /* '\n' or ';' under #delimit */
    bool    pending;     /* block balanced; held in case `else` follows */
    int     cdepth;      /* block-comment nesting depth */
    char    cjoin[16384];/* pre-comment fragment awaiting the closing */
    Lines   L;
};

void interp_hist_add(Interp *ip, const char *line){
    if(!line || !*line) return;
    if(ip->nhist && !strcmp(ip->hist[ip->nhist-1], line)) return; /* dedupe runs */
    if(ip->nhist == ip->histcap){
        ip->histcap = ip->histcap ? ip->histcap*2 : 64;
        ip->hist = realloc(ip->hist, (size_t)ip->histcap * sizeof(char*));
    }
    ip->hist[ip->nhist++] = strdup(line);
}

TeaSession *tea_session_new(Interp *ip, bool interactive){
    TeaSession *s = calloc(1, sizeof *s);
    if(!s) return NULL;
    s->ip = ip; s->interactive = interactive; s->delim = '\n';
    return s;
}

static int session_run_block_rc(TeaSession *s){
    /* echo every line of the block to log (Stata's behavior) */
    for(int z=0;z<s->L.n;z++) tea_log_command(s->L.v[z]);
    if(s->interactive)
        for(int z=0;z<s->L.n;z++) interp_hist_add(s->ip, s->L.v[z]);
    int idx=0; int rc=exec_one(s->ip,&s->L,&idx);
    for(int z=0;z<s->L.n;z++)free(s->L.v[z]); free(s->L.v);
    s->L.v=NULL; s->L.n=0;
    s->in_block=false; s->inblk=0;
    return rc;
}

int tea_session_feed(TeaSession *s, const char *raw, bool *need_more){
    /* ---- block comments (slash-star ... star-slash; nesting) --------------
     * Text inside is removed; a comment spanning lines JOINS the code
     * before the opener with the code after the closer (Stata's old-style
     * line continuation).  Runs first so every later stage — blocks,
     * #delimit, history — sees comment-free input. */
    char _cstrip[16384]; size_t _cw = 0;
    {
        const char *p = raw;
        while(*p && _cw < sizeof _cstrip - 1){
            if(s->cdepth == 0){
                if(p[0]=='/' && p[1]=='*'){ s->cdepth = 1; p += 2; continue; }
                _cstrip[_cw++] = *p++;
            } else {
                if(p[0]=='/' && p[1]=='*'){ s->cdepth++; p += 2; continue; }
                if(p[0]=='*' && p[1]=='/'){ s->cdepth--; p += 2; continue; }
                p++;
            }
        }
        _cstrip[_cw] = 0;
        if(s->cdepth > 0){
            /* unterminated on this line: stash any code fragment, ask for more */
            size_t have = strlen(s->cjoin);
            snprintf(s->cjoin + have, sizeof s->cjoin - have, "%s", _cstrip);
            if(need_more) *need_more = true;
            return 0;
        }
        if(s->cjoin[0]){
            char joined[16384];
            snprintf(joined, sizeof joined, "%s%s", s->cjoin, _cstrip);
            s->cjoin[0] = 0;
            snprintf(_cstrip, sizeof _cstrip, "%s", joined);
        }
        raw = _cstrip;
    }

    if(need_more) *need_more = false;

    /* block-accumulation mode: lines are taken verbatim (no // stripping,
     * no continuation), exactly as the old nested read loop did */
    /* a balanced block is HELD one line: if the next line begins with
     * `else`, it belongs to the same if-statement and accumulation
     * continues (covers else-if chains); anything else flushes the held
     * block first, then the new line is processed normally */
    if(s->pending){
        const char *cs=raw; while(*cs==' ')cs++;
        if(!strncmp(cs,"else",4) && (cs[4]==0||cs[4]==' '||cs[4]=='{')){
            s->pending=false;
            for(const char *p=raw;*p;p++){ if(*p=='{')s->inblk++; else if(*p=='}')s->inblk--; }
            add_line(&s->L,raw);
            if(s->inblk>0){ s->in_block=true; if(need_more)*need_more=true; return 0; }
            s->pending=true;       /* balanced again: another else may follow */
            if(need_more)*need_more=true;
            return 0;
        }
        s->pending=false; s->in_block=false;
        int hrc=session_run_block_rc(s);
        if(hrc>0 && hrc!=5 && !s->interactive) return hrc;
        /* fall through: process `raw` as a fresh line */
    }

    if(s->in_block){
        const char *cs=raw; while(*cs==' ')cs++;
        if(*cs=='*'){ if(need_more)*need_more=true; return 0; }
        for(const char *p=raw;*p;p++){ if(*p=='{')s->inblk++; else if(*p=='}')s->inblk--; }
        add_line(&s->L,raw);
        if(s->inblk>0){ if(need_more)*need_more=true; return 0; }
        s->in_block=false; s->pending=true;   /* hold for possible else */
        if(need_more)*need_more=true;
        return 0;
    }

    /* #delimit directive — owns the whole physical line */
    { const char *cs=raw; while(*cs==' '||*cs=='\t')cs++;
      if(!strncmp(cs,"#delimit",8)){
          while(*cs&&!isspace((unsigned char)*cs))cs++;
          while(*cs==' '||*cs=='\t')cs++;
          s->delim = (*cs==';') ? ';' : '\n';
          s->acc[0]=0;
          return 0;
      } }

    /* strip // and * comments (not inside quotes) */
    char clean[8192]; int ci=0,inq=0; int saw_cont=0;
    for(int k=0;raw[k] && ci<(int)sizeof clean-1;k++){
        if(raw[k]=='"')inq=!inq;
        if(!inq&&raw[k]=='/'&&raw[k+1]=='/'){
            if(raw[k+2]=='/'){ saw_cont=1; break; }
            break;
        }
        clean[ci++]=raw[k];
    }
    clean[ci]=0;
    { const char *cs=clean; while(*cs==' ')cs++;
      if(*cs=='*'){ if(need_more)*need_more=(s->acc[0]!=0); return 0; } }
    if(saw_cont){
        if(s->acc[0]) strncat(s->acc,clean,sizeof s->acc-strlen(s->acc)-1);
        else snprintf(s->acc,sizeof s->acc,"%s",clean);
        if(need_more)*need_more=true;
        return 0;
    }
    if(s->acc[0]){ strncat(s->acc," ",sizeof s->acc-strlen(s->acc)-1);
                   strncat(s->acc,clean,sizeof s->acc-strlen(s->acc)-1); }
    else snprintf(s->acc,sizeof s->acc,"%s",clean);
    for(char *p=s->acc;*p;p++){ if(*p=='{')s->inblk++; else if(*p=='}')s->inblk--; }

    /* Under '#delimit ;' a statement ends at ';', not at newline. */
    if(s->delim==';' && s->inblk<=0 && !strchr(s->acc,';')){
        if(need_more)*need_more=true;
        return 0;
    }

    if(s->inblk>0){
        char buf[16384]; snprintf(buf,sizeof buf,"%s",s->acc); s->acc[0]=0;
        add_line(&s->L,buf);
        s->in_block=true;
        if(need_more)*need_more=true;
        return 0;
    }

    /* In ';' mode, split acc on ';' and execute each segment */
    if(s->delim==';'){
        char *p = s->acc;
        while(p && *p){
            char *semi = strchr(p,';');
            if(!semi) break;
            *semi = 0;
            char *seg = p; while(*seg==' '||*seg=='\t')seg++;
            if(*seg){ tea_log_command(seg); add_line(&s->L,seg);
                if(s->interactive) interp_hist_add(s->ip, s->L.v[s->L.n-1]);
                int idx=s->L.n-1; int rc=exec_one(s->ip,&s->L,&idx);
                for(int z=0;z<s->L.n;z++)free(s->L.v[z]); free(s->L.v);
                s->L.v=NULL; s->L.n=0;
                if(rc>0 && rc!=5 && !s->interactive){ s->acc[0]=0; return rc; }
            }
            p = semi+1;
        }
        /* leftover after last ';' becomes next statement's prefix */
        if(p && *p){ memmove(s->acc,p,strlen(p)+1); } else s->acc[0]=0;
        if(need_more)*need_more=(s->acc[0]!=0);
        return 0;
    }

    if(s->acc[0]){
        tea_log_command(s->acc);
        add_line(&s->L,s->acc); s->acc[0]=0;
        if(s->interactive) interp_hist_add(s->ip, s->L.v[s->L.n-1]);
        int idx=s->L.n-1; int rc=exec_one(s->ip,&s->L,&idx);
        for(int z=0;z<s->L.n;z++)free(s->L.v[z]); free(s->L.v);
        s->L.v=NULL; s->L.n=0;
        if(rc>0 && rc!=5 && !s->interactive) return rc;
    }
    return 0;
}

void tea_session_flush(TeaSession *s){
    if((s->in_block || s->pending) && s->L.n) (void)session_run_block_rc(s);
}

void tea_session_free(TeaSession *s){
    if(!s) return;
    for(int z=0;z<s->L.n;z++)free(s->L.v[z]); free(s->L.v);
    free(s);
}

int run_stream(Interp *ip,FILE *in,bool interactive){
    if(interactive) rl_setup(ip->ws);
    TeaSession *s = tea_session_new(ip, interactive);
    if(!s){ if(interactive) rl_teardown(); return 1; }
    int rc_final = 0;
    bool need_more = false;
    while(!g_exit_requested){
        char *raw = read_one_line(in, interactive, need_more?"> ":". ");
        if(!raw) break;
        if(!interactive) g_current_line++;
        int rc = tea_session_feed(s, raw, &need_more);
        free(raw);
        if(rc>0 && rc!=5 && !interactive){
            fprintf(stderr,"do-file aborted at line %d (rc=%d)\n",g_current_line,rc);
            rc_final = rc;
            break;
        }
    }
    if(!rc_final) tea_session_flush(s);   /* EOF mid-block: run it (historical) */
    tea_session_free(s);
    if(interactive) rl_teardown();
    return rc_final;
}

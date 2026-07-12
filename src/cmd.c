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
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fnmatch.h>

/* ---- varlist expansion -------------------------------------------------- */
static void push(int **a,int *n,int *cap,int v){
    if(*n==*cap){ *cap=*cap?*cap*2:8; *a=realloc(*a,*cap*sizeof(int)); }
    (*a)[(*n)++]=v;
}
int varlist_expand(Frame *f,const char *spec,int **out){
    int *a=NULL,n=0,cap=0; char buf[2048]; snprintf(buf,sizeof buf,"%s",spec);
    char *save=NULL;
    for(char *tok=strtok_r(buf," \t",&save);tok;tok=strtok_r(NULL," \t",&save)){
        if(!strcmp(tok,"_all")){ for(int i=0;i<f->nvar;i++)push(&a,&n,&cap,i); continue; }
        char *dash=strchr(tok,'-');
        if(dash && dash!=tok && dash[1]){            /* range a-b by position */
            *dash=0; int lo=var_find_abbrev(f,tok), hi=var_find_abbrev(f,dash+1);
            if(lo>=0&&hi>=0){ if(lo>hi){int t=lo;lo=hi;hi=t;} for(int i=lo;i<=hi;i++)push(&a,&n,&cap,i); continue; }
            *dash='-';
        }
        /* wildcard: '*' (any) or '?' (one char) anywhere in the token.  Use
         * fnmatch for full glob semantics — Stata supports z*, *z, z?, z?x. */
        if(strpbrk(tok,"*?")){
            int matched=0;
            for(int i=0;i<f->nvar;i++)
                if(fnmatch(tok, f->vars[i].name, 0)==0){ push(&a,&n,&cap,i); matched=1; }
            if(!matched){ /* Stata: no match is OK (silently empty), not an error */ }
            continue;
        }
        int vi=var_find_abbrev(f,tok);
        if(vi>=0) push(&a,&n,&cap,vi);
        else { free(a); *out=NULL; return -1; }      /* unknown var */
    }
    *out=a; return n;
}

/* ---- split args into varlist / if / in / , options ---------------------- */
void cmd_split(Cmd *c){
    c->varlist[0]=c->ifexp[0]=c->options[0]=c->wexp[0]=0;
    c->wtype=0;
    c->in_lo=c->in_hi=-1;
    const char *s=c->args;
    /* options: everything after first top-level comma.  Must skip over
     * parens AND quoted strings — a comma inside "2,500.00" is part of the
     * literal, not an options separator. */
    const char *comma=NULL; int depth=0; int inq=0;
    for(const char *p=s;*p;p++){
        if(inq){ if(*p=='"') inq=0; continue; }
        if(*p=='"'){ inq=1; continue; }
        if(*p=='(')depth++;
        else if(*p==')')depth--;
        else if(*p==','&&depth==0){ comma=p; break; }
    }
    char head[4096];
    if(comma){ snprintf(head,sizeof head,"%.*s",(int)(comma-s),s);
               snprintf(c->options,sizeof c->options,"%s",comma+1); }
    else snprintf(head,sizeof head,"%s",s);

    /* weight clause:  [fw=exp] | [aweight=exp] | [pw=exp] | [iw=exp] */
    char *lb=strchr(head,'['); 
    if(lb){ char *rb=strchr(lb,']'); char *eq=lb?strchr(lb,'='):NULL;
        if(rb&&eq&&eq<rb){
            char wt[16]; snprintf(wt,sizeof wt,"%.*s",(int)(eq-lb-1),lb+1);
            for(char *q=wt;*q;q++)*q=*q==' '?0:*q;
            if(!strncmp(wt,"fw",2)||!strncmp(wt,"fr",2))c->wtype=1;
            else if(!strncmp(wt,"aw",2)||!strncmp(wt,"an",2))c->wtype=2;
            else if(!strncmp(wt,"pw",2)||!strncmp(wt,"pr",2))c->wtype=3;
            else if(!strncmp(wt,"iw",2)||!strncmp(wt,"im",2))c->wtype=4;
            else c->wtype=2;
            snprintf(c->wexp,sizeof c->wexp,"%.*s",(int)(rb-eq-1),eq+1);
            char *we=c->wexp; while(*we==' ')we++; if(we!=c->wexp)memmove(c->wexp,we,strlen(we)+1);
            for(int z=(int)strlen(c->wexp)-1;z>=0&&c->wexp[z]==' ';z--)c->wexp[z]=0;
            /* excise the [ ... ] from head */
            memmove(lb,rb+1,strlen(rb+1)+1);
        }
    }

    /* find ' if ' and ' in ' at depth 0, skipping over quoted strings. */
    char *ifp=NULL,*inp=NULL; depth=0; int inq2=0;
    for(char *p=head;*p;p++){
        if(inq2){ if(*p=='"') inq2=0; continue; }
        if(*p=='"'){ inq2=1; continue; }
        if(*p=='(')depth++; else if(*p==')')depth--;
        if(depth==0 && (p==head||p[-1]==' ')){
            if(!strncmp(p,"if ",3)&&!ifp) ifp=p;
            if(!strncmp(p,"in ",3)&&!inp) inp=p;
        }
    }
    char *cut=head+strlen(head);
    if(ifp&&ifp<cut)cut=ifp;
    if(inp&&inp<cut)cut=inp;
    char save=*cut; *cut=0;
    snprintf(c->varlist,sizeof c->varlist,"%s",head);
    *cut=save;
    /* trim trailing spaces on varlist */
    for(int i=(int)strlen(c->varlist)-1;i>=0&&c->varlist[i]==' ';i--)c->varlist[i]=0;

    if(ifp){ char *end=head+strlen(head); if(inp&&inp>ifp)end=inp;
        char sv=*end;*end=0; snprintf(c->ifexp,sizeof c->ifexp,"%s",ifp+3); *end=sv; }
    if(inp){ long a=1,b=-1; char *q=inp+3; while(*q==' ')q++;
        if(!strncmp(q,"f",1)) a=1; else a=strtol(q,&q,10);
        while(*q==' '||*q=='/')q++;
        if(*q=='l') b=(long)c->f->nobs; else if(*q) b=strtol(q,&q,10); else b=a;
        c->in_lo=a; c->in_hi=b; }
}

bool opt_present(const char *opts,const char *name){
    size_t nl=strlen(name); const char *p=opts;
    while((p=strstr(p,name))){
        bool lb=(p==opts||!isalnum((unsigned char)p[-1]));
        char a=p[nl]; bool rb=(a==0||a==' '||a=='('||a==',');
        if(lb&&rb) return true; p+=nl;
    }
    return false;
}
bool opt_value(const char *opts,const char *name,char *buf,size_t n){
    size_t nl=strlen(name); const char *p=opts;
    while((p=strstr(p,name))){
        bool lb=(p==opts||!isalnum((unsigned char)p[-1]));
        if(lb&&p[nl]=='('){ const char *q=p+nl+1; int d=1; const char *st=q;
            while(*q&&d){ if(*q=='(')d++; else if(*q==')')d--; if(d)q++; }
            snprintf(buf,n,"%.*s",(int)(q-st),st); return true; }
        p+=nl;
    }
    return false;
}

/* ---- physical stable multi-key sort ------------------------------------ */
typedef struct { Frame *f; int *keys; int nkeys; const int *desc; } SortCtx;
static SortCtx *g_sc;
static int cmp_rows(const void *pa,const void *pb){
    size_t ra=*(const size_t*)pa, rb=*(const size_t*)pb;
    for(int k=0;k<g_sc->nkeys;k++){
        Variable *v=&g_sc->f->vars[g_sc->keys[k]];
        int c;
        if(v->type==VT_NUM) c=sv_cmp(v->num[ra],v->num[rb]);
        else c=strcmp(v->str[ra],v->str[rb]);
        if(g_sc->desc&&g_sc->desc[k]) c=-c;
        if(c) return c;
    }
    return (ra>rb)-(ra<rb);   /* stable */
}
void frame_physical_sort(Frame *f,int *keys,int nkeys,const int *desc){
    size_t N=f->nobs; if(N<2){ frame_set_sort(f,keys,nkeys); return; }
    size_t *idx=malloc(N*sizeof(size_t));
    for(size_t i=0;i<N;i++)idx[i]=i;
    SortCtx sc={f,keys,nkeys,desc}; g_sc=&sc;
    qsort(idx,N,sizeof(size_t),cmp_rows);
    for(int vi=0;vi<f->nvar;vi++){
        Variable *v=&f->vars[vi];
        if(v->type==VT_NUM){ double *t=malloc(N*sizeof(double));
            for(size_t i=0;i<N;i++)t[i]=v->num[idx[i]]; free(v->num); v->num=t; }
        else { char **t=malloc(N*sizeof(char*));
            for(size_t i=0;i<N;i++)t[i]=v->str[idx[i]]; free(v->str); v->str=t; }
    }
    free(idx);
    if(!desc) frame_set_sort(f,keys,nkeys); else { free(f->sortvars); f->sortvars=NULL; f->nsort=0; }
}

/* ---- contiguous by-groups (requires sorted by byvars) ------------------- */
int by_groups(Frame *f,int *bv,int nby,size_t **los,size_t **his){
    size_t N=f->nobs;
    size_t *lo=malloc((N+1)*sizeof(size_t)),*hi=malloc((N+1)*sizeof(size_t));
    int g=0; if(N==0){*los=lo;*his=hi;return 0;}
    size_t start=0;
    for(size_t i=1;i<=N;i++){
        int brk=(i==N);
        if(!brk) for(int k=0;k<nby;k++){
            Variable *v=&f->vars[bv[k]];
            if(v->type==VT_NUM){ if(sv_cmp(v->num[i],v->num[i-1])) {brk=1;break;} }
            else if(strcmp(v->str[i],v->str[i-1])){ brk=1; break; }
        }
        if(brk){ lo[g]=start; hi[g]=i-1; g++; start=i; }
    }
    *los=lo;*his=hi;return g;
}

/* dispatch lives in commands.c */
extern int run_command(Cmd *c);
int cmd_dispatch(Cmd *c){ return run_command(c); }

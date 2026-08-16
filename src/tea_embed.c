/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * tea_embed.c — implementation of the neutral embedding API
 * (tea_embed.h).  Owns one Workspace/Interp/TeaSession triple, same
 * as wasm_main.c does for the browser; the two coexist because only
 * one frontend is compiled into any given binary's entry path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tea_version.h"
#include "tea_embed.h"
#include "interp.h"
#include "dataset.h"

static Workspace  *g_e_ws = NULL;
static Interp     *g_e_ip = NULL;
static TeaSession *g_e_s  = NULL;

int tea_embed_init(void){
    if (g_e_s) return 0;
    g_e_ws = ws_new();
    g_e_ip = interp_new(g_e_ws);
    g_e_s  = tea_session_new(g_e_ip, /*interactive=*/true);
    return g_e_s ? 0 : 1;
}

int tea_embed_exec(const char *line){
    if (!g_e_s) return -1;
    bool need_more = false;
    tea_session_feed(g_e_s, line ? line : "", &need_more);
    fflush(stdout); fflush(stderr);
    return need_more ? 1 : 0;
}

int tea_embed_run_dofile(const char *path){
    if (!g_e_s || !path || !path[0]) return -1;
    char line[4200];
    snprintf(line, sizeof line, "do \"%s\"", path);
    bool need_more = false;
    tea_session_feed(g_e_s, line, &need_more);
    fflush(stdout); fflush(stderr);
    return g_e_ip->rc;
}

const char *tea_embed_version(void){
    return TEA_VERSION;
}

extern int tea_complete(Frame *f, const char *line, int point,
                        char *out, size_t outsz);

int tea_embed_complete(const char *line, int point, char *out, size_t outsz){
    if (!g_e_ws || !out || !outsz) return 0;
    out[0] = 0;
    return tea_complete(g_e_ws->cur, line ? line : "", point, out, outsz);
}

void tea_embed_interrupt(void){ tea_interrupt_request(); }

int tea_embed_last_rc(void){ return g_e_ip ? g_e_ip->rc : -1; }

/* ---- frame accessors --------------------------------------------- */

static Frame *F(void){ return g_e_ws ? g_e_ws->cur : NULL; }

int  tea_embed_nvar(void){ Frame *f=F(); return f ? f->nvar : 0; }
long tea_embed_nobs(void){ Frame *f=F(); return f ? (long)f->nobs : 0; }

static const Variable *V(int j){
    Frame *f=F();
    if(!f || j < 0 || j >= f->nvar) return NULL;
    return &f->vars[j];
}

const char *tea_embed_var_name(int j)  { const Variable *v=V(j); return v? v->name   : ""; }
const char *tea_embed_var_label(int j) { const Variable *v=V(j); return v? v->vlabel : ""; }
const char *tea_embed_var_format(int j){ const Variable *v=V(j); return v? v->format : ""; }
const char *tea_embed_var_vallab(int j){ const Variable *v=V(j); return v? v->vallab : ""; }
int         tea_embed_var_is_str(int j){ const Variable *v=V(j); return v? (v->type==VT_STR) : 0; }
const char *tea_embed_data_label(void) { Frame *f=F(); return f? f->data_label : ""; }
const char *tea_embed_data_source(void){ Frame *f=F(); return f? f->source     : ""; }

int tea_embed_sorted_by(int j){
    Frame *f=F(); if(!f) return 0;
    for(int k=0;k<f->nsort;k++) if(f->sortvars[k]==j) return 1;
    return 0;
}

double tea_embed_cell_num(long i, int j){
    Frame *f=F(); const Variable *v=V(j);
    if(!f || !v || v->type!=VT_NUM || i<0 || (size_t)i>=f->nobs) return NAN;
    return v->num[i];
}

/* cell text through the SAME renderer as `list` (fmt_cell, exported
 * from commands.c as tea_cell_text): extended missing codes, value
 * labels, %td/%tm/%tq/%tw/%th/%ty time formats, custom numeric
 * formats — the data browser shows exactly what the terminal prints */
extern void tea_cell_text(const Variable *v, size_t i, char *out, size_t n);

void tea_embed_cell(long i, int j, char *buf, size_t n){
    if(!buf || !n) return;
    buf[0]=0;
    Frame *f=F(); const Variable *v=V(j);
    if(!f || !v || i<0 || (size_t)i>=f->nobs) return;
    tea_cell_text(v, (size_t)i, buf, n);
}

/* FNV over names + payload: cheap change probe for view refresh */
unsigned tea_embed_data_hash(void){
    Frame *f=F();
    unsigned h = 2166136261u;
    if(!f) return h;
    #define EMB_MIX(p, len) do { const unsigned char *_b=(const unsigned char*)(p); \
        for(size_t _i=0;_i<(len);_i++){ h ^= _b[_i]; h *= 16777619u; } } while(0)
    EMB_MIX(&f->nobs, sizeof f->nobs);
    for(int j=0;j<f->nvar;j++){
        Variable *v=&f->vars[j];
        EMB_MIX(v->name, strlen(v->name)+1);
        if(v->type==VT_NUM) EMB_MIX(v->num, f->nobs*sizeof(double));
        else for(size_t i=0;i<f->nobs;i++) EMB_MIX(v->str[i], strlen(v->str[i])+1);
    }
    #undef EMB_MIX
    return h;
}

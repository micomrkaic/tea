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
#include "dataset.h"
#include "value.h"
#include "estimates.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

Workspace *ws_new(void) {
    Workspace *w = calloc(1, sizeof *w);
    Frame *f = frame_create(w, "default");
    w->cur = f;
    return w;
}

static void frame_free(Frame *f) {
    for (int i = 0; i < f->nvar; i++) {
        Variable *v = &f->vars[i];
        if (v->type == VT_STR && v->str) {
            for (size_t r = 0; r < f->nobs; r++) free(v->str[r]);
            free(v->str);
        } else {
            free(v->num);
        }
    }
    free(f->vars);
    free(f->sortvars);
    free(f);
}

void ws_free(Workspace *w) {
    if (!w) return;
    Frame *f = w->frames;
    while (f) { Frame *n = f->next; frame_free(f); f = n; }
    VLabel *L = w->vlabels;
    while (L) { VLabel *ln = L->next;
        VLItem *it = L->items;
        while (it) { VLItem *in = it->next; free(it->txt); free(it); it = in; }
        free(L); L = ln; }
    est_free(w->last_est);
    est_free(w->fe_est);
    est_free(w->re_est);
    /* drop named stored estimates */
    StoredEst *s = w->stored_est;
    while(s){ StoredEst *n = s->next; est_free(s->est); free(s); s = n; }
    free(w);
}

Frame *frame_get(Workspace *w, const char *name) {
    for (Frame *f = w->frames; f; f = f->next)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

Frame *frame_create(Workspace *w, const char *name) {
    Frame *f = calloc(1, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->ts_panel = f->ts_time = -1;
    f->ts_delta = 1;
    f->next = w->frames;
    w->frames = f;
    return f;
}

void frame_clear(Frame *f) {
    for (int i = 0; i < f->nvar; i++) {
        Variable *v = &f->vars[i];
        if (v->type == VT_STR && v->str) {
            for (size_t r = 0; r < f->nobs; r++) free(v->str[r]);
            free(v->str);
        } else free(v->num);
    }
    free(f->vars); f->vars = NULL;
    f->nvar = f->cap_var = 0;
    f->nobs = 0;
    free(f->sortvars); f->sortvars = NULL; f->nsort = 0;
    f->ts_panel = f->ts_time = -1; f->ts_delta = 1; f->ts_fmt[0] = 0;
}

int var_find(Frame *f, const char *name) {
    for (int i = 0; i < f->nvar; i++)
        if (!strcmp(f->vars[i].name, name)) return i;
    return -1;
}

Variable *var_add(Frame *f, const char *name, VarType t) {
    if (f->nvar == f->cap_var) {
        f->cap_var = f->cap_var ? f->cap_var * 2 : 8;
        f->vars = realloc(f->vars, f->cap_var * sizeof *f->vars);
    }
    Variable *v = &f->vars[f->nvar++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = t;
    strcpy(v->format, t == VT_NUM ? "%9.0g" : "%9s");
    if (f->nobs) {
        if (t == VT_NUM) {
            v->num = malloc(f->nobs * sizeof(double));
            for (size_t i = 0; i < f->nobs; i++) v->num[i] = SV_MISS;
        } else {
            v->str = malloc(f->nobs * sizeof(char *));
            for (size_t i = 0; i < f->nobs; i++) v->str[i] = calloc(1, 1);
        }
        v->cap = f->nobs;
    }
    return v;
}

void frame_set_nobs(Frame *f, size_t n) {
    for (int i = 0; i < f->nvar; i++) {
        Variable *v = &f->vars[i];
        if (n > v->cap) {
            /* geometric growth: row-at-a-time loaders (CSV, dta) call this
             * once per row, and realloc-to-exact-size made that quadratic */
            size_t nc = v->cap ? v->cap : 64;
            while (nc < n) nc *= 2;
            if (v->type == VT_NUM) v->num = realloc(v->num, nc * sizeof(double));
            else                   v->str = realloc(v->str, nc * sizeof(char *));
            v->cap = nc;
        }
        if (v->type == VT_NUM) {
            for (size_t r = f->nobs; r < n; r++) v->num[r] = SV_MISS;
        } else {
            for (size_t r = f->nobs; r < n; r++) v->str[r] = calloc(1, 1);
        }
    }
    f->nobs = n;
}

void frame_unsort(Frame *f) {
    free(f->sortvars); f->sortvars = NULL; f->nsort = 0;
    /* panel order can no longer be trusted */
    f->ts_panel = f->ts_time = -1;
}

void frame_set_sort(Frame *f, const int *idx, int n) {
    free(f->sortvars);
    f->sortvars = malloc(n * sizeof(int));
    memcpy(f->sortvars, idx, n * sizeof(int));
    f->nsort = n;
}

const char *str_get(Variable *v, size_t i) { return v->str[i]; }

void str_set(Variable *v, size_t i, const char *s) {
    free(v->str[i]);
    size_t n = strlen(s);
    v->str[i] = malloc(n + 1);
    memcpy(v->str[i], s, n + 1);
}

VLabel *vlabel_get(Workspace *w, const char *name) {
    for (VLabel *L = w->vlabels; L; L = L->next)
        if (!strcmp(L->name, name)) return L;
    return NULL;
}
VLabel *vlabel_ensure(Workspace *w, const char *name) {
    VLabel *L = vlabel_get(w, name);
    if (L) return L;
    L = calloc(1, sizeof *L);
    snprintf(L->name, sizeof L->name, "%s", name);
    L->next = w->vlabels; w->vlabels = L;
    return L;
}
void vlabel_put(VLabel *L, double val, const char *txt) {
    for (VLItem *it = L->items; it; it = it->next)
        if (it->val == val) { free(it->txt); it->txt = strdup(txt); return; }
    VLItem *it = calloc(1, sizeof *it);
    it->val = val; it->txt = strdup(txt);
    it->next = L->items; L->items = it;
}
const char *vlabel_lookup(Workspace *w, const char *lname, double val) {
    if (!lname || !lname[0]) return NULL;
    VLabel *L = vlabel_get(w, lname);
    if (!L) return NULL;
    for (VLItem *it = L->items; it; it = it->next)
        if (it->val == val) return it->txt;
    return NULL;
}

void ws_clear_labels(Workspace *w) {
    VLabel *L = w->vlabels;
    while (L) {
        VLabel *next = L->next;
        VLItem *it = L->items;
        while (it) {
            VLItem *itn = it->next;
            free(it->txt); free(it);
            it = itn;
        }
        free(L);
        L = next;
    }
    w->vlabels = NULL;
}

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
/* dataset.h — typed columnar store + frames + sort/panel state. */
#ifndef PSTATA_DATASET_H
#define PSTATA_DATASET_H

#include <stddef.h>
#include <stdbool.h>

typedef enum { VT_NUM, VT_STR } VarType;

typedef struct {
    char    name[33];
    VarType type;
    double *num;          /* VT_NUM: nobs doubles */
    char  **str;          /* VT_STR: nobs owned char* (never NULL; "" == missing) */
    char    vlabel[81];   /* variable label */
    char    format[33];   /* display format, e.g. %9.0g, %td, %tq */
    char    vallab[33];   /* attached value-label name, or "" */
} Variable;

typedef struct Frame {
    char       name[33];
    Variable  *vars;
    int        nvar;
    int        cap_var;
    size_t     nobs;
    char       data_label[81];   /* dataset label (Stata's `label data` text) */
    char       fweight_var[33];  /* name of variable declared as fweight, or "" */
    /* sort state: list of var indices the data is currently sorted by */
    int       *sortvars;
    int        nsort;
    /* panel state from tsset/xtset */
    int        ts_panel;  /* var index of panel id, or -1 */
    int        ts_time;   /* var index of time var, or -1 */
    int        ts_delta;  /* spacing (usually 1) */
    char       ts_fmt[33];/* time display format implied by tsset */
    struct Frame *next;
} Frame;

typedef struct VLItem { double val; char *txt; struct VLItem *next; } VLItem;
typedef struct VLabel { char name[33]; VLItem *items; struct VLabel *next; } VLabel;

struct Estimates;   /* fwd; defined in estimates.h */

/* Named-estimates storage (estimates store / restore / dir / table /
 * drop).  Linked list keyed by user-supplied name.  `est` is owned by
 * the StoredEst node; freed when the node is removed. */
typedef struct StoredEst {
    char name[33];
    struct Estimates *est;
    struct StoredEst *next;
} StoredEst;

typedef struct {
    Frame *frames;        /* linked list */
    Frame *cur;           /* active frame ("default" unless changed) */
    VLabel *vlabels;      /* defined value-label sets */
    struct Estimates *last_est;   /* last estimation result, or NULL */
    /* Auxiliary slots for hausman.  When xtreg runs in fe mode it
     * stores into fe_est; in re mode it stores into re_est.  This way
     * the user can run both then say `hausman` with no arguments. */
    struct Estimates *fe_est;
    struct Estimates *re_est;
    StoredEst *stored_est;  /* named saved estimates */
} Workspace;

Workspace *ws_new(void);
void       ws_free(Workspace *w);

Frame *frame_get(Workspace *w, const char *name);   /* NULL if absent */
Frame *frame_create(Workspace *w, const char *name); /* empty frame */
void   frame_clear(Frame *f);                        /* drop all data */

int        var_find(Frame *f, const char *name);     /* index or -1 */
Variable  *var_add(Frame *f, const char *name, VarType t);
void       frame_set_nobs(Frame *f, size_t n);       /* grow/init columns */

/* invalidate sort + panel order (called by destructive commands) */
void       frame_unsort(Frame *f);
void       frame_set_sort(Frame *f, const int *idx, int n);

const char *str_get(Variable *v, size_t i);
void        str_set(Variable *v, size_t i, const char *s);

VLabel *vlabel_get(Workspace *w, const char *name);   /* NULL if absent */
VLabel *vlabel_ensure(Workspace *w, const char *name);
void    vlabel_put(VLabel *L, double val, const char *txt);
const char *vlabel_lookup(Workspace *w, const char *lname, double val); /* or NULL */
void    ws_clear_labels(Workspace *w);  /* drop all VLabel sets in workspace */

#endif

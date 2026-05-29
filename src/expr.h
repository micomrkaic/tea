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
/* expr.h — AST, parser, evaluator for the expression sublanguage. */
#ifndef PSTATA_EXPR_H
#define PSTATA_EXPR_H

#include "dataset.h"
#include <stdbool.h>

typedef enum {
    N_NUM, N_STR, N_VAR, N_TSOP, N_UNARY, N_BINARY, N_CALL
} NodeKind;

typedef struct Node {
    NodeKind kind;
    double   num;
    char     text[256];        /* string lit / var name / func name */
    struct Node *a, *b;        /* operands / subscript / args list */
    struct Node *next;         /* CALL arg chain */
    int      op;               /* token kind for UNARY/BINARY */
    int      tslag;            /* TSOP: signed lag (L2 -> -2, F1 -> +1) */
    char     tskind;           /* TSOP: 'L' 'F' 'D' 'S' */
} Node;

/* Evaluation context: where & how we are iterating. */
typedef struct {
    Frame  *f;
    size_t  i;          /* absolute storage row of current obs */
    long    n;          /* value of _n (1-based; within group under by:) */
    long    N;          /* value of _N (group size under by:, else nobs) */
    size_t *order;      /* iteration permutation (size f->nobs), or NULL */
    size_t  grp_lo;     /* group's first slot in `order` (by:) */
    size_t  grp_hi;     /* group's last slot in `order` (inclusive) */
    bool    grouped;    /* true when running under by: */
    /* lazily built (panel,time)->row index for TS operators */
    void   *tsidx;
    void   *accs;       /* running-accumulator list for sum(); reset per pass */
    const char *err;
} EvalCtx;

typedef struct {
    bool    is_str;
    double  num;
    char   *str;        /* owned when is_str */
} EVal;

Node *expr_parse(const char *src, Frame *f, const char **err);
void  node_free(Node *n);

EVal  expr_eval(Node *n, EvalCtx *ctx);
void  eval_free(EVal *v);

/* convenience: evaluate to truthiness (Stata: nonzero & nonmissing => true) */
bool  expr_eval_bool(Node *n, EvalCtx *ctx);

void  tsidx_free(void *idx);
void  accs_free(void **accs);   /* call before & after each row pass */

#endif

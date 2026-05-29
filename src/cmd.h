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
/* cmd.h — command parsing, cross-cutting qualifiers, dispatch. */
#ifndef PSTATA_CMD_H
#define PSTATA_CMD_H

#include "dataset.h"
#include "expr.h"
#include <stdbool.h>

typedef struct Interp Interp;   /* fwd: macros, control flow, I/O */
struct Estimates;               /* fwd: defined in estimates.h */
struct MacroKV;                 /* fwd: defined in interp.h */

typedef struct {
    Interp  *ip;
    Workspace *ws;
    Frame   *f;             /* active frame shortcut */
    /* by prefix */
    int     *byvars; int nby; bool bysort;
    /* parsed pieces */
    char     cmd[32];
    char     args[4096];    /* everything after command word, macros expanded */
    /* qualifiers, filled by cmd_split() */
    char     varlist[2048];
    char     ifexp[2048];
    long     in_lo, in_hi;  /* 1-based; -1 if absent */
    char     options[2048];
    char     wexp[256];     /* weight expression (var/expr), or "" */
    int      wtype;         /* 0 none, 1 fw, 2 aw, 3 pw, 4 iw */
    bool     quiet;
} Cmd;

/* expand a varlist string into variable indices (handles a-b, v*, _all). */
int  varlist_expand(Frame *f, const char *spec, int **out);

/* split Cmd.args into varlist / if / in / options (Stata grammar). */
void cmd_split(Cmd *c);

/* option helpers operating on Cmd.options */
bool opt_present(const char *opts, const char *name);
bool opt_value(const char *opts, const char *name, char *buf, size_t n);

/* run one command line (already macro-expanded, prefix parsed by interp). */
int  cmd_dispatch(Cmd *c);

/* shared: build [lo,hi] contiguous by-group ranges over current order.
 * returns count; caller frees the los and his arrays. Requires sorted by byvars. */
int  by_groups(Frame *f, int *byvars, int nby, size_t **los, size_t **his);

/* physical stable sort of all columns by keys (asc), used by sort/by. */
void frame_physical_sort(Frame *f, int *keys, int nkeys, const int *desc);

/* command dispatch entry point (defined in commands.c) */
int run_command(Cmd *c);

/* echo a command line to the open log file, if any (defined in commands.c).
 * Called from the interpreter on every executed command in non-interactive
 * mode so that `log using FILE` captures both commands and output. */
void tea_log_command(const char *line);

/* estimation handlers (defined in regress.c) */
int do_regress(Cmd *c);
int do_predict(Cmd *c);
int do_test(Cmd *c);
int do_lincom(Cmd *c);
int do_xtreg(Cmd *c);
int do_hausman(Cmd *c);
int do_logit(Cmd *c);
int do_probit(Cmd *c);
int do_margins(Cmd *c);
int do_ivregress(Cmd *c);
int do_poisson(Cmd *c);
/* defined in arima.c */
int do_arima(Cmd *c);

/* shared helper from regress.c — store _b[name] and _se[name] macros */
void store_coef_macros(struct Estimates *e, struct MacroKV **tbl);

/* PRNG: defined in eval.c, called from commands.c (`set seed`) */
void tea_srand(unsigned long seed);

/* defined in estcmd.c */
int do_estimates(Cmd *c);
/* defined in estout.c */
int do_estout(Cmd *c);

#endif

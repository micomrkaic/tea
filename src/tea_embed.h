/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * tea_embed.h — the neutral embedding API.
 *
 * tea has multiple frontends over one core: the readline REPL
 * (main.c), the browser (wasm_main.c), and the Qt desktop shell
 * (gui/).  This header is the surface they share.  It is a plain C
 * API, safe to include from C++ (the Qt shell) — GUI frontends never
 * include core headers, so the core's types stay out of C++
 * translation units entirely.
 *
 * Threading contract: all functions here must be called from ONE
 * thread (the GUI runs them on a worker).  tea_embed_interrupt() is
 * the single exception — it only sets a flag and may be called from
 * any thread (the UI thread's Break button).  Data accessors are
 * only meaningful BETWEEN commands: the GUI refreshes its models
 * after each command completes, never during.
 *
 * Output: the core prints to stdout/stderr.  Embedding frontends
 * capture at the fd level (pipe + reader thread); see gui/ OutputPump.
 */
#ifndef TEA_EMBED_H
#define TEA_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle & execution -------------------------------------- */
int         tea_embed_init(void);              /* 0 ok; idempotent */
/* feed one input line; returns 1 when a continuation ("> ") is
 * wanted, 0 otherwise */
int         tea_embed_exec(const char *line);
int         tea_embed_run_dofile(const char *path);   /* via `do` */
const char *tea_embed_version(void);
/* newline-joined completion candidates into out; returns count */
int         tea_embed_complete(const char *line, int point,
                               char *out, size_t outsz);
/* request Break: honored at the next command boundary (any thread) */
void        tea_embed_interrupt(void);
/* Stata rc of the last executed command (_rc) */
int         tea_embed_last_rc(void);

/* ---- frame accessors for GUI models ------------------------------ */
/* Valid between commands only.  Column index j in [0, nvar). */
int         tea_embed_nvar(void);
long        tea_embed_nobs(void);
const char *tea_embed_var_name(int j);
const char *tea_embed_var_label(int j);        /* "" if none */
const char *tea_embed_var_format(int j);
const char *tea_embed_var_vallab(int j);       /* value-label name or "" */
int         tea_embed_var_is_str(int j);
const char *tea_embed_data_label(void);        /* dataset label or "" */
const char *tea_embed_data_source(void);       /* use/import provenance */
int         tea_embed_sorted_by(int j);        /* 1 if j in sort key */

/* cell rendered for display (value labels applied, '.' for missing);
 * writes a NUL-terminated string into buf */
void        tea_embed_cell(long i, int j, char *buf, size_t n);
unsigned    tea_embed_data_hash(void);         /* change probe for views */
/* raw numeric value (NaN when missing or when the column is string) */
double      tea_embed_cell_num(long i, int j);

#ifdef __cplusplus
}
#endif
#endif /* TEA_EMBED_H */

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
/* interp.h — macros, scalars, r()-results, control flow, do-file driver. */
#ifndef PSTATA_INTERP_H
#define PSTATA_INTERP_H

#include "dataset.h"
#include "tea_system.h"
#include <stdbool.h>
#include <stdio.h>

/* Single-source version: the Makefile injects -DTEA_VERSION_FROM_FILE
 * from the VERSION file at the repo root.  Everything user-visible
 * (banner, --version, wasm splash, manual, dist tarball, git tag)
 * derives from that one file. */
#ifdef TEA_VERSION_FROM_FILE
#define TEA_VERSION TEA_VERSION_FROM_FILE
#else
#define TEA_VERSION "dev"
#endif

typedef struct MacroKV { char *name; char *val; struct MacroKV *next; } MacroKV;

typedef struct Interp {
    Workspace *ws;
    MacroKV   *locals;     /* `name' */
    MacroKV   *globals;    /* $name  */
    MacroKV   *rret;       /* r(name) set by summarize/count/... */
    int        rc;         /* last return code (for capture) */
    bool       quiet;      /* quietly suppression in effect */
    int        depth;      /* nesting (loops/if) for safety */
    bool       strict_stata; /* reject tea-only features; default true. */
} Interp;

Interp *interp_new(Workspace *ws);
void    interp_free(Interp *ip);

void    mac_set(MacroKV **tbl,const char *name,const char *val);
const char *mac_get(MacroKV *tbl,const char *name);
void    mac_clear(MacroKV **tbl);

/* expand `local' and $global and =exp and r()/e() refs in a line. */
char   *macro_expand(Interp *ip,const char *line);

/* run a single logical line (prefix/qualifiers handled inside). */
int     run_line(Interp *ip,const char *line);

/* run a stream of lines (do-file). handles continuation /// , comments,
 * and multi-line control blocks (foreach/forvalues/if/{ }). */
int     run_stream(Interp *ip,FILE *in,bool interactive);

/* ---- push-mode session ---------------------------------------------------
 * Event-driven line interface: the caller feeds one physical input line at
 * a time and the session keeps all cross-line state (`///` continuations,
 * `#delimit ;` mode, `{...}` block accumulation).  run_stream() is a thin
 * driver over this; a browser/WASM front-end feeds it directly, one line
 * per keypress-Enter, with no blocking read loop.
 */
typedef struct TeaSession TeaSession;
TeaSession *tea_session_new(Interp *ip, bool interactive);
/* Feed one physical line (without trailing newline).  Returns the return
 * code the driver should treat as do-file-aborting (0 = fine).  Sets
 * *need_more when the statement is incomplete (continuation prompt). */
int  tea_session_feed(TeaSession *s, const char *line, bool *need_more);
/* EOF: execute any pending unterminated {...} block (matches historical
 * behavior); leftover partial statements are discarded. */
void tea_session_flush(TeaSession *s);
void tea_session_free(TeaSession *s);

#endif

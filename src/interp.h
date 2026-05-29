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
#include <stdbool.h>
#include <stdio.h>

#define TEA_VERSION "1.0"

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

#endif

/* tsop.h — Shared time-series operator handling for command varlists.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) Mico Mrkaic.
 *
 * Tea commands (summarize, list, regress, tabulate, ...) accept varlists
 * that can mix plain variable names with Stata-style time-series operator
 * tokens: L.x, L2.x, L(1/2).x, L.(x y), L(1(2)9).x, L(1/2).(x y), etc.
 * (Also F, D, S as operator letters.)
 *
 * The plain-name and wildcard machinery lives in varlist_expand
 * (cmd.c).  This module adds TS-op support on top: for each TS-op token
 * found, it materializes a TEMPORARY numeric column on the frame with
 * the canonical display name ("L.x", "D2.y", etc.) and returns its
 * index alongside the indices that varlist_expand would have produced.
 *
 * Typical call site:
 *
 *     int *idx = NULL; int n_temps = 0;
 *     int nv = tsop_expand_varlist(f, c->varlist, &idx, &n_temps, &err);
 *     if (nv < 0) { tea_err("%s\n", err); ...handle... }
 *     ...use idx[0..nv-1] as variable indices, refer to f->vars[idx[k]]
 *     ...after the command:
 *     free(idx);
 *     tsop_drop_temps(f, n_temps);
 *
 * The temporary columns are always appended at the end of the frame, so
 * tsop_drop_temps just truncates the last n_temps columns.  Callers
 * MUST NOT add or remove other columns between expand and drop.
 */
#ifndef PSTATA_TSOP_H
#define PSTATA_TSOP_H

#include "dataset.h"

/* Expand a varlist string, materializing TS-op tokens as temporary
 * frame columns.  Plain names, wildcards, ranges, and _all are routed
 * through varlist_expand as usual.
 *
 *   f         — frame (may have new columns appended)
 *   varlist   — input varlist string (NUL-terminated)
 *   indices   — out: malloc'd array of variable indices.  Caller frees.
 *   n_temps   — out: number of temporary columns added at the end of f
 *               (caller MUST call tsop_drop_temps(f, *n_temps) when done)
 *   err       — out: error message string on failure
 *
 * Returns the number of resolved variables on success (size of *indices),
 * or -1 on error (with *err set, *indices and *n_temps undefined).
 *
 * If the varlist token is empty (""), returns 0 with no temps. */
int tsop_expand_varlist(Frame *f, const char *varlist,
                        int **indices, int *n_temps, const char **err);

/* Drop the last n_temps columns from the frame.  Safe to call with
 * n_temps == 0.  Frees the columns' data arrays. */
void tsop_drop_temps(Frame *f, int n_temps);

#endif

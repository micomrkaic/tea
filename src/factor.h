/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * factor.h — Factor-variable resolution for command varlists.
 *
 * Stata factor-variable grammar:
 *
 *   i.var               discrete factor; one dummy column per non-base
 *                       level.  Base level defaults to the smallest
 *                       observed level (Stata default).
 *   ib<n>.var           same, but the base level is explicitly n.
 *   c.var               mark `var` as continuous (no expansion).  Mostly
 *                       useful inside # interactions where the default
 *                       interpretation would be discrete.
 *   A#B                 interaction: cross-product of the columns of A
 *                       and B.  For i#i interactions, the cross excludes
 *                       any cell where either factor is at its base —
 *                       so (K_A - 1) * (K_B - 1) columns.
 *   A##B                main effects + interaction.  Equivalent to
 *                       "A B A#B".
 *
 * Where A, B can be `i.var`, `ib<n>.var`, or `c.var`.
 *
 * Examples and their expansions (assume country has levels {1,2,3}
 * and year has levels {2010, 2011, 2012}):
 *
 *   i.country           → 2 columns: "2.country" "3.country"
 *   ib2.country         → 2 columns: "1.country" "3.country"
 *   c.gdp               → 1 column:  "gdp"  (just the variable itself)
 *   i.country#c.gdp     → 2 columns: "2.country#c.gdp" "3.country#c.gdp"
 *   i.country#i.year    → 4 columns: "2.country#2011.year" "2.country#2012.year"
 *                                    "3.country#2011.year" "3.country#2012.year"
 *   i.country##c.gdp    → 3 columns: "2.country" "3.country" "c.gdp"
 *                         actually:  "2.country" "3.country" "gdp"
 *                         plus 2 interaction columns
 *
 * In each case the materialised columns are appended as TEMPORARY frame
 * columns and returned alongside their indices (same pattern as the
 * tsop module).  Caller is responsible for calling factor_drop_temps
 * before adding any new permanent variables, or before exiting the
 * command.
 *
 * Restrictions in v1.0:
 *   - i. requires the variable to be numeric.  For string panel IDs,
 *     use `encode` first.
 *   - Composition with TS-ops (e.g., i.country#c.L.gdp) is not yet
 *     supported in v1.0.  Users wanting this should manually create
 *     the lag with `gen L1_gdp = L.gdp` then use `i.country#c.L1_gdp`.
 *
 * The token detector `factor_is_factor_token` is used by the higher-
 * level varlist resolver to decide whether to dispatch a token here
 * versus to tsop or plain varlist_expand.
 */
#ifndef PSTATA_FACTOR_H
#define PSTATA_FACTOR_H

#include "dataset.h"
#include <stdbool.h>

/* Return true if `tok` looks like a factor-variable token.  Recognised
 * patterns:
 *   i.<name>      ib<digits>.<name>     c.<name>
 *   anything containing '#' between atoms of the above forms.
 *
 * NOT recognised (returns false): plain variable names, names starting
 * with 'L.' / 'F.' / 'D.' / 'S.' (those are TS-ops). */
bool factor_is_factor_token(const char *tok);

/* Expand a single factor-variable token into temp frame columns.
 *
 *   f         — frame (will have new numeric columns appended)
 *   token     — single token from the varlist (no whitespace; no
 *               leading/trailing spaces)
 *   indices   — out: malloc'd array of new column indices in f
 *               (caller frees with free())
 *   n_temps   — out: number of temp columns added to f
 *               (caller MUST call factor_drop_temps(f, *n_temps))
 *   err       — out: error message on failure
 *
 * Returns the number of new columns (>= 0) on success, or -1 on error
 * (with *err set; nothing appended to f). */
int factor_expand_token(Frame *f, const char *token,
                        int **indices, int *n_temps, const char **err);

/* Drop the last n_temps columns from f.  Mirrors tsop_drop_temps. */
void factor_drop_temps(Frame *f, int n_temps);

#endif

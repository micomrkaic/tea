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
/* graph.h — multi-series `twoway`, `graph box`, and the named-graph
 * registry with `graph combine`.
 *
 * Round two of tea graphics.  plot.c (scatter / line / histogram, one
 * series per plot) is deliberately untouched so its golden SVGs stay
 * byte-identical; everything new lives here.  Same renderer philosophy:
 * fixed canvas, deterministic text output (%.2f coordinates, snapped),
 * golden-file testable on all three rigs.
 *
 * Stata-parity notes (documented deviations in COMPATIBILITY.md):
 *   - name(NAME) also writes NAME.svg to disk (Stata keeps graphs only
 *     in memory until `graph export`); on a CLI the file IS the artifact
 *     and in the browser each write feeds the Plots tab.
 *   - `twoway (line ...)` sorts each line/lowess series by x.
 *   - unknown COSMETIC suboptions inside graphics options are accepted
 *     and ignored; malformed structure still errors loudly.
 */
#ifndef TEA_GRAPH_H
#define TEA_GRAPH_H

#include "cmd.h"

int do_twoway(Cmd *c);     /* twoway (plottype y x [if], opts) ... , gopts */
int do_graph(Cmd *c);      /* graph box | graph combine | graph drop|dir  */

/* registry teardown (called from workspace/interp teardown paths) */
void graph_registry_clear(void);

#endif

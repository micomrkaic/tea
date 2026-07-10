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
/* plot.h — self-contained SVG plot renderer + plot commands.
 *
 * Design: commands (scatter / line / histogram) fill a PlotSpec; a single
 * renderer turns the spec into SVG text.  The same spec+renderer pair is
 * used by the native build (write file, optionally open a viewer) and by
 * the future WASM build (hand the SVG string to the page).  No external
 * graphics dependency.
 */
#ifndef PSTATA_PLOT_H
#define PSTATA_PLOT_H

#include <stdio.h>
#include "cmd.h"

typedef enum { PK_SCATTER, PK_LINE, PK_HIST } PlotKind;

typedef struct {
    PlotKind kind;
    /* data: for scatter/line, paired x/y of length n.
     * for histogram, y is unused; x holds the raw sample of length n. */
    double *x, *y;
    size_t  n;
    /* histogram layout, filled by the command layer */
    int     bins;        /* 0 = auto: min(ceil(sqrt(N)), 50) */
    bool    freq;        /* true = frequency scale, false = density (default) */
    /* labels (may be "") */
    char    title[128];
    char    xtitle[64];
    char    ytitle[64];
} PlotSpec;

/* Render the spec as a complete SVG document to `out`.
 * Deterministic for identical input (golden-file testable). */
void plot_render_svg(const PlotSpec *sp, FILE *out);

/* command handlers, registered in commands.c */
int do_scatter(Cmd *c);
int do_lineplot(Cmd *c);
int do_histogram(Cmd *c);

#endif

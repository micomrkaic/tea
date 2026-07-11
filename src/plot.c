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
/* plot.c — SVG plot renderer and the scatter / line / histogram commands.
 *
 * The renderer is deliberately small: fixed canvas, 1-2-5 "nice" ticks,
 * one series per plot.  Output is deterministic text, so regression tests
 * are plain diffs against golden SVG files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "plot.h"
#include "tsop.h"
#include "dataset.h"
#include "expr.h"
#include "value.h"
#include "interp.h"

/* ---- geometry (single fixed canvas; keep numbers integral) ------------- */
#define W       640
#define H       440
#define ML       70     /* left margin: y tick labels + y title  */
#define MR       20
#define MT       40     /* top margin: title                      */
#define MB       60     /* bottom margin: x tick labels + x title */
#define PW      (W-ML-MR)
#define PH      (H-MT-MB)

/* ---- error helper (same convention as commands.c) ---------------------- */
extern int g_current_line;       /* 0 == interactive REPL */
#include <stdarg.h>
__attribute__((format(__printf__,1,2)))
static void plot_err(const char *fmt, ...){
    if (g_current_line) fprintf(stderr, "line %d: ", g_current_line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#define tea_err plot_err

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Calls window-side hook if the page installed one (Module.teaPlot). */
EM_JS(void, tea_wasm_plot_notify, (const char *fname), {
    if (Module.teaPlot) Module.teaPlot(UTF8ToString(fname));
});
#endif

/* ======================================================================= */
/* nice ticks: choose a 1-2-5 step so that 4..8 ticks span [lo,hi].        */
/* ======================================================================= */
typedef struct { double lo, hi, step; int nt; } Axis;

static double nice_step(double range, int target)
{
    if (range <= 0) range = 1.0;
    double raw = range / target;
    double mag = pow(10.0, floor(log10(raw)));
    double m   = raw / mag;                 /* 1 <= m < 10 */
    double f   = (m < 1.5) ? 1 : (m < 3.5) ? 2 : (m < 7.5) ? 5 : 10;
    return f * mag;
}

static Axis axis_fit(double dmin, double dmax)
{
    Axis a;
    if (dmin == dmax) { dmin -= 0.5; dmax += 0.5; }        /* degenerate */
    a.step = nice_step(dmax - dmin, 5);
    a.lo   = floor(dmin / a.step) * a.step;
    a.hi   = ceil (dmax / a.step) * a.step;
    a.nt   = (int)lround((a.hi - a.lo) / a.step) + 1;
    return a;
}

/* format a tick label compactly; %.6g never prints trailing zeros */
static void tick_label(char *buf, size_t n, double v)
{
    if (fabs(v) < 1e-12) v = 0.0;          /* avoid "-0" */
    snprintf(buf, n, "%.6g", v);
}

/* map data coords -> pixel coords */
static double px(const Axis *ax, double v){ return ML + (v - ax->lo) / (ax->hi - ax->lo) * PW; }
static double py(const Axis *ay, double v){ return MT + PH - (v - ay->lo) / (ay->hi - ay->lo) * PH; }

/* minimal XML escaping for user-supplied titles */
static void xml_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 6 < n; s++) {
        switch (*s) {
        case '<': o += (size_t)snprintf(out+o, n-o, "&lt;");  break;
        case '>': o += (size_t)snprintf(out+o, n-o, "&gt;");  break;
        case '&': o += (size_t)snprintf(out+o, n-o, "&amp;"); break;
        case '"': o += (size_t)snprintf(out+o, n-o, "&quot;");break;
        default:  out[o++] = *s;
        }
    }
    out[o] = 0;
}

/* ======================================================================= */
/* renderer                                                                */
/* ======================================================================= */
static void svg_frame_open(FILE *o)
{
    fprintf(o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
               "viewBox=\"0 0 %d %d\" font-family=\"Helvetica,Arial,sans-serif\">\n",
               W, H, W, H);
    fprintf(o, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", W, H);
}

static void svg_axes(FILE *o, const Axis *ax, const Axis *ay, const PlotSpec *sp)
{
    char esc[256], lab[32];

    /* plot border */
    fprintf(o, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
               "fill=\"none\" stroke=\"black\" stroke-width=\"1\"/>\n", ML, MT, PW, PH);

    /* x ticks, gridlines, labels */
    for (int i = 0; i < ax->nt; i++) {
        double v = ax->lo + i * ax->step;
        double X = px(ax, v);
        fprintf(o, "<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" "
                   "stroke=\"#dddddd\" stroke-width=\"1\"/>\n", X, MT, X, MT+PH);
        fprintf(o, "<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" "
                   "stroke=\"black\" stroke-width=\"1\"/>\n", X, MT+PH, X, MT+PH+5);
        tick_label(lab, sizeof lab, v);
        fprintf(o, "<text x=\"%.2f\" y=\"%d\" text-anchor=\"middle\" "
                   "font-size=\"12\">%s</text>\n", X, MT+PH+20, lab);
    }
    /* y ticks, gridlines, labels */
    for (int i = 0; i < ay->nt; i++) {
        double v = ay->lo + i * ay->step;
        double Y = py(ay, v);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" "
                   "stroke=\"#dddddd\" stroke-width=\"1\"/>\n", ML, Y, ML+PW, Y);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" "
                   "stroke=\"black\" stroke-width=\"1\"/>\n", ML-5, Y, ML, Y);
        tick_label(lab, sizeof lab, v);
        fprintf(o, "<text x=\"%d\" y=\"%.2f\" text-anchor=\"end\" dominant-baseline=\"middle\" "
                   "font-size=\"12\">%s</text>\n", ML-9, Y, lab);
    }

    if (sp->title[0]) {
        xml_escape(sp->title, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                   "font-size=\"16\" font-weight=\"bold\">%s</text>\n", W/2, 24, esc);
    }
    if (sp->xtitle[0]) {
        xml_escape(sp->xtitle, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                   "font-size=\"13\">%s</text>\n", ML+PW/2, H-14, esc);
    }
    if (sp->ytitle[0]) {
        xml_escape(sp->ytitle, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%.1f\" text-anchor=\"middle\" font-size=\"13\" "
                   "transform=\"rotate(-90 18 %.1f)\">%s</text>\n",
                   18, MT+PH/2.0, MT+PH/2.0, esc);
    }
}

static void render_scatter(FILE *o, const PlotSpec *sp, const Axis *ax, const Axis *ay)
{
    for (size_t i = 0; i < sp->n; i++)
        fprintf(o, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"3\" "
                   "fill=\"#1f4e79\" fill-opacity=\"0.75\"/>\n",
                   px(ax, sp->x[i]), py(ay, sp->y[i]));
}

static void render_line(FILE *o, const PlotSpec *sp, const Axis *ax, const Axis *ay)
{
    fprintf(o, "<polyline fill=\"none\" stroke=\"#1f4e79\" stroke-width=\"1.5\" points=\"");
    for (size_t i = 0; i < sp->n; i++)
        fprintf(o, "%s%.2f,%.2f", i ? " " : "", px(ax, sp->x[i]), py(ay, sp->y[i]));
    fprintf(o, "\"/>\n");
}

/* histogram: sp->x holds raw sample; bins/freq set by command layer */
static void render_hist(FILE *o, const PlotSpec *sp)
{
    size_t n = sp->n;
    double dmin = sp->x[0], dmax = sp->x[0];
    for (size_t i = 1; i < n; i++) {
        if (sp->x[i] < dmin) dmin = sp->x[i];
        if (sp->x[i] > dmax) dmax = sp->x[i];
    }
    int nb = sp->bins;
    if (nb <= 0) { nb = (int)ceil(sqrt((double)n)); if (nb > 50) nb = 50; }
    if (nb < 1) nb = 1;
    if (dmax == dmin) dmax = dmin + 1.0;
    double bw = (dmax - dmin) / nb;

    double *cnt = calloc((size_t)nb, sizeof *cnt);
    if (!cnt) { tea_err("histogram: out of memory\n"); return; }
    for (size_t i = 0; i < n; i++) {
        int b = (int)((sp->x[i] - dmin) / bw);
        if (b >= nb) b = nb - 1;                 /* max value into last bin */
        if (b < 0)  b = 0;
        cnt[b] += 1.0;
    }
    /* density (default) or frequency */
    double ymax = 0;
    for (int b = 0; b < nb; b++) {
        if (!sp->freq) cnt[b] /= (double)n * bw;
        if (cnt[b] > ymax) ymax = cnt[b];
    }

    Axis ax = axis_fit(dmin, dmax);
    Axis ay = axis_fit(0, ymax);
    ay.lo = 0;                                    /* histograms start at 0 */
    ay.nt = (int)lround(ay.hi / ay.step) + 1;

    svg_axes(o, &ax, &ay, sp);
    for (int b = 0; b < nb; b++) {
        double x0 = px(&ax, dmin + b * bw);
        double x1 = px(&ax, dmin + (b + 1) * bw);
        double y0 = py(&ay, cnt[b]);
        double y1 = py(&ay, 0);
        fprintf(o, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" "
                   "fill=\"#1f4e79\" fill-opacity=\"0.8\" stroke=\"white\" "
                   "stroke-width=\"0.5\"/>\n", x0, y0, x1 - x0, y1 - y0);
    }
    free(cnt);
}

void plot_render_svg(const PlotSpec *sp, FILE *out)
{
    svg_frame_open(out);
    if (sp->kind == PK_HIST) {
        render_hist(out, sp);
    } else {
        double xmin = sp->x[0], xmax = sp->x[0], ymin = sp->y[0], ymax = sp->y[0];
        for (size_t i = 1; i < sp->n; i++) {
            if (sp->x[i] < xmin) xmin = sp->x[i];
            if (sp->x[i] > xmax) xmax = sp->x[i];
            if (sp->y[i] < ymin) ymin = sp->y[i];
            if (sp->y[i] > ymax) ymax = sp->y[i];
        }
        Axis ax = axis_fit(xmin, xmax);
        Axis ay = axis_fit(ymin, ymax);
        svg_axes(out, &ax, &ay, sp);
        if (sp->kind == PK_SCATTER) render_scatter(out, sp, &ax, &ay);
        else                        render_line   (out, sp, &ax, &ay);
    }
    fprintf(out, "</svg>\n");
}

/* ======================================================================= */
/* command layer                                                           */
/* ======================================================================= */

/* Collect filtered numeric observations for up to two variables.
 * Applies [if] and [in]; drops rows where any requested var is missing
 * (pairwise-complete, like Stata's twoway).  Returns n kept, or -1. */
static long collect(Cmd *c, int vy, int vx, double **outy, double **outx)
{
    Frame *f = c->f;
    const char *perr;
    Node *ifn = NULL;
    if (c->ifexp[0]) {
        ifn = expr_parse(c->ifexp, f, &perr);
        if (!ifn) { tea_err("if error: %s\n", perr); return -1; }
    }
    size_t lo = 0, hi = f->nobs;                       /* [lo,hi) */
    if (c->in_lo > 0) lo = (size_t)c->in_lo - 1;
    if (c->in_hi > 0 && (size_t)c->in_hi < hi) hi = (size_t)c->in_hi;

    double *ybuf = malloc(f->nobs * sizeof *ybuf);
    double *xbuf = (vx >= 0) ? malloc(f->nobs * sizeof *xbuf) : NULL;
    if (!ybuf || (vx >= 0 && !xbuf)) {
        free(ybuf); free(xbuf); node_free(ifn);
        tea_err("plot: out of memory\n"); return -1;
    }

    EvalCtx ec = {0}; ec.f = f;
    long n = 0;
    for (size_t i = lo; i < hi; i++) {
        if (ifn) {
            ec.i = i; ec.n = (long)i + 1; ec.N = (long)f->nobs;
            if (!expr_eval_bool(ifn, &ec)) continue;
        }
        double yv = f->vars[vy].num[i];
        if (sv_is_miss(yv)) continue;
        if (vx >= 0) {
            double xv = f->vars[vx].num[i];
            if (sv_is_miss(xv)) continue;
            xbuf[n] = xv;
        }
        ybuf[n++] = yv;
    }
    node_free(ifn);
    *outy = ybuf;
    if (outx) *outx = xbuf;
    return n;
}

/* resolve one numeric variable by name or complain */
static int num_var(Cmd *c, const char *name)
{
    int vi = var_find(c->f, name);
    if (vi < 0) {
        /* accept time-series operators (D.x, L2.x, ...) like estimators
         * and summarize do: expand through the shared tsop machinery,
         * which creates a temp variable we plot from */
        int *vs = NULL, ntemp = 0; const char *terr = NULL;
        int nv = tsop_expand_varlist(c->f, name, &vs, &ntemp, &terr);
        if (nv == 1) { vi = vs[0]; free(vs); return vi; }
        free(vs);
        tea_err("variable %s not found\n", name);
        return -1;
    }
    if (c->f->vars[vi].type != VT_NUM)
                                { tea_err("%s is a string variable\n", name); return -1; }
    return vi;
}

/* shared option handling + output for all three commands */
/* drop tsop temp vars appended after _nv0 (D.x etc. in plot varlists) */
#define PLOT_RETURN(rc) do { tsop_drop_temps(c->f, c->f->nvar - _nv0); return (rc); } while (0)

static int finish_plot(Cmd *c, PlotSpec *sp)
{
    char fname[512] = "";
    bool explicit_save = opt_value(c->options, "saving", fname, sizeof fname);
    if (!explicit_save) snprintf(fname, sizeof fname, "tea_graph.svg");

    opt_value(c->options, "title",  sp->title,  sizeof sp->title);
    opt_value(c->options, "xtitle", sp->xtitle, sizeof sp->xtitle);
    opt_value(c->options, "ytitle", sp->ytitle, sizeof sp->ytitle);

    FILE *o = fopen(fname, "w");
    if (!o) { tea_err("cannot write %s\n", fname); return 603; }
    plot_render_svg(sp, o);
    fclose(o);
    printf("(graph written to %s, n=%zu)\n", fname, sp->n);

    /* interactive REPL only: hand the file to the OS viewer.  Never in
     * do-files, so batch runs and the test suite stay deterministic. */
#ifdef __EMSCRIPTEN__
    /* browser build: tell the page a plot landed in MEMFS; the front-end
     * reads the SVG out and renders it inline. */
    tea_wasm_plot_notify(fname);
#else
    if (g_current_line == 0 && !explicit_save && !opt_present(c->options, "noview")) {
        char cmdbuf[600];
#ifdef __APPLE__
        snprintf(cmdbuf, sizeof cmdbuf, "open '%s' >/dev/null 2>&1 &", fname);
        if (system(cmdbuf) != 0) { /* viewer optional; ignore */ }
#else
        /* Pick a viewer, not an editor: xdg-open often routes SVG to
         * GIMP/Inkscape, which is hostile for a quick look.  Order:
         *   1. $TEA_VIEWER if set (user override, documented)
         *   2. lightweight image viewers
         *   3. a browser (renders SVG well)
         *   4. xdg-open as the last resort                          */
        const char *viewer = getenv("TEA_VIEWER");
        if (viewer && *viewer) {
            snprintf(cmdbuf, sizeof cmdbuf, "%s '%s' >/dev/null 2>&1 &", viewer, fname);
            if (system(cmdbuf) != 0) { /* ignore */ }
        } else {
            static const char *cands[] = { "eog", "feh", "ristretto", "xviewer",
                "sensible-browser", "x-www-browser", "firefox", "chromium",
                "xdg-open", NULL };
            for (int ci = 0; cands[ci]; ci++) {
                snprintf(cmdbuf, sizeof cmdbuf,
                         "command -v %s >/dev/null 2>&1", cands[ci]);
                if (system(cmdbuf) == 0) {
                    snprintf(cmdbuf, sizeof cmdbuf, "%s '%s' >/dev/null 2>&1 &",
                             cands[ci], fname);
                    if (system(cmdbuf) != 0) { /* ignore */ }
                    break;
                }
            }
        }
#endif /* __APPLE__ */
    }
#endif /* __EMSCRIPTEN__ */
    return 0;
}

/* scatter y x [if] [in] [, title() xtitle() ytitle() saving() noview] */
int do_scatter(Cmd *c)
{
    int _nv0 = c->f->nvar;
    char yn[33], xn[33], rest[8];
    if (sscanf(c->varlist, "%32s %32s %7s", yn, xn, rest) != 2) {
        tea_err("syntax: scatter yvar xvar [if] [in] [, options]\n"); PLOT_RETURN(198);
    }
    int vy = num_var(c, yn); if (vy < 0) PLOT_RETURN(111);
    int vx = num_var(c, xn); if (vx < 0) PLOT_RETURN(111);

    PlotSpec sp = {0}; sp.kind = PK_SCATTER;
    long n = collect(c, vy, vx, &sp.y, &sp.x);
    if (n < 0) PLOT_RETURN(198);
    if (n == 0) { free(sp.x); free(sp.y); tea_err("no observations\n"); PLOT_RETURN(2000); }
    sp.n = (size_t)n;
    snprintf(sp.xtitle, sizeof sp.xtitle, "%s", xn);   /* defaults; options may override */
    snprintf(sp.ytitle, sizeof sp.ytitle, "%s", yn);

    int rc = finish_plot(c, &sp);
    free(sp.x); free(sp.y);
    PLOT_RETURN(rc);
}

/* line y x [if] [in] [, sort title() ...]   connects in data order,
 * or in x order with the `sort` option. */
static int cmp_pair(const void *a, const void *b)
{
    const double *pa = a, *pb = b;
    return (pa[0] > pb[0]) - (pa[0] < pb[0]);
}

int do_lineplot(Cmd *c)
{
    int _nv0 = c->f->nvar;
    char yn[33], xn[33], rest[8];
    if (sscanf(c->varlist, "%32s %32s %7s", yn, xn, rest) != 2) {
        tea_err("syntax: line yvar xvar [if] [in] [, sort options]\n"); PLOT_RETURN(198);
    }
    int vy = num_var(c, yn); if (vy < 0) PLOT_RETURN(111);
    int vx = num_var(c, xn); if (vx < 0) PLOT_RETURN(111);

    PlotSpec sp = {0}; sp.kind = PK_LINE;
    long n = collect(c, vy, vx, &sp.y, &sp.x);
    if (n < 0) PLOT_RETURN(198);
    if (n == 0) { free(sp.x); free(sp.y); tea_err("no observations\n"); PLOT_RETURN(2000); }
    sp.n = (size_t)n;

    if (opt_present(c->options, "sort")) {
        /* pack, sort by x, unpack — stable enough for plotting */
        double *pairs = malloc(sp.n * 2 * sizeof *pairs);
        if (!pairs) { free(sp.x); free(sp.y); tea_err("line: out of memory\n"); PLOT_RETURN(198); }
        for (size_t i = 0; i < sp.n; i++) { pairs[2*i] = sp.x[i]; pairs[2*i+1] = sp.y[i]; }
        qsort(pairs, sp.n, 2 * sizeof(double), cmp_pair);
        for (size_t i = 0; i < sp.n; i++) { sp.x[i] = pairs[2*i]; sp.y[i] = pairs[2*i+1]; }
        free(pairs);
    }
    snprintf(sp.xtitle, sizeof sp.xtitle, "%s", xn);
    snprintf(sp.ytitle, sizeof sp.ytitle, "%s", yn);

    int rc = finish_plot(c, &sp);
    free(sp.x); free(sp.y);
    PLOT_RETURN(rc);
}

/* histogram var [if] [in] [, bins(#) freq title() ...] */
int do_histogram(Cmd *c)
{
    int _nv0 = c->f->nvar;
    char vn[33], rest[8];
    if (sscanf(c->varlist, "%32s %7s", vn, rest) != 1) {
        tea_err("syntax: histogram var [if] [in] [, bins(#) freq options]\n"); PLOT_RETURN(198);
    }
    int vi = num_var(c, vn); if (vi < 0) PLOT_RETURN(111);

    PlotSpec sp = {0}; sp.kind = PK_HIST;
    long n = collect(c, vi, -1, &sp.x, NULL);          /* sample lands in x */
    if (n < 0) PLOT_RETURN(198);
    if (n == 0) { free(sp.x); tea_err("no observations\n"); PLOT_RETURN(2000); }
    sp.n = (size_t)n;

    char bopt[16];
    if (opt_value(c->options, "bins", bopt, sizeof bopt)) {
        sp.bins = atoi(bopt);
        if (sp.bins < 1 || sp.bins > 1000) { free(sp.x); tea_err("bins() must be 1..1000\n"); PLOT_RETURN(198); }
    }
    sp.freq = opt_present(c->options, "freq");
    snprintf(sp.xtitle, sizeof sp.xtitle, "%s", vn);
    snprintf(sp.ytitle, sizeof sp.ytitle, "%s", sp.freq ? "Frequency" : "Density");

    int rc = finish_plot(c, &sp);
    free(sp.x);
    PLOT_RETURN(rc);
}

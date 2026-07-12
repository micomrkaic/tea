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
/* graph.c — see graph.h.  Layout:
 *   1. geometry + small helpers (a few duplicated from plot.c on purpose:
 *      refactoring plot.c would risk its golden test outputs for ~40 lines)
 *   2. paren-aware option scanner (opt_value stops at the first match;
 *      graph box legitimately has TWO over() options)
 *   3. per-series data collection with per-series `if`
 *   4. lowess (locally weighted regression, Stata defaults)
 *   5. twoway spec + renderer
 *   6. graph box (two-level over(), Stata layout)
 *   7. named-graph registry + graph combine
 *   8. command parsers
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "graph.h"
#include "dataset.h"
#include "expr.h"
#include "value.h"
#include "interp.h"

/* ---- geometry ----------------------------------------------------------- */
#define GW      640
#define GH      440
#define GMR      20
#define GMT      40

extern int g_current_line;
#include <stdarg.h>
__attribute__((format(__printf__,1,2)))
static void graph_err(const char *fmt, ...){
    if (g_current_line) fprintf(stderr, "line %d: ", g_current_line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#define tea_err graph_err

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, tea_wasm_graph_notify, (const char *fname), {
    if (Module.teaPlot) Module.teaPlot(UTF8ToString(fname));
});
#endif

/* ---- axis fitting (mirrors plot.c so both stay byte-stable) ------------ */
typedef struct { double lo, hi, step; int nt; } GAxis;

static double g_nice_step(double range, int target){
    if (range <= 0) range = 1.0;
    double raw = range / target;
    double mag = pow(10.0, floor(log10(raw)));
    double m   = raw / mag;
    double f   = (m < 1.5) ? 1 : (m < 3.5) ? 2 : (m < 7.5) ? 5 : 10;
    return f * mag;
}
static GAxis g_axis_fit(double dmin, double dmax){
    GAxis a;
    if (dmin == dmax) { dmin -= 0.5; dmax += 0.5; }
    a.step = g_nice_step(dmax - dmin, 5);
    a.lo   = floor(dmin / a.step) * a.step;
    a.hi   = ceil (dmax / a.step) * a.step;
    a.nt   = (int)lround((a.hi - a.lo) / a.step) + 1;
    return a;
}
static void g_tick_label(char *buf, size_t n, double v){
    if (fabs(v) < 1e-12) v = 0.0;
    snprintf(buf, n, "%.6g", v);
}
static void g_xml_escape(const char *s, char *out, size_t n){
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
/* deterministic coordinate: 2 decimals, -0.00 snapped to 0.00 */
static double snap2(double v){
    double r = round(v * 100.0) / 100.0;
    if (r == 0.0) r = 0.0;            /* kill -0 */
    return r;
}

/* ---- paren-aware option scanner ----------------------------------------
 * Iterates (name, argument) pairs over an option string.  `noout` yields
 * arg="".  `over(decade, label(angle(45)))` yields the full nested text.
 * Returns 1 and advances *cursor, or 0 at end. */
static int opt_next(const char **cursor, char *name, size_t nn, char *arg, size_t na){
    const char *p = *cursor;
    while (*p==' ' || *p==',') p++;
    if (!*p) { *cursor = p; return 0; }
    size_t w = 0;
    while (*p && *p!=' ' && *p!='(' && *p!=',' && w+1<nn) name[w++]=*p++;
    name[w]=0; arg[0]=0;
    if (*p=='(') {
        p++; int d=1; size_t a=0; int inq=0;
        while (*p && d) {
            if (*p=='"') inq=!inq;
            else if (!inq && *p=='(') d++;
            else if (!inq && *p==')') { d--; if(!d){p++;break;} }
            if (a+1<na) arg[a++]=*p;
            p++;
        }
        arg[a]=0;
    }
    *cursor = p;
    return name[0] != 0;
}
/* find the Nth (0-based) occurrence of option `want`; 1 on success */
static int opt_find_nth(const char *opts, const char *want, int nth,
                        char *arg, size_t na){
    const char *cur = opts; char nm[64]; int seen = 0;
    while (opt_next(&cur, nm, sizeof nm, arg, na))
        if (!strcmp(nm, want) && seen++ == nth) return 1;
    arg[0]=0; return 0;
}
/* strip surrounding plain quotes from an option argument, in place */
static void unquote(char *s){
    size_t L = strlen(s);
    while (L>0 && s[0]==' ') { memmove(s,s+1,L--); }
    while (L>0 && s[L-1]==' ') s[--L]=0;
    if (L>=2 && s[0]=='"' && s[L-1]=='"') { memmove(s, s+1, L-2); s[L-2]=0; }
}

/* ---- colors -------------------------------------------------------------
 * Named Stata colors -> hex; unknown names pass through (SVG may know
 * them).  Default multi-series palette is fixed for determinism. */
static const char *color_hex(const char *nm){
    static const struct { const char *n, *h; } tab[] = {
        {"blue","#1f4e79"}, {"red","#c0392b"}, {"green","#1e8449"},
        {"black","#000000"}, {"orange","#ca6f1e"}, {"purple","#7d3c98"},
        {"gray","#7f8c8d"}, {"grey","#7f8c8d"}, {"navy","#1a5276"},
        {"maroon","#78281f"}, {"cyan","#148f9f"}, {"magenta","#a03972"},
        {"white","#ffffff"}, {NULL,NULL} };
    for (int i=0; tab[i].n; i++) if(!strcmp(nm,tab[i].n)) return tab[i].h;
    return nm;
}
static const char *palette(int i){
    static const char *pal[] = {"#1f4e79","#c0392b","#1e8449","#ca6f1e",
                                "#7d3c98","#148f9f","#a03972","#7f8c8d"};
    return pal[i % 8];
}
static const char *dash_for(const char *pat){
    if(!strcmp(pat,"dot"))       return "2,4";
    if(!strcmp(pat,"dash"))      return "8,4";
    if(!strcmp(pat,"shortdash")) return "4,3";
    if(!strcmp(pat,"longdash"))  return "12,4";
    if(!strcmp(pat,"dash_dot"))  return "8,4,2,4";
    return "";                    /* solid */
}

/* ---- series model ------------------------------------------------------- */
typedef enum { GS_SCATTER, GS_LINE, GS_LOWESS } GSKind;

typedef struct {
    GSKind kind;
    double *x, *y; size_t n;
    char **mlab;                  /* marker labels (scatter), or NULL */
    char  color[48];              /* resolved (hex or passthrough)    */
    char  lpattern[16];
    int   msym_none;              /* msymbol(i): labels only          */
    int   mlabpos;                /* clock position, default 3        */
    char  mlabcolor[48];
    char  legend_label[64];       /* yvar name, for the legend        */
    /* lowess parameters (computed into x/y before rendering) */
    double bwidth; int lw_mean, lw_adjust;
} GSeries;

typedef struct {
    GSeries s[32]; int ns;
    char title[128], xtitle[64], ytitle[64], note[128];
    int  legend_off;
    int    has_yscale; double ysc_lo, ysc_hi;
    int    has_ylab;   double ylab_lo, ylab_step, ylab_hi;
    int    has_xlab;   double xlab_lo, xlab_step, xlab_hi;
    double yline_v[8]; char yline_pat[8][16]; char yline_col[8][48]; int nyline;
} GSpec;

static void gspec_free(GSpec *g){
    for (int i=0; i<g->ns; i++){
        free(g->s[i].x); free(g->s[i].y);
        if (g->s[i].mlab){ for(size_t r=0;r<g->s[i].n;r++) free(g->s[i].mlab[r]); free(g->s[i].mlab); }
    }
}

/* ---- per-series data collection -----------------------------------------
 * Evaluate the optional if-expression per row; keep pairwise-complete
 * (x,y) pairs; optionally collect a string label variable alongside. */
static long collect_series(Frame *f, const char *yv, const char *xv,
                           const char *ifexp, const char *mlabvar,
                           double **ox, double **oy, char ***omlab){
    int yi = var_find(f, yv), xi = var_find(f, xv);
    if (yi < 0){ tea_err("twoway: variable %s not found\n", yv); return -1; }
    if (xi < 0){ tea_err("twoway: variable %s not found\n", xv); return -1; }
    if (f->vars[yi].type != VT_NUM || f->vars[xi].type != VT_NUM){
        tea_err("twoway: %s and %s must be numeric\n", yv, xv); return -1; }
    int li = -1;
    if (mlabvar && mlabvar[0]){
        li = var_find(f, mlabvar);
        if (li < 0){ tea_err("twoway: mlabel variable %s not found\n", mlabvar); return -1; }
    }
    Node *ifn = NULL;
    if (ifexp && ifexp[0]){
        const char *perr;
        ifn = expr_parse(ifexp, f, &perr);
        if (!ifn){ tea_err("twoway: bad if expression: %s\n", perr?perr:ifexp); return -1; }
    }
    double *X = malloc(f->nobs * sizeof *X), *Y = malloc(f->nobs * sizeof *Y);
    char **L = (li>=0) ? calloc(f->nobs, sizeof *L) : NULL;
    size_t n = 0;
    for (size_t r = 0; r < f->nobs; r++){
        if (ifn){
            EvalCtx ec = {0}; ec.f = f; ec.i = (long)r; ec.n = (long)r+1; ec.N = (long)f->nobs;
            EVal v = expr_eval(ifn, &ec);
            int keep = !v.is_str && !sv_is_miss(v.num) && v.num != 0;
            eval_free(&v);
            if (!keep) continue;
        }
        double xv2 = f->vars[xi].num[r], yv2 = f->vars[yi].num[r];
        if (sv_is_miss(xv2) || sv_is_miss(yv2)) continue;
        X[n]=xv2; Y[n]=yv2;
        if (L){ const char *t = (f->vars[li].type==VT_STR && f->vars[li].str[r]) ? f->vars[li].str[r] : "";
                L[n]=strdup(t); }
        n++;
    }
    if (ifn) node_free(ifn);
    *ox = X; *oy = Y; if (omlab) *omlab = L;
    return (long)n;
}

/* sort a series by x (labels ride along).  Stable insertion sort keeps
 * equal-x points in data order — deterministic. */
static void series_sort_x(double *x, double *y, char **lab, size_t n){
    for (size_t i = 1; i < n; i++){
        double kx=x[i], ky=y[i]; char *kl = lab? lab[i] : NULL;
        size_t j = i;
        while (j > 0 && x[j-1] > kx){
            x[j]=x[j-1]; y[j]=y[j-1]; if(lab) lab[j]=lab[j-1]; j--;
        }
        x[j]=kx; y[j]=ky; if(lab) lab[j]=kl;
    }
}

/* ---- lowess --------------------------------------------------------------
 * Stata's lowess: locally weighted regression of y on x, tricube kernel,
 * default bandwidth 0.8 (fraction of the sample in each local window),
 * running-LINE (local linear) by default, `mean` for running mean,
 * `adjust` rescales so mean(smooth) == mean(y).  Input must be x-sorted;
 * output y is replaced by the smooth at each x. */
static void lowess_smooth(double *x, double *y, size_t n,
                          double bwidth, int use_mean, int adjust){
    if (n < 2) return;
    long k = (long)floor(bwidth * (double)n);
    if (k < 2) k = 2;
    if (k > (long)n) k = (long)n;
    double *ys = malloc(n * sizeof *ys);
    for (size_t i = 0; i < n; i++){
        /* window: the k points nearest x[i] (two-pointer over sorted x) */
        long lo = (long)i, hi = (long)i;
        while (hi - lo + 1 < k){
            if (lo == 0) hi++;
            else if (hi == (long)n-1) lo--;
            else if (x[i]-x[lo-1] <= x[hi+1]-x[i]) lo--;
            else hi++;
        }
        double dmax = fmax(x[i]-x[lo], x[hi]-x[i]);
        if (dmax <= 0) dmax = 1.0;
        double sw=0, swx=0, swy=0, swxx=0, swxy=0;
        for (long j = lo; j <= hi; j++){
            double u = fabs(x[j]-x[i]) / dmax;
            double w = (u >= 1.0) ? 0.0 : pow(1.0 - u*u*u, 3.0);   /* tricube */
            sw += w; swx += w*x[j]; swy += w*y[j];
            swxx += w*x[j]*x[j]; swxy += w*x[j]*y[j];
        }
        if (sw <= 0){ ys[i] = y[i]; continue; }
        if (use_mean){ ys[i] = swy / sw; }
        else {
            double den = sw*swxx - swx*swx;
            if (fabs(den) < 1e-12 * fmax(1.0, sw*swxx)) ys[i] = swy / sw;
            else {
                double b = (sw*swxy - swx*swy) / den;
                double a = (swy - b*swx) / sw;
                ys[i] = a + b*x[i];
            }
        }
    }
    if (adjust){
        double my=0, ms=0;
        for (size_t i=0;i<n;i++){ my += y[i]; ms += ys[i]; }
        if (fabs(ms) > 1e-12){ double f = my/ms; for(size_t i=0;i<n;i++) ys[i]*=f; }
    }
    memcpy(y, ys, n * sizeof *ys);
    free(ys);
}

/* ---- twoway renderer ----------------------------------------------------- */
static void mlab_offset(int pos, double *dx, double *dy, const char **anchor){
    /* clock positions; 3 o'clock default */
    switch (pos){
    case 12: *dx=0;  *dy=-8;  *anchor="middle"; break;
    case  1: case 2: *dx=7; *dy=-6; *anchor="start"; break;
    case  3: *dx=8;  *dy=4;   *anchor="start";  break;
    case  4: case 5: *dx=7; *dy=12; *anchor="start"; break;
    case  6: *dx=0;  *dy=14;  *anchor="middle"; break;
    case  7: case 8: *dx=-7; *dy=12; *anchor="end"; break;
    case  9: *dx=-8; *dy=4;   *anchor="end";    break;
    case 10: case 11: *dx=-7; *dy=-6; *anchor="end"; break;
    default: *dx=8;  *dy=4;   *anchor="start";  break;
    }
}

static void render_twoway(const GSpec *g, FILE *o){
    /* margins: bottom grows if a note is present */
    int ML = 70, MB = g->note[0] ? 74 : 60;
    int PW = GW - ML - GMR, PH = GH - GMT - MB;
    double pxlo, pxhi, pylo, pyhi;

    /* data extent over all series */
    double xmin=0, xmax=0, ymin=0, ymax=0; int first=1;
    for (int s=0; s<g->ns; s++) for (size_t i=0;i<g->s[s].n;i++){
        double X=g->s[s].x[i], Y=g->s[s].y[i];
        if (first){ xmin=xmax=X; ymin=ymax=Y; first=0; }
        if (X<xmin)xmin=X; if(X>xmax)xmax=X;
        if (Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
    }
    if (first){ xmin=0; xmax=1; ymin=0; ymax=1; }
    for (int i=0;i<g->nyline;i++){ if(g->yline_v[i]<ymin)ymin=g->yline_v[i]; if(g->yline_v[i]>ymax)ymax=g->yline_v[i]; }

    /* Ticks and plot RANGE are separate concerns (Stata semantics):
     * xlabel/ylabel rules fix where the ticks sit; the range is the
     * union of the tick span, the data, yscale(range()), and any
     * ylines — a rule never clips data outside the plot box. */
    GAxis ax = g_axis_fit(xmin, xmax);
    GAxis ay = g_axis_fit(ymin, ymax);
    if (g->has_xlab){ ax.lo=g->xlab_lo; ax.hi=g->xlab_hi; ax.step=g->xlab_step;
        ax.nt=(int)lround((ax.hi-ax.lo)/ax.step)+1; }
    if (g->has_ylab){ ay.lo=g->ylab_lo; ay.hi=g->ylab_hi; ay.step=g->ylab_step;
        ay.nt=(int)lround((ay.hi-ay.lo)/ay.step)+1; }
    pxlo = fmin(ax.lo, xmin); pxhi = fmax(ax.hi, xmax);
    pylo = fmin(ay.lo, ymin); pyhi = fmax(ay.hi, ymax);
    if (g->has_yscale){
        if (g->ysc_lo < pylo) pylo = g->ysc_lo;
        if (g->ysc_hi > pyhi) pyhi = g->ysc_hi;
    }
    if (pxhi <= pxlo) pxhi = pxlo + 1;
    if (pyhi <= pylo) pyhi = pylo + 1;
    #define PX(v) snap2(ML + ((v)-pxlo)/(pxhi-pxlo)*PW)
    #define PY(v) snap2(GMT + PH - ((v)-pylo)/(pyhi-pylo)*PH)

    fprintf(o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
               "viewBox=\"0 0 %d %d\" font-family=\"Helvetica,Arial,sans-serif\">\n",
               GW, GH, GW, GH);
    fprintf(o, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", GW, GH);
    fprintf(o, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
               "fill=\"none\" stroke=\"black\" stroke-width=\"1\"/>\n", ML, GMT, PW, PH);

    char lab[32], esc[256];
    for (int i=0;i<ax.nt;i++){
        double v = ax.lo + i*ax.step, X = PX(v);
        fprintf(o, "<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" stroke=\"#dddddd\" stroke-width=\"1\"/>\n", X, GMT, X, GMT+PH);
        fprintf(o, "<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" stroke=\"black\" stroke-width=\"1\"/>\n", X, GMT+PH, X, GMT+PH+5);
        g_tick_label(lab, sizeof lab, v);
        fprintf(o, "<text x=\"%.2f\" y=\"%d\" text-anchor=\"middle\" font-size=\"12\">%s</text>\n", X, GMT+PH+20, lab);
    }
    for (int i=0;i<ay.nt;i++){
        double v = ay.lo + i*ay.step, Y = PY(v);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"#dddddd\" stroke-width=\"1\"/>\n", ML, Y, ML+PW, Y);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", ML-5, Y, ML, Y);
        g_tick_label(lab, sizeof lab, v);
        fprintf(o, "<text x=\"%d\" y=\"%.2f\" text-anchor=\"end\" dominant-baseline=\"middle\" font-size=\"12\">%s</text>\n", ML-9, Y, lab);
    }

    /* reference ylines */
    for (int i=0;i<g->nyline;i++){
        double Y = PY(g->yline_v[i]);
        const char *dash = dash_for(g->yline_pat[i]);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"%s\" stroke-width=\"1\"%s%s%s/>\n",
                ML, Y, ML+PW, Y,
                g->yline_col[i][0] ? g->yline_col[i] : "black",
                dash[0]?" stroke-dasharray=\"":"", dash, dash[0]?"\"":"");
    }

    /* series */
    for (int s=0; s<g->ns; s++){
        const GSeries *S = &g->s[s];
        const char *col = S->color[0] ? S->color : palette(s);
        if (S->kind == GS_LINE || S->kind == GS_LOWESS){
            if (S->n >= 2){
                const char *dash = dash_for(S->lpattern);
                fprintf(o, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"1.5\"%s%s%s points=\"", col,
                        dash[0]?" stroke-dasharray=\"":"", dash, dash[0]?"\"":"");
                for (size_t i=0;i<S->n;i++)
                    fprintf(o, "%s%.2f,%.2f", i?" ":"", PX(S->x[i]), PY(S->y[i]));
                fprintf(o, "\"/>\n");
            }
        } else {
            for (size_t i=0;i<S->n;i++){
                double X=PX(S->x[i]), Y=PY(S->y[i]);
                if (!S->msym_none)
                    fprintf(o, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"%s\" fill-opacity=\"0.75\"/>\n", X, Y, col);
                if (S->mlab && S->mlab[i] && S->mlab[i][0]){
                    double dx,dy; const char *anch;
                    mlab_offset(S->mlabpos, &dx, &dy, &anch);
                    g_xml_escape(S->mlab[i], esc, sizeof esc);
                    fprintf(o, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"%s\" font-size=\"11\" fill=\"%s\">%s</text>\n",
                            snap2(X+dx), snap2(Y+dy), anch,
                            S->mlabcolor[0] ? S->mlabcolor : col, esc);
                }
            }
        }
    }

    /* legend: simple top-right swatch list, on unless legend(off) */
    if (!g->legend_off && g->ns > 1){
        int lh = 16, lw = 130;
        int lx = ML + PW - lw - 6, ly = GMT + 6;
        fprintf(o, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"white\" fill-opacity=\"0.85\" stroke=\"#999999\" stroke-width=\"0.5\"/>\n",
                lx, ly, lw, lh*g->ns + 8);
        for (int s=0;s<g->ns;s++){
            const char *col = g->s[s].color[0] ? g->s[s].color : palette(s);
            int yy = ly + 12 + s*lh;
            fprintf(o, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"%s\" stroke-width=\"2\"/>\n",
                    lx+6, yy-4, lx+24, yy-4, col);
            g_xml_escape(g->s[s].legend_label, esc, sizeof esc);
            fprintf(o, "<text x=\"%d\" y=\"%d\" font-size=\"11\">%s</text>\n", lx+30, yy, esc);
        }
    }

    if (g->title[0]){
        g_xml_escape(g->title, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" font-size=\"16\" font-weight=\"bold\">%s</text>\n", GW/2, 24, esc);
    }
    if (g->xtitle[0]){
        g_xml_escape(g->xtitle, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" font-size=\"13\">%s</text>\n", ML+PW/2, GMT+PH+40, esc);
    }
    if (g->ytitle[0]){
        g_xml_escape(g->ytitle, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%.1f\" text-anchor=\"middle\" font-size=\"13\" transform=\"rotate(-90 18 %.1f)\">%s</text>\n",
                18, GMT+PH/2.0, GMT+PH/2.0, esc);
    }
    if (g->note[0]){
        g_xml_escape(g->note, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" font-size=\"10\" fill=\"#555555\">%s</text>\n", ML, GH-6, esc);
    }
    fprintf(o, "</svg>\n");
    #undef PX
    #undef PY
}

/* ======================================================================== */
/* graph box                                                                */
/* ======================================================================== */
typedef struct {
    double med, q1, q3, wlo, whi;      /* box + adjacent-value whiskers   */
    double *out; size_t nout;          /* outside values                  */
    size_t n;                          /* obs in cell (0 = empty cell)    */
} BoxStat;

static int cmp_dbl(const void *a, const void *b){
    double x = *(const double*)a, y = *(const double*)b;
    return (x>y) - (x<y);
}
/* Stata's pctile: linear interpolation on the order statistics */
static double pct_interp(const double *v, size_t n, double p){
    if (n == 1) return v[0];
    double idx = p * (double)(n + 1) / 100.0;   /* Stata definition */
    if (idx <= 1) return v[0];
    if (idx >= (double)n) return v[n-1];
    size_t lo = (size_t)floor(idx);
    double fr = idx - (double)lo;
    return v[lo-1] + fr * (v[lo] - v[lo-1]);
}
static void box_stats(double *v, size_t n, BoxStat *b){
    memset(b, 0, sizeof *b);
    b->n = n;
    if (!n) return;
    qsort(v, n, sizeof *v, cmp_dbl);
    b->q1  = pct_interp(v, n, 25);
    b->med = pct_interp(v, n, 50);
    b->q3  = pct_interp(v, n, 75);
    double iqr = b->q3 - b->q1;
    double fence_lo = b->q1 - 1.5*iqr, fence_hi = b->q3 + 1.5*iqr;
    /* adjacent values: most extreme observations inside the fences */
    b->wlo = b->q1; b->whi = b->q3;
    for (size_t i=0;i<n;i++)   if (v[i] >= fence_lo){ b->wlo = v[i]; break; }
    for (size_t i=n;i-- > 0;)  if (v[i] <= fence_hi){ b->whi = v[i]; break; }
    b->out = malloc(n * sizeof *b->out); b->nout = 0;
    for (size_t i=0;i<n;i++)
        if (v[i] < fence_lo || v[i] > fence_hi) b->out[b->nout++] = v[i];
}

/* categorical level set of a variable (numeric or string), sorted */
typedef struct { double nval; char *sval; char *label; } GLevel;
static int lev_cmp(const void *a, const void *b){
    const GLevel *x=a, *y=b;
    if (x->sval && y->sval) return strcmp(x->sval, y->sval);
    return (x->nval > y->nval) - (x->nval < y->nval);
}
static const char *vallab_text(Workspace *ws, Variable *v, double val){
    if (!v->vallab[0]) return NULL;
    VLabel *L = vlabel_get(ws, v->vallab);
    if (!L) return NULL;
    for (VLItem *it = L->items; it; it = it->next)
        if (it->val == val) return it->txt;
    return NULL;
}
static int levels_of(Workspace *ws, Frame *f, int vi, const unsigned char *mask,
                     GLevel **out){
    Variable *v = &f->vars[vi];
    GLevel *L = NULL; int nl = 0, cap = 0;
    for (size_t r = 0; r < f->nobs; r++){
        if (mask && !mask[r]) continue;
        double nv = 0; const char *sv = NULL;
        if (v->type == VT_NUM){ nv = v->num[r]; if (sv_is_miss(nv)) continue; }
        else { sv = v->str[r] ? v->str[r] : ""; if(!sv[0]) continue; }
        int found = 0;
        for (int i = 0; i < nl; i++){
            if (v->type == VT_NUM ? (L[i].nval == nv) : !strcmp(L[i].sval, sv)){ found = 1; break; }
        }
        if (found) continue;
        if (nl == cap){ cap = cap ? cap*2 : 16; L = realloc(L, (size_t)cap * sizeof *L); }
        L[nl].nval = nv;
        L[nl].sval = sv ? strdup(sv) : NULL;
        L[nl].label = NULL;
        nl++;
    }
    qsort(L, (size_t)nl, sizeof *L, lev_cmp);
    for (int i = 0; i < nl; i++){
        char buf[64];
        const char *t = NULL;
        if (v->type == VT_NUM) t = vallab_text(ws, v, L[i].nval);
        if (!t){
            if (v->type == VT_STR) t = L[i].sval;
            else { g_tick_label(buf, sizeof buf, L[i].nval); t = buf; }
        }
        L[i].label = strdup(t);
    }
    *out = L;
    return nl;
}
static void levels_free(GLevel *L, int nl){
    for (int i=0;i<nl;i++){ free(L[i].sval); free(L[i].label); }
    free(L);
}
static int level_index(GLevel *L, int nl, Variable *v, size_t r){
    if (v->type == VT_NUM){
        double x = v->num[r]; if (sv_is_miss(x)) return -1;
        for (int i=0;i<nl;i++) if (L[i].nval == x) return i;
    } else {
        const char *s = v->str[r] ? v->str[r] : ""; if(!s[0]) return -1;
        for (int i=0;i<nl;i++) if (!strcmp(L[i].sval, s)) return i;
    }
    return -1;
}

/* over() suboptions: relabel(1 "80s" 2 "90s" ...), label(angle(#) labsize(SZ)) */
typedef struct { int angle; int fontsz; } OverStyle;
static void parse_over_sub(const char *sub, GLevel *L, int nl, OverStyle *st){
    st->angle = 0; st->fontsz = 12;
    const char *cur = sub; char nm[64], arg[512];
    while (opt_next(&cur, nm, sizeof nm, arg, sizeof arg)){
        if (!strcmp(nm, "relabel")){
            const char *p = arg;
            while (*p){
                while (*p==' ') p++;
                char *e; long idx = strtol(p, &e, 10);
                if (e == p) break;
                p = e; while (*p==' ') p++;
                if (*p != '"') break;
                p++;
                char txt[64]; size_t w=0;
                while (*p && *p!='"' && w+1<sizeof txt) txt[w++]=*p++;
                txt[w]=0; if (*p=='"') p++;
                if (idx >= 1 && idx <= nl){ free(L[idx-1].label); L[idx-1].label = strdup(txt); }
            }
        } else if (!strcmp(nm, "label")){
            const char *c2 = arg; char n2[64], a2[256];
            while (opt_next(&c2, n2, sizeof n2, a2, sizeof a2)){
                if (!strcmp(n2, "angle")) st->angle = atoi(a2);
                else if (!strcmp(n2, "labsize")){
                    if (!strcmp(a2,"vsmall")) st->fontsz = 8;
                    else if (!strcmp(a2,"small")) st->fontsz = 10;
                    else if (!strcmp(a2,"medsmall")) st->fontsz = 11;
                    else if (!strcmp(a2,"medium")) st->fontsz = 12;
                    else if (!strcmp(a2,"large")) st->fontsz = 14;
                    else if (!strcmp(a2,"vlarge")) st->fontsz = 16;
                }
                /* other label() suboptions: cosmetic, accepted-and-ignored */
            }
        }
        /* other over() suboptions (sort, gap, ...): accepted-and-ignored */
    }
}

/* render grouped box plot.
 * inner  = FIRST over() (varies fastest), outer = SECOND over() (bands). */
static void render_graph_box(Workspace *ws, Frame *f, int yv,
                             const unsigned char *mask,
                             int in_vi,  GLevel *inL,  int nin,  OverStyle *inSt,
                             int out_vi, GLevel *outL, int nout_, OverStyle *outSt,
                             int hide_outliers,
                             const char *title, const char *note, FILE *o){
    (void)ws;
    int ncell = nin * (nout_ ? nout_ : 1);
    int nband = nout_ ? nout_ : 1;

    /* bottom margin: inner labels (rotated?) + outer band labels */
    int in_h  = inSt->angle ? 34 : 18;
    int MB2   = 24 + in_h + (nout_ ? 20 : 0) + (note && note[0] ? 14 : 0);
    int ML2 = 70;
    int PW2 = GW - ML2 - GMR, PH2 = GH - GMT - MB2;

    /* per-cell stats + y extent */
    BoxStat *bs = calloc((size_t)ncell, sizeof *bs);
    double ymin = 0, ymax = 0; int first = 1;
    Variable *Y = &f->vars[yv];
    double *buf = malloc(f->nobs * sizeof *buf);
    for (int b = 0; b < nband; b++){
        for (int i = 0; i < nin; i++){
            size_t n = 0;
            for (size_t r = 0; r < f->nobs; r++){
                if (mask && !mask[r]) continue;
                if (sv_is_miss(Y->num[r])) continue;
                if (level_index(inL, nin, &f->vars[in_vi], r) != i) continue;
                if (nout_ && level_index(outL, nout_, &f->vars[out_vi], r) != b) continue;
                buf[n++] = Y->num[r];
            }
            BoxStat *B = &bs[b*nin + i];
            box_stats(buf, n, B);
            if (n){
                double lo = hide_outliers ? B->wlo : (B->nout ? fmin(B->wlo, B->out[0]) : B->wlo);
                double hi = hide_outliers ? B->whi : (B->nout ? fmax(B->whi, B->out[B->nout-1]) : B->whi);
                if (first){ ymin=lo; ymax=hi; first=0; }
                if (lo<ymin)ymin=lo; if(hi>ymax)ymax=hi;
            }
        }
    }
    if (first){ ymin=0; ymax=1; }
    GAxis ay = g_axis_fit(ymin, ymax);
    #define BPY(v) snap2(GMT + PH2 - ((v)-ay.lo)/(ay.hi-ay.lo)*PH2)

    fprintf(o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
               "viewBox=\"0 0 %d %d\" font-family=\"Helvetica,Arial,sans-serif\">\n",
               GW, GH, GW, GH);
    fprintf(o, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", GW, GH);
    fprintf(o, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"black\" stroke-width=\"1\"/>\n",
            ML2, GMT, PW2, PH2);

    char lab[64], esc[256];
    for (int i=0;i<ay.nt;i++){
        double v = ay.lo + i*ay.step, Yp = BPY(v);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"#dddddd\" stroke-width=\"1\"/>\n", ML2, Yp, ML2+PW2, Yp);
        fprintf(o, "<line x1=\"%d\" y1=\"%.2f\" x2=\"%d\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", ML2-5, Yp, ML2, Yp);
        g_tick_label(lab, sizeof lab, v);
        fprintf(o, "<text x=\"%d\" y=\"%.2f\" text-anchor=\"end\" dominant-baseline=\"middle\" font-size=\"12\">%s</text>\n", ML2-9, Yp, lab);
    }

    /* horizontal layout: bands of nin slots; box width 60%% of slot */
    double band_w = (double)PW2 / nband;
    double slot_w = band_w / nin;
    double box_w  = slot_w * 0.6;
    for (int b = 0; b < nband; b++){
        for (int i = 0; i < nin; i++){
            BoxStat *B = &bs[b*nin + i];
            double cx = snap2(ML2 + b*band_w + (i + 0.5)*slot_w);
            /* inner tick label */
            double ly = GMT + PH2 + 16;
            g_xml_escape(inL[i].label, esc, sizeof esc);
            if (inSt->angle)
                fprintf(o, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"end\" font-size=\"%d\" transform=\"rotate(-%d %.2f %.2f)\">%s</text>\n",
                        cx, ly, inSt->fontsz, inSt->angle, cx, ly, esc);
            else
                fprintf(o, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"middle\" font-size=\"%d\">%s</text>\n",
                        cx, ly, inSt->fontsz, esc);
            if (!B->n) continue;
            double yq1=BPY(B->q1), yq3=BPY(B->q3), ymed=BPY(B->med), ywlo=BPY(B->wlo), ywhi=BPY(B->whi);
            double x0 = snap2(cx - box_w/2), x1 = snap2(cx + box_w/2);
            /* whisker stems + caps */
            fprintf(o, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", cx, ywhi, cx, yq3);
            fprintf(o, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", cx, yq1, cx, ywlo);
            fprintf(o, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", snap2(cx-box_w/4), ywhi, snap2(cx+box_w/4), ywhi);
            fprintf(o, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1\"/>\n", snap2(cx-box_w/4), ywlo, snap2(cx+box_w/4), ywlo);
            /* box + median */
            fprintf(o, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#1f4e79\" fill-opacity=\"0.55\" stroke=\"black\" stroke-width=\"1\"/>\n",
                    x0, yq3, snap2(x1-x0), snap2(yq1-yq3));
            fprintf(o, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"black\" stroke-width=\"1.5\"/>\n", x0, ymed, x1, ymed);
            /* outside values */
            if (!hide_outliers)
                for (size_t k=0;k<B->nout;k++)
                    fprintf(o, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"2\" fill=\"none\" stroke=\"black\" stroke-width=\"0.8\"/>\n",
                            cx, BPY(B->out[k]));
        }
        /* outer band label + separator */
        if (nout_){
            double bx = snap2(ML2 + (b + 0.5)*band_w);
            g_xml_escape(outL[b].label, esc, sizeof esc);
            fprintf(o, "<text x=\"%.2f\" y=\"%d\" text-anchor=\"middle\" font-size=\"%d\">%s</text>\n",
                    bx, GMT + PH2 + in_h + 18, outSt->fontsz, esc);
            if (b) {
                double sx = snap2(ML2 + b*band_w);
                fprintf(o, "<line x1=\"%.2f\" y1=\"%d\" x2=\"%.2f\" y2=\"%d\" stroke=\"#bbbbbb\" stroke-width=\"0.5\"/>\n",
                        sx, GMT, sx, GMT+PH2);
            }
        }
    }

    if (title && title[0]){
        g_xml_escape(title, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" font-size=\"16\" font-weight=\"bold\">%s</text>\n", GW/2, 24, esc);
    }
    if (note && note[0]){
        g_xml_escape(note, esc, sizeof esc);
        fprintf(o, "<text x=\"%d\" y=\"%d\" font-size=\"10\" fill=\"#555555\">%s</text>\n", ML2, GH-6, esc);
    }
    fprintf(o, "</svg>\n");
    for (int i=0;i<ncell;i++) free(bs[i].out);
    free(bs); free(buf);
    #undef BPY
}

/* ======================================================================== */
/* named-graph registry + combine                                           */
/* ======================================================================== */
typedef struct GEntry { char name[64]; char *svg; struct GEntry *next; } GEntry;
static GEntry *g_registry = NULL;

void graph_registry_clear(void){
    for (GEntry *e = g_registry; e; ){
        GEntry *nx = e->next; free(e->svg); free(e); e = nx;
    }
    g_registry = NULL;
}
static GEntry *registry_find(const char *nm){
    for (GEntry *e = g_registry; e; e = e->next)
        if (!strcmp(e->name, nm)) return e;
    return NULL;
}
static int registry_put(const char *nm, const char *svg, int replace){
    GEntry *e = registry_find(nm);
    if (e && !replace){
        tea_err("graph: %s already exists (use name(%s, replace))\n", nm, nm);
        return 110;
    }
    if (!e){
        static int at_registered = 0;
        if (!at_registered){ atexit(graph_registry_clear); at_registered = 1; }
        e = calloc(1, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", nm);
        e->next = g_registry; g_registry = e;
    } else free(e->svg);
    e->svg = strdup(svg);
    return 0;
}

/* parse name(NAME[, replace]) out of an option string */
static int parse_name_opt(const char *opts, char *nm, size_t nn, int *replace){
    char arg[128];
    *replace = 0; nm[0] = 0;
    if (!opt_find_nth(opts, "name", 0, arg, sizeof arg)) return 0;
    char *cm = strchr(arg, ',');
    if (cm){ *cm = 0; if (strstr(cm+1, "replace")) *replace = 1; }
    char *p = arg; while (*p==' ') p++;
    char *e = p + strlen(p); while (e>p && e[-1]==' ') *--e = 0;
    snprintf(nm, nn, "%s", p);
    return nm[0] != 0;
}

/* Common tail for twoway / graph box: render into memory, then route to
 * the registry (name()), a file (saving() or NAME.svg or tea_graph.svg),
 * and the web Plots hook.  Returns 0 or an rc. */
static int graph_emit(Cmd *c, void (*render)(void*, FILE*), void *ctx, size_t npts){
    char *body = NULL; size_t blen = 0;
    FILE *m = open_memstream(&body, &blen);
    if (!m){ tea_err("graph: out of memory\n"); return 909; }
    render(ctx, m);
    fclose(m);

    char nm[64]; int replace = 0;
    int named = parse_name_opt(c->options, nm, sizeof nm, &replace);
    if (named){
        int rc = registry_put(nm, body, replace);
        if (rc){ free(body); return rc; }
    }
    char fname[512] = "";
    int explicit_save = opt_find_nth(c->options, "saving", 0, fname, sizeof fname);
    if (explicit_save) unquote(fname);
    if (!explicit_save){
        if (named) snprintf(fname, sizeof fname, "%s.svg", nm);
        else       snprintf(fname, sizeof fname, "tea_graph.svg");
    }
    FILE *o = fopen(fname, "w");
    if (!o){ tea_err("graph: cannot write %s\n", fname); free(body); return 603; }
    fwrite(body, 1, blen, o);
    fclose(o);
    free(body);
    if (!c->quiet) printf("(graph written to %s, n=%zu)\n", fname, npts);
#ifdef __EMSCRIPTEN__
    tea_wasm_graph_notify(fname);
#endif
    return 0;
}

/* graph combine: nest each stored SVG (which carries its own viewBox) in
 * a cols() grid.  A nested <svg> with x/y/width/height scales cleanly. */
static int do_graph_combine(Cmd *c){
    char names[16][64]; int nn = 0;
    const char *p = c->varlist;
    while (*p && nn < 16){
        while (*p==' ') p++;
        if (!*p) break;
        int w = 0;
        while (*p && *p!=' ' && w+1<64) names[nn][w++] = *p++;
        names[nn][w] = 0; nn++;
    }
    if (nn < 1){ tea_err("graph combine: at least one graph name required\n"); return 198; }
    GEntry *ent[16];
    for (int i=0;i<nn;i++){
        ent[i] = registry_find(names[i]);
        if (!ent[i]){ tea_err("graph combine: graph %s not found (was it made with name(%s)?)\n",
                              names[i], names[i]); return 111; }
    }
    char carg[32];
    int cols = opt_find_nth(c->options, "cols", 0, carg, sizeof carg) ? atoi(carg) : 0;
    if (opt_find_nth(c->options, "rows", 0, carg, sizeof carg)){
        int rows = atoi(carg);
        if (rows > 0) cols = (nn + rows - 1) / rows;
    }
    if (cols <= 0) cols = (nn > 1) ? 2 : 1;
    if (cols > nn) cols = nn;
    int rows = (nn + cols - 1) / cols;

    char title[128]="", note[128]="";
    if (opt_find_nth(c->options,"title",0,title,sizeof title)) unquote(title);
    if (opt_find_nth(c->options,"note", 0,note, sizeof note))  unquote(note);
    int mt = title[0] ? 34 : 0, mb = note[0] ? 18 : 0;
    int CW = cols*GW, CH = rows*GH + mt + mb;

    char *body = NULL; size_t blen = 0;
    FILE *m = open_memstream(&body, &blen);
    if (!m){ tea_err("graph: out of memory\n"); return 909; }
    fprintf(m, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
               "viewBox=\"0 0 %d %d\" font-family=\"Helvetica,Arial,sans-serif\">\n",
               CW, CH, CW, CH);
    fprintf(m, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", CW, CH);
    char esc[256];
    if (title[0]){
        g_xml_escape(title, esc, sizeof esc);
        fprintf(m, "<text x=\"%d\" y=\"22\" text-anchor=\"middle\" font-size=\"17\" font-weight=\"bold\">%s</text>\n", CW/2, esc);
    }
    for (int i=0;i<nn;i++){
        int r2 = i / cols, c2 = i % cols;
        /* the stored document is a complete <svg>; as a child element with
         * x/y it renders scaled into its cell via its own viewBox. */
        const char *doc = ent[i]->svg;
        const char *tagend = strchr(doc, '>');
        if (!tagend){ fclose(m); free(body); tea_err("graph combine: malformed stored SVG\n"); return 499; }
        fprintf(m, "<svg x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
                c2*GW, mt + r2*GH, GW, GH, GW, GH);
        fputs(tagend+1, m);            /* body + its own </svg> closes ours? no: */
        /* the stored doc ends with </svg>\n which closes THIS nested element —
         * exactly one open tag was replaced by ours, so nesting balances. */
    }
    if (note[0]){
        g_xml_escape(note, esc, sizeof esc);
        fprintf(m, "<text x=\"12\" y=\"%d\" font-size=\"10\" fill=\"#555555\">%s</text>\n", CH-6, esc);
    }
    fprintf(m, "</svg>\n");
    fclose(m);

    char nm[64]; int replace=0;
    int named = parse_name_opt(c->options, nm, sizeof nm, &replace);
    if (named){
        int rc = registry_put(nm, body, replace);
        if (rc){ free(body); return rc; }
    }
    char fname[512]="";
    int explicit_save = opt_find_nth(c->options,"saving",0,fname,sizeof fname);
    if (explicit_save) unquote(fname);
    if (!explicit_save){
        if (named) snprintf(fname,sizeof fname,"%s.svg",nm);
        else       snprintf(fname,sizeof fname,"tea_graph.svg");
    }
    FILE *o = fopen(fname,"w");
    if (!o){ tea_err("graph: cannot write %s\n", fname); free(body); return 603; }
    fwrite(body,1,blen,o); fclose(o); free(body);
    if (!c->quiet) printf("(graph written to %s, %d panels)\n", fname, nn);
#ifdef __EMSCRIPTEN__
    tea_wasm_graph_notify(fname);
#endif
    return 0;
}

/* ======================================================================== */
/* command parsers                                                          */
/* ======================================================================== */

/* trampoline ctx for graph_emit */
typedef struct { const GSpec *g; } TwCtx;
static void tw_render(void *v, FILE *o){ render_twoway(((TwCtx*)v)->g, o); }

/* parse ONE series spec: `kind y x [if EXPR] [, opts]` (text inside parens) */
static int parse_series(Cmd *c, const char *spec, const char *global_if, GSeries *S, GSpec *g){
    memset(S, 0, sizeof *S);
    S->mlabpos = 3; S->bwidth = 0.8;
    char kind[24]; int k = 0;
    const char *p = spec; while (*p==' ') p++;
    while (*p && !isspace((unsigned char)*p) && k+1<(int)sizeof kind) kind[k++]=*p++;
    kind[k]=0;
    if      (!strcmp(kind,"scatter"))   S->kind = GS_SCATTER;
    else if (!strcmp(kind,"line"))      S->kind = GS_LINE;
    else if (!strcmp(kind,"connected")) S->kind = GS_LINE;
    else if (!strcmp(kind,"lowess"))    S->kind = GS_LOWESS;
    else { tea_err("twoway: unknown plot type `%s' (tea has: scatter line connected lowess)\n", kind); return 198; }
    while (*p==' ') p++;
    char yv[64], xv[64]; int w=0;
    while (*p && !isspace((unsigned char)*p) && *p!=',' && w+1<64) yv[w++]=*p++;
    yv[w]=0; while (*p==' ') p++;
    w=0;
    while (*p && !isspace((unsigned char)*p) && *p!=',' && w+1<64) xv[w++]=*p++;
    xv[w]=0;
    if (!yv[0] || !xv[0]){ tea_err("twoway: series needs `%s yvar xvar'\n", kind); return 198; }
    while (*p==' ') p++;
    /* optional per-series if, up to the option comma (depth-aware) */
    char ifx[512]=""; char opts[1024]="";
    if (!strncmp(p, "if ", 3)){
        p += 3;
        int d=0, iw=0, inq=0;
        while (*p && iw+1<(int)sizeof ifx){
            if (*p=='"') inq=!inq;
            else if (!inq && *p=='(') d++;
            else if (!inq && *p==')') d--;
            else if (!inq && *p==',' && d==0) break;
            ifx[iw++]=*p++;
        }
        ifx[iw]=0;
        while (iw>0 && ifx[iw-1]==' ') ifx[--iw]=0;
    }
    if (*p==','){ p++; while(*p==' ')p++; snprintf(opts,sizeof opts,"%s",p); }

    /* combine a global (top-level) if with the per-series if */
    char combined[1100];
    if (global_if && global_if[0] && ifx[0])
        snprintf(combined,sizeof combined,"(%s) & (%s)", global_if, ifx);
    else if (global_if && global_if[0])
        snprintf(combined,sizeof combined,"%s", global_if);
    else
        snprintf(combined,sizeof combined,"%s", ifx);

    /* per-series options */
    char mlabvar[64] = "";
    const char *cur = opts; char nm[64], arg[512];
    while (opt_next(&cur, nm, sizeof nm, arg, sizeof arg)){
        if      (!strcmp(nm,"lcolor")||!strcmp(nm,"mcolor")||!strcmp(nm,"color"))
            snprintf(S->color,sizeof S->color,"%s",color_hex(arg));
        else if (!strcmp(nm,"lpattern")) snprintf(S->lpattern,sizeof S->lpattern,"%s",arg);
        else if (!strcmp(nm,"msymbol")){ if(!strcmp(arg,"i")||!strcmp(arg,"none")) S->msym_none=1; }
        else if (!strcmp(nm,"mlabel"))   snprintf(mlabvar,sizeof mlabvar,"%s",arg);
        else if (!strcmp(nm,"mlabcolor"))snprintf(S->mlabcolor,sizeof S->mlabcolor,"%s",color_hex(arg));
        else if (!strcmp(nm,"mlabposition")) S->mlabpos = atoi(arg);
        else if (!strcmp(nm,"bwidth"))   S->bwidth = atof(arg);
        else if (!strcmp(nm,"mean"))     S->lw_mean = 1;
        else if (!strcmp(nm,"adjust"))   S->lw_adjust = 1;
        /* axis-level options are legal inside a series in Stata and apply
         * to the whole graph — merge upward (first one wins) */
        else if (!strcmp(nm,"xtitle")){ if(!g->xtitle[0]){ unquote(arg); snprintf(g->xtitle,sizeof g->xtitle,"%s",arg);} }
        else if (!strcmp(nm,"ytitle")){ if(!g->ytitle[0]){ unquote(arg); snprintf(g->ytitle,sizeof g->ytitle,"%s",arg);} }
        else if (!strcmp(nm,"title")) { if(!g->title[0]) { unquote(arg); snprintf(g->title, sizeof g->title, "%s",arg);} }
        /* everything else: cosmetic, accepted-and-ignored (see graph.h) */
    }
    long n = collect_series(c->f, yv, xv, combined, mlabvar[0]?mlabvar:NULL,
                            &S->x, &S->y, &S->mlab);
    if (n < 0) return 198;
    S->n = (size_t)n;
    if (S->kind != GS_SCATTER) series_sort_x(S->x, S->y, S->mlab, S->n);
    if (S->kind == GS_LOWESS)
        lowess_smooth(S->x, S->y, S->n, S->bwidth, S->lw_mean, S->lw_adjust);
    snprintf(S->legend_label, sizeof S->legend_label, "%s%s", yv,
             S->kind==GS_LOWESS ? " (lowess)" : "");
    return 0;
}

static int parse_globals(Cmd *c, GSpec *g){
    char arg[512];
    if (opt_find_nth(c->options,"title", 0,arg,sizeof arg)){ unquote(arg); snprintf(g->title, sizeof g->title, "%s",arg); }
    if (opt_find_nth(c->options,"xtitle",0,arg,sizeof arg)){ unquote(arg); snprintf(g->xtitle,sizeof g->xtitle,"%s",arg); }
    if (opt_find_nth(c->options,"ytitle",0,arg,sizeof arg)){ unquote(arg); snprintf(g->ytitle,sizeof g->ytitle,"%s",arg); }
    if (opt_find_nth(c->options,"note",  0,arg,sizeof arg)){ unquote(arg); snprintf(g->note,  sizeof g->note,  "%s",arg); }
    if (opt_find_nth(c->options,"legend",0,arg,sizeof arg) && strstr(arg,"off")) g->legend_off = 1;
    for (int i=0; i<8 && opt_find_nth(c->options,"yline",i,arg,sizeof arg); i++){
        char *cm = strchr(arg, ',');
        char sub[256]=""; if (cm){ *cm=0; snprintf(sub,sizeof sub,"%s",cm+1); }
        g->yline_v[g->nyline] = atof(arg);
        g->yline_pat[g->nyline][0] = 0; g->yline_col[g->nyline][0] = 0;
        const char *c2=sub; char n2[64], a2[128];
        while (opt_next(&c2,n2,sizeof n2,a2,sizeof a2)){
            if (!strcmp(n2,"lpattern")) snprintf(g->yline_pat[g->nyline],16,"%s",a2);
            else if (!strcmp(n2,"lcolor")) snprintf(g->yline_col[g->nyline],48,"%s",color_hex(a2));
        }
        g->nyline++;
    }
    if (opt_find_nth(c->options,"yscale",0,arg,sizeof arg)){
        char *r = strstr(arg,"range(");
        if (r){ double lo,hi; if (sscanf(r+6,"%lf %lf",&lo,&hi)==2){ g->has_yscale=1; g->ysc_lo=lo; g->ysc_hi=hi; } }
    }
    if (opt_find_nth(c->options,"ylabel",0,arg,sizeof arg)){
        double lo,st,hi;
        if (sscanf(arg,"%lf(%lf)%lf",&lo,&st,&hi)==3 && st>0 && hi>lo){
            g->has_ylab=1; g->ylab_lo=lo; g->ylab_step=st; g->ylab_hi=hi; }
        /* other ylabel forms: cosmetic, accepted-and-ignored */
    }
    if (opt_find_nth(c->options,"xlabel",0,arg,sizeof arg)){
        double lo,st,hi;
        if (sscanf(arg,"%lf(%lf)%lf",&lo,&st,&hi)==3 && st>0 && hi>lo){
            g->has_xlab=1; g->xlab_lo=lo; g->xlab_step=st; g->xlab_hi=hi; }
    }
    return 0;
}

int do_twoway(Cmd *c){
    GSpec g; memset(&g, 0, sizeof g);
    const char *p = c->varlist;
    while (*p==' ') p++;
    if (*p != '('){
        /* parenless single-series form: twoway scatter y x [if], opts
         * (the top-level if already landed in c->ifexp) */
        GSeries *S = &g.s[0];
        char spec[2048]; snprintf(spec, sizeof spec, "%s%s%s", c->varlist,
                                  c->options[0] ? ", " : "", c->options);
        int rc = parse_series(c, spec, c->ifexp, S, &g);
        if (rc){ gspec_free(&g); return rc; }
        g.ns = 1;
    } else {
        while (*p){
            while (*p==' ') p++;
            if (!*p) break;
            if (*p != '('){ tea_err("twoway: expected `(' starting a series, got `%s'\n", p);
                            gspec_free(&g); return 198; }
            p++;
            char spec[2048]; int w=0, d=1, inq=0;
            while (*p && d && w+1<(int)sizeof spec){
                if (*p=='"') inq=!inq;
                else if (!inq && *p=='(') d++;
                else if (!inq && *p==')'){ d--; if(!d){ p++; break; } }
                spec[w++]=*p++;
            }
            spec[w]=0;
            if (d){ tea_err("twoway: unbalanced parentheses in series\n"); gspec_free(&g); return 198; }
            if (g.ns >= 32){ tea_err("twoway: at most 32 series\n"); gspec_free(&g); return 198; }
            int rc = parse_series(c, spec, c->ifexp, &g.s[g.ns], &g);
            if (rc){ gspec_free(&g); return rc; }
            g.ns++;
        }
        if (!g.ns){ tea_err("twoway: no series given\n"); return 198; }
    }
    parse_globals(c, &g);
    size_t npts = 0; for (int s=0;s<g.ns;s++) npts += g.s[s].n;
    TwCtx ctx = { &g };
    int rc = graph_emit(c, tw_render, &ctx, npts);
    gspec_free(&g);
    return rc;
}

/* trampoline ctx for graph box */
typedef struct {
    Workspace *ws; Frame *f; int yv;
    unsigned char *mask;
    int in_vi;  GLevel *inL;  int nin;  OverStyle inSt;
    int out_vi; GLevel *outL; int nout; OverStyle outSt;
    int noout;
    char title[128], note[128];
} BoxCtx;
static void box_render(void *v, FILE *o){
    BoxCtx *b = v;
    render_graph_box(b->ws, b->f, b->yv, b->mask,
                     b->in_vi, b->inL, b->nin, &b->inSt,
                     b->out_vi, b->outL, b->nout, &b->outSt,
                     b->noout, b->title, b->note, o);
}

static int do_graph_box(Cmd *c){
    /* c->varlist = "box YVAR"; c->ifexp/in set; options carry the rest */
    char sub[16], yv[64]="";
    sscanf(c->varlist, "%15s %63s", sub, yv);
    if (!yv[0]){ tea_err("graph box: syntax is `graph box var [if], over(v) [over(v2)] ...'\n"); return 198; }
    int yi = var_find(c->f, yv);
    if (yi < 0){ tea_err("graph box: variable %s not found\n", yv); return 111; }
    if (c->f->vars[yi].type != VT_NUM){ tea_err("graph box: %s must be numeric\n", yv); return 109; }

    /* observation mask from if/in */
    unsigned char *mask = NULL;
    if (c->ifexp[0]){
        const char *perr;
        Node *ifn = expr_parse(c->ifexp, c->f, &perr);
        if (!ifn){ tea_err("graph box: bad if expression: %s\n", perr?perr:c->ifexp); return 198; }
        mask = malloc(c->f->nobs);
        for (size_t r=0;r<c->f->nobs;r++){
            EvalCtx ec={0}; ec.f=c->f; ec.i=(long)r; ec.n=(long)r+1; ec.N=(long)c->f->nobs;
            EVal v = expr_eval(ifn,&ec);
            mask[r] = (unsigned char)(!v.is_str && !sv_is_miss(v.num) && v.num!=0);
            eval_free(&v);
        }
        node_free(ifn);
    }

    BoxCtx b; memset(&b,0,sizeof b);
    b.ws=c->ws; b.f=c->f; b.yv=yi; b.mask=mask;
    char arg[1024];
    { char a2[8]; const char *cur=c->options; char nm[64];
      while (opt_next(&cur,nm,sizeof nm,a2,sizeof a2)) if(!strcmp(nm,"noout")) b.noout=1; }
    if (opt_find_nth(c->options,"title",0,arg,sizeof arg)){ unquote(arg); snprintf(b.title,sizeof b.title,"%s",arg); }
    if (opt_find_nth(c->options,"note", 0,arg,sizeof arg)){ unquote(arg); snprintf(b.note, sizeof b.note, "%s",arg); }

    /* the two over() clauses: first = inner (varies fastest) */
    char ov[2][1024]; int nover = 0;
    for (int i=0;i<2;i++) if (opt_find_nth(c->options,"over",i,ov[i],sizeof ov[i])) nover++;
    if (!nover){ tea_err("graph box: at least one over() is required\n"); free(mask); return 198; }

    int rc = 0;
    char sub2[2][1024]; char vnm[2][64];
    int vi[2] = {-1,-1};
    for (int i=0;i<nover;i++){
        char *cm = strchr(ov[i], ',');
        sub2[i][0]=0;
        if (cm){ *cm=0; snprintf(sub2[i],sizeof sub2[i],"%s",cm+1); }
        char *q=ov[i]; while(*q==' ')q++;
        char *e=q+strlen(q); while(e>q&&e[-1]==' ')*--e=0;
        snprintf(vnm[i],64,"%s",q);
        vi[i]=var_find(c->f,vnm[i]);
        if (vi[i]<0){ tea_err("graph box: over() variable %s not found\n",vnm[i]); rc=111; goto out; }
    }
    b.in_vi = vi[0];
    b.nin = levels_of(c->ws, c->f, vi[0], mask, &b.inL);
    if (b.nin <= 0){ tea_err("graph box: over() variable %s has no levels in sample\n", vnm[0]); rc=2000; goto out; }
    parse_over_sub(sub2[0], b.inL, b.nin, &b.inSt);
    if (nover==2){
        b.out_vi = vi[1];
        b.nout = levels_of(c->ws, c->f, vi[1], mask, &b.outL);
        if (b.nout <= 0){ tea_err("graph box: over() variable %s has no levels in sample\n", vnm[1]); rc=2000; goto out; }
        parse_over_sub(sub2[1], b.outL, b.nout, &b.outSt);
    } else { b.outSt.fontsz = 12; }

    { size_t npts=0; Variable *Y=&c->f->vars[yi];
      for (size_t r=0;r<c->f->nobs;r++){ if(mask&&!mask[r])continue; if(!sv_is_miss(Y->num[r]))npts++; }
      rc = graph_emit(c, box_render, &b, npts); }
out:
    if (b.inL)  levels_free(b.inL,  b.nin);
    if (b.outL) levels_free(b.outL, b.nout);
    free(mask);
    return rc;
}

int do_graph(Cmd *c){
    char sub[24]="";
    sscanf(c->varlist, "%23s", sub);
    if (!strcmp(sub,"box"))     return do_graph_box(c);
    if (!strcmp(sub,"combine")){
        /* shift the subword off the varlist for the name list */
        char *p = c->varlist + 7; while(*p==' ')p++;
        memmove(c->varlist, p, strlen(p)+1);
        return do_graph_combine(c);
    }
    if (!strcmp(sub,"dir")){
        int n=0;
        for (GEntry *e=g_registry; e; e=e->next){ printf("%s%s", n++?"  ":"", e->name); }
        printf(n ? "\n" : "(no graphs in memory)\n");
        return 0;
    }
    if (!strcmp(sub,"drop")){
        char nm[64]=""; sscanf(c->varlist+5,"%63s",nm);
        if (!strcmp(nm,"_all")){ graph_registry_clear(); return 0; }
        GEntry **pp=&g_registry;
        while (*pp && strcmp((*pp)->name,nm)) pp=&(*pp)->next;
        if (!*pp){ tea_err("graph drop: %s not found\n", nm); return 111; }
        GEntry *e=*pp; *pp=e->next; free(e->svg); free(e);
        return 0;
    }
    tea_err("graph: unknown subcommand `%s' (tea has: box combine dir drop)\n", sub);
    return 199;
}

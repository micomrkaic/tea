/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * estout.c — formatted regression-result tables.
 *
 * Syntax:
 *   estout [names] [, options]
 *
 * Produces a side-by-side table of stored estimates.  If no names are
 * given, uses all currently stored estimates (in insertion order).
 * If the user types `estout` immediately after an estimation without
 * having stored anything, uses last_est as the single column.
 *
 * Defaults: LaTeX format (per IMF/academic-economics convention).
 *
 * Options:
 *   format(latex|markdown|md|plain)   output format (default latex)
 *   se                                show SE in parens below coef
 *   t                                 show t-stat (z-stat for MLE) in parens
 *   p                                 show p-value in brackets
 *   star | stars                      add significance stars
 *   nostar                            suppress stars (overrides defaults)
 *   stats(N r2 r2_a rmse ll F ...)    per-model summary rows
 *   nogaps                            don't insert blank lines between coefs
 *   title("...")                      table caption (LaTeX/markdown only)
 *   label(tab:foo)                    LaTeX \label{} value
 *   keep(varlist)                     restrict to these coefficients
 *   drop(varlist)                     exclude these coefficients
 *   nomtitles                         suppress per-column model labels
 *   mtitles("M1" "M2" ...)            override per-column labels
 *   using FILENAME                    write to file instead of stdout
 *
 * The defaults aim to produce a paper-ready LaTeX fragment that a user
 * can `\input{}` directly.  Stars default to ON for LaTeX/markdown
 * since that's the academic convention.
 */
#define _GNU_SOURCE
#include "interp.h"
#include "cmd.h"
#include "estimates.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

/* ---- format-specific helpers ----------------------------------------- */

typedef enum { FMT_LATEX = 0, FMT_MARKDOWN, FMT_PLAIN } OutFmt;

/* Escape a variable name for LaTeX.  Names like "L.growth" or
 * "2.country#c.gdp" have special LaTeX characters that need escaping:
 * '_' and '#'.  Output to `out`, max `out_sz`.  Returns out. */
static char *latex_escape(const char *src, char *out, size_t out_sz)
{
    size_t pos = 0;
    for(const char *p = src; *p && pos < out_sz - 2; p++){
        if(*p == '_' || *p == '#' || *p == '&' || *p == '%' || *p == '$'){
            out[pos++] = '\\';
            out[pos++] = *p;
        } else if(*p == '.'){
            /* Keep dots as-is; they're legal in LaTeX text */
            out[pos++] = *p;
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = 0;
    return out;
}

/* Markdown escape: just '|' and '*' are problematic in cells. */
static char *md_escape(const char *src, char *out, size_t out_sz)
{
    size_t pos = 0;
    for(const char *p = src; *p && pos < out_sz - 2; p++){
        if(*p == '|' || *p == '*' || *p == '`'){
            out[pos++] = '\\';
            out[pos++] = *p;
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = 0;
    return out;
}

/* Significance stars based on z/t.  Uses z-distribution for simplicity
 * (close enough at usual N).  Stata default thresholds: 10%, 5%, 1%. */
static const char *sig_stars(double absz)
{
    if(absz > 2.5758) return "***";
    if(absz > 1.9600) return "**";
    if(absz > 1.6449) return "*";
    return "";
}

/* Format a coefficient cell.  Buffer must be large (64 chars). */
static void fmt_coef(double b, const char *stars, char *out, size_t out_sz, OutFmt fmt)
{
    if(fmt == FMT_LATEX && stars[0]){
        snprintf(out, out_sz, "%.4g$^{%s}$", b, stars);
    } else if(stars[0]){
        snprintf(out, out_sz, "%.4g%s", b, stars);
    } else {
        snprintf(out, out_sz, "%.4g", b);
    }
}

/* ---- main impl -------------------------------------------------------- */

static StoredEst *find_stored(Workspace *ws, const char *name)
{
    for(StoredEst *s = ws->stored_est; s; s = s->next)
        if(!strcmp(s->name, name)) return s;
    return NULL;
}

/* Match name against a space-separated keep/drop list. */
static bool name_in_list(const char *name, const char *list)
{
    if(!list || !list[0]) return false;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    char *sp = NULL;
    for(char *t = strtok_r(buf, " ,", &sp); t; t = strtok_r(NULL, " ,", &sp)){
        if(!strcmp(t, name)) return true;
    }
    return false;
}

int do_estout(Cmd *c)
{
    /* Parse options */
    char fmt_str[16] = "latex";
    opt_value(c->options, "format", fmt_str, sizeof fmt_str);
    OutFmt fmt = FMT_LATEX;
    if(!strcmp(fmt_str, "markdown") || !strcmp(fmt_str, "md")) fmt = FMT_MARKDOWN;
    else if(!strcmp(fmt_str, "plain") || !strcmp(fmt_str, "text")) fmt = FMT_PLAIN;
    else if(!strcmp(fmt_str, "latex") || !strcmp(fmt_str, "tex"))  fmt = FMT_LATEX;
    else {
        fprintf(stderr,"estout: unknown format '%s' (use latex|markdown|plain)\n", fmt_str);
        return 198;
    }

    bool show_se = opt_present(c->options, "se");
    bool show_t  = opt_present(c->options, "t");
    bool show_p  = opt_present(c->options, "p");
    if(!show_se && !show_t && !show_p) show_se = true;  /* default: SE in parens */

    bool want_stars  = !opt_present(c->options, "nostar")
                    && (opt_present(c->options, "star")
                        || opt_present(c->options, "stars")
                        || fmt != FMT_PLAIN);
    char stats_buf[256] = "";
    opt_value(c->options, "stats", stats_buf, sizeof stats_buf);

    char title[128] = "";
    opt_value(c->options, "title", title, sizeof title);
    char label_str[64] = "";
    opt_value(c->options, "label", label_str, sizeof label_str);
    char keep_str[256] = "";
    bool has_keep = opt_value(c->options, "keep", keep_str, sizeof keep_str);
    char drop_str[256] = "";
    bool has_drop = opt_value(c->options, "drop", drop_str, sizeof drop_str);
    bool nomtitles = opt_present(c->options, "nomtitles");
    char mtitles_buf[512] = "";
    bool has_mtitles = opt_value(c->options, "mtitles", mtitles_buf, sizeof mtitles_buf);

    char outfile[256] = "";
    /* Stata-like `using FILENAME` syntax: check varlist for 'using ' */
    char vlcopy[1024]; snprintf(vlcopy, sizeof vlcopy, "%s", c->varlist);
    char *using_ptr = strstr(vlcopy, " using ");
    if(using_ptr){
        *using_ptr = 0;
        const char *fp = using_ptr + 7;
        while(*fp == ' ') fp++;
        snprintf(outfile, sizeof outfile, "%s", fp);
        /* Strip trailing whitespace */
        char *e = outfile + strlen(outfile);
        while(e > outfile && (e[-1]==' '||e[-1]=='\n'||e[-1]=='\t')) *--e = 0;
    } else if(!strncmp(vlcopy, "using ", 6)){
        const char *fp = vlcopy + 6;
        while(*fp == ' ') fp++;
        snprintf(outfile, sizeof outfile, "%s", fp);
        char *e = outfile + strlen(outfile);
        while(e > outfile && (e[-1]==' '||e[-1]=='\n'||e[-1]=='\t')) *--e = 0;
        vlcopy[0] = 0;  /* no model names listed */
    }

    /* Collect models.  If names given, look them up.  If not, use all
     * stored, or fall back to last_est if no stored. */
    StoredEst *picked[32]; int npicked = 0;
    StoredEst tmp_last;   /* used as a synthetic wrapper around last_est */
    char *p = vlcopy; while(*p == ' ') p++;
    if(*p){
        char tok[33];
        while(*p){
            while(*p == ' ') p++;
            if(!*p) break;
            int j = 0;
            while(*p && *p != ' ' && j < 32) tok[j++] = *p++;
            tok[j] = 0;
            StoredEst *s = find_stored(c->ws, tok);
            if(!s){
                fprintf(stderr,"estout: %s not in stored estimates\n", tok);
                return 198;
            }
            if(npicked < 32) picked[npicked++] = s;
        }
    } else if(c->ws->stored_est){
        for(StoredEst *s = c->ws->stored_est; s && npicked < 32; s = s->next)
            picked[npicked++] = s;
    } else if(c->ws->last_est){
        memset(&tmp_last, 0, sizeof tmp_last);
        snprintf(tmp_last.name, sizeof tmp_last.name, "(1)");
        tmp_last.est = c->ws->last_est;
        picked[0] = &tmp_last;
        npicked = 1;
    } else {
        fprintf(stderr,"estout: no estimates available\n");
        return 301;
    }

    /* Apply mtitles override if given.  We don't modify the StoredEst
     * names directly; instead use a parallel array of display names. */
    char display_names[32][33];
    for(int m = 0; m < npicked; m++)
        snprintf(display_names[m], 33, "%s", picked[m]->name);
    if(has_mtitles){
        /* Parse mtitles as space-separated quoted-or-bare names */
        const char *q = mtitles_buf;
        int mi = 0;
        while(*q && mi < npicked){
            while(*q == ' ') q++;
            if(!*q) break;
            int j = 0;
            if(*q == '"'){
                q++;
                while(*q && *q != '"' && j < 32) display_names[mi][j++] = *q++;
                if(*q == '"') q++;
            } else {
                while(*q && *q != ' ' && j < 32) display_names[mi][j++] = *q++;
            }
            display_names[mi][j] = 0;
            mi++;
        }
    }

    /* Union of variable names across models, in first-seen order.  Drop
     * _cons if present (we'll put it at the end). */
    char (*names)[33] = malloc(1024 * sizeof *names);
    int nnames = 0;
    bool any_cons = false;
    for(int m = 0; m < npicked; m++){
        Estimates *e = picked[m]->est;
        for(int j = 0; j < e->K; j++){
            if(!strcmp(e->xnames[j], "_cons")){ any_cons = true; continue; }
            if(has_keep && !name_in_list(e->xnames[j], keep_str)) continue;
            if(has_drop && name_in_list(e->xnames[j], drop_str)) continue;
            int found = 0;
            for(int k = 0; k < nnames; k++)
                if(!strcmp(names[k], e->xnames[j])){ found = 1; break; }
            if(!found && nnames < 1024){
                snprintf(names[nnames], 33, "%s", e->xnames[j]);
                nnames++;
            }
        }
    }
    if(any_cons && !has_keep) {
        snprintf(names[nnames], 33, "%s", "_cons");
        nnames++;
    }
    if(has_keep && name_in_list("_cons", keep_str)){
        snprintf(names[nnames], 33, "%s", "_cons");
        nnames++;
    }

    /* Open output destination */
    FILE *fp_out = stdout;
    if(outfile[0]){
        fp_out = fopen(outfile, "w");
        if(!fp_out){
            fprintf(stderr,"estout: cannot open %s for writing\n", outfile);
            free(names); return 603;
        }
    }

    /* --- Emit by format --- */
    char esc[64];

    if(fmt == FMT_LATEX){
        if(title[0]) fprintf(fp_out, "\\begin{table}[h]\n\\centering\n\\caption{%s}\n", title);
        if(label_str[0]) fprintf(fp_out, "\\label{%s}\n", label_str);
        /* Column spec: l for var names, c for each model */
        fprintf(fp_out, "\\begin{tabular}{l");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, "c");
        fprintf(fp_out, "}\n\\hline\\hline\n");
        if(!nomtitles){
            fprintf(fp_out, " ");
            for(int m = 0; m < npicked; m++)
                fprintf(fp_out, " & %s", latex_escape(display_names[m], esc, sizeof esc));
            fprintf(fp_out, " \\\\\n");
            fprintf(fp_out, " ");
            for(int m = 0; m < npicked; m++)
                fprintf(fp_out, " & %s", picked[m]->est->depvar);
            fprintf(fp_out, " \\\\\n");
        }
        fprintf(fp_out, "\\hline\n");

        for(int k = 0; k < nnames; k++){
            fprintf(fp_out, "%s", latex_escape(names[k], esc, sizeof esc));
            for(int m = 0; m < npicked; m++){
                Estimates *e = picked[m]->est;
                int j = est_idx_of(e, names[k]);
                if(j < 0 || e->omitted[j]){
                    fprintf(fp_out, " & ");
                } else {
                    double b = e->b[j];
                    double v = e->V[(size_t)j*e->K + j];
                    double se = v > 0 ? sqrt(v) : 0;
                    double t  = se > 0 ? b/se : 0;
                    const char *stars = want_stars ? sig_stars(fabs(t)) : "";
                    char coef[64];
                    fmt_coef(b, stars, coef, sizeof coef, FMT_LATEX);
                    fprintf(fp_out, " & %s", coef);
                }
            }
            fprintf(fp_out, " \\\\\n");

            if(show_se || show_t || show_p){
                fprintf(fp_out, " ");
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    int j = est_idx_of(e, names[k]);
                    if(j < 0 || e->omitted[j]){
                        fprintf(fp_out, " & ");
                    } else {
                        double v = e->V[(size_t)j*e->K + j];
                        double se = v > 0 ? sqrt(v) : 0;
                        double b = e->b[j];
                        double t = se > 0 ? b/se : 0;
                        char paren[40];
                        if(show_se)      snprintf(paren, sizeof paren, "(%.4g)", se);
                        else if(show_t)  snprintf(paren, sizeof paren, "(%.2f)", t);
                        else { double pval = 2.0*(1.0 - 0.5*(1.0 + erf(fabs(t)/M_SQRT2)));
                               snprintf(paren, sizeof paren, "[%.3f]", pval); }
                        fprintf(fp_out, " & %s", paren);
                    }
                }
                fprintf(fp_out, " \\\\\n");
            }
        }
        fprintf(fp_out, "\\hline\n");

        /* Stats footer */
        fprintf(fp_out, "N");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, " & %ld", picked[m]->est->N);
        fprintf(fp_out, " \\\\\n");

        if(stats_buf[0]){
            char buf[256]; snprintf(buf, sizeof buf, "%s", stats_buf);
            char *sp = NULL;
            for(char *st = strtok_r(buf, " ,", &sp); st; st = strtok_r(NULL, " ,", &sp)){
                if(!strcmp(st, "N")) continue;
                fprintf(fp_out, "%s", latex_escape(st, esc, sizeof esc));
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    double v = NAN;
                    if(!strcmp(st,"r2"))         v = e->r2;
                    else if(!strcmp(st,"r2_a"))  v = e->r2_a;
                    else if(!strcmp(st,"rmse"))  v = e->rmse;
                    else if(!strcmp(st,"F"))     v = e->F;
                    else if(!strcmp(st,"df_r"))  v = e->df_r;
                    else if(!strcmp(st,"df_m"))  v = e->df_m;
                    else if(!strcmp(st,"sigma_u")) v = e->sigma_u;
                    else if(!strcmp(st,"sigma_e")) v = e->sigma_e;
                    else if(!strcmp(st,"rho"))   v = e->rho;
                    if(!isnan(v) && isfinite(v)) fprintf(fp_out, " & %.4g", v);
                    else fprintf(fp_out, " & ");
                }
                fprintf(fp_out, " \\\\\n");
            }
        }
        fprintf(fp_out, "\\hline\\hline\n\\end{tabular}\n");
        if(want_stars){
            fprintf(fp_out, "\\footnotesize{$^{*}$ $p<0.10$, $^{**}$ $p<0.05$, $^{***}$ $p<0.01$.  ");
            if(show_se) fprintf(fp_out, "Standard errors in parentheses.");
            else if(show_t) fprintf(fp_out, "$t$-statistics in parentheses.");
            else if(show_p) fprintf(fp_out, "$p$-values in brackets.");
            fprintf(fp_out, "}\n");
        }
        if(title[0]) fprintf(fp_out, "\\end{table}\n");
    }
    else if(fmt == FMT_MARKDOWN){
        if(title[0]) fprintf(fp_out, "**%s**\n\n", title);
        fprintf(fp_out, "| Variable");
        for(int m = 0; m < npicked; m++){
            if(!nomtitles) fprintf(fp_out, " | %s", display_names[m]);
            else fprintf(fp_out, " | ");
        }
        fprintf(fp_out, " |\n");
        fprintf(fp_out, "|---");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, "|---");
        fprintf(fp_out, "|\n");

        for(int k = 0; k < nnames; k++){
            fprintf(fp_out, "| %s", md_escape(names[k], esc, sizeof esc));
            for(int m = 0; m < npicked; m++){
                Estimates *e = picked[m]->est;
                int j = est_idx_of(e, names[k]);
                if(j < 0 || e->omitted[j]){
                    fprintf(fp_out, " | ");
                } else {
                    double b = e->b[j];
                    double v = e->V[(size_t)j*e->K + j];
                    double se = v > 0 ? sqrt(v) : 0;
                    double t = se > 0 ? b/se : 0;
                    const char *stars = want_stars ? sig_stars(fabs(t)) : "";
                    char coef[64];
                    fmt_coef(b, stars, coef, sizeof coef, FMT_MARKDOWN);
                    fprintf(fp_out, " | %s", coef);
                }
            }
            fprintf(fp_out, " |\n");

            if(show_se || show_t || show_p){
                fprintf(fp_out, "| ");
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    int j = est_idx_of(e, names[k]);
                    if(j < 0 || e->omitted[j]){
                        fprintf(fp_out, " | ");
                    } else {
                        double v = e->V[(size_t)j*e->K + j];
                        double se = v > 0 ? sqrt(v) : 0;
                        double b = e->b[j];
                        double t = se > 0 ? b/se : 0;
                        char paren[40];
                        if(show_se)      snprintf(paren, sizeof paren, "(%.4g)", se);
                        else if(show_t)  snprintf(paren, sizeof paren, "(%.2f)", t);
                        else { double pval = 2.0*(1.0 - 0.5*(1.0 + erf(fabs(t)/M_SQRT2)));
                               snprintf(paren, sizeof paren, "[%.3f]", pval); }
                        fprintf(fp_out, " | %s", paren);
                    }
                }
                fprintf(fp_out, " |\n");
            }
        }
        fprintf(fp_out, "| N");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, " | %ld", picked[m]->est->N);
        fprintf(fp_out, " |\n");
        if(stats_buf[0]){
            char buf[256]; snprintf(buf, sizeof buf, "%s", stats_buf);
            char *sp = NULL;
            for(char *st = strtok_r(buf, " ,", &sp); st; st = strtok_r(NULL, " ,", &sp)){
                if(!strcmp(st, "N")) continue;
                fprintf(fp_out, "| %s", st);
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    double v = NAN;
                    if(!strcmp(st,"r2"))         v = e->r2;
                    else if(!strcmp(st,"r2_a"))  v = e->r2_a;
                    else if(!strcmp(st,"rmse"))  v = e->rmse;
                    else if(!strcmp(st,"F"))     v = e->F;
                    else if(!strcmp(st,"sigma_u")) v = e->sigma_u;
                    else if(!strcmp(st,"sigma_e")) v = e->sigma_e;
                    else if(!strcmp(st,"rho"))   v = e->rho;
                    if(!isnan(v) && isfinite(v)) fprintf(fp_out, " | %.4g", v);
                    else fprintf(fp_out, " | ");
                }
                fprintf(fp_out, " |\n");
            }
        }
        if(want_stars) fprintf(fp_out, "\n\\* p<0.10, ** p<0.05, *** p<0.01\n");
    }
    else { /* PLAIN */
        if(title[0]) fprintf(fp_out, "%s\n\n", title);
        int dash_per = 14;
        int total_dash = 20 + npicked * dash_per;
        fprintf(fp_out, "%-18s", "Variable");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, " %13s", display_names[m]);
        fprintf(fp_out, "\n");
        if(!nomtitles){
            fprintf(fp_out, "%-18s", "");
            for(int m = 0; m < npicked; m++) fprintf(fp_out, " %13s", picked[m]->est->depvar);
            fprintf(fp_out, "\n");
        }
        for(int i = 0; i < total_dash; i++) fputc('-', fp_out);
        fprintf(fp_out, "\n");
        for(int k = 0; k < nnames; k++){
            fprintf(fp_out, "%-18s", names[k]);
            for(int m = 0; m < npicked; m++){
                Estimates *e = picked[m]->est;
                int j = est_idx_of(e, names[k]);
                if(j < 0 || e->omitted[j]){ fprintf(fp_out, " %13s", "."); }
                else {
                    double b = e->b[j];
                    double v = e->V[(size_t)j*e->K + j];
                    double se = v > 0 ? sqrt(v) : 0;
                    double t = se > 0 ? b/se : 0;
                    const char *stars = want_stars ? sig_stars(fabs(t)) : "";
                    fprintf(fp_out, " %9.4g%-3s", b, stars);
                }
            }
            fprintf(fp_out, "\n");
            if(show_se || show_t || show_p){
                fprintf(fp_out, "%-18s", "");
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    int j = est_idx_of(e, names[k]);
                    if(j < 0 || e->omitted[j]){ fprintf(fp_out, " %13s", ""); }
                    else {
                        double v = e->V[(size_t)j*e->K + j];
                        double se = v > 0 ? sqrt(v) : 0;
                        double b = e->b[j];
                        double t = se > 0 ? b/se : 0;
                        char paren[40];
                        if(show_se)      snprintf(paren, sizeof paren, "(%.4g)", se);
                        else if(show_t)  snprintf(paren, sizeof paren, "(%.2f)", t);
                        else { double pval = 2.0*(1.0 - 0.5*(1.0 + erf(fabs(t)/M_SQRT2)));
                               snprintf(paren, sizeof paren, "[%.3f]", pval); }
                        fprintf(fp_out, " %13s", paren);
                    }
                }
                fprintf(fp_out, "\n");
            }
        }
        for(int i = 0; i < total_dash; i++) fputc('-', fp_out);
        fprintf(fp_out, "\n");
        fprintf(fp_out, "%-18s", "N");
        for(int m = 0; m < npicked; m++) fprintf(fp_out, " %13ld", picked[m]->est->N);
        fprintf(fp_out, "\n");
        if(stats_buf[0]){
            char buf[256]; snprintf(buf, sizeof buf, "%s", stats_buf);
            char *sp = NULL;
            for(char *st = strtok_r(buf, " ,", &sp); st; st = strtok_r(NULL, " ,", &sp)){
                if(!strcmp(st, "N")) continue;
                fprintf(fp_out, "%-18s", st);
                for(int m = 0; m < npicked; m++){
                    Estimates *e = picked[m]->est;
                    double v = NAN;
                    if(!strcmp(st,"r2"))         v = e->r2;
                    else if(!strcmp(st,"r2_a"))  v = e->r2_a;
                    else if(!strcmp(st,"rmse"))  v = e->rmse;
                    else if(!strcmp(st,"F"))     v = e->F;
                    else if(!strcmp(st,"sigma_u")) v = e->sigma_u;
                    else if(!strcmp(st,"sigma_e")) v = e->sigma_e;
                    else if(!strcmp(st,"rho"))   v = e->rho;
                    if(!isnan(v) && isfinite(v)) fprintf(fp_out, " %13.4g", v);
                    else fprintf(fp_out, " %13s", ".");
                }
                fprintf(fp_out, "\n");
            }
        }
        if(want_stars) fprintf(fp_out, "\n* p<0.10, ** p<0.05, *** p<0.01\n");
    }

    if(outfile[0]){
        fclose(fp_out);
        if(!c->quiet) fprintf(stderr, "(estout: wrote %s)\n", outfile);
    }
    free(names);
    return 0;
}

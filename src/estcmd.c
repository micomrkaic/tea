/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * estcmd.c — `estimates` command suite (store/restore/dir/drop/table).
 *
 * `estimates store name`     — clone last_est and save under <name>
 * `estimates restore name`   — replace last_est with a clone of <name>
 * `estimates dir`            — list saved names with cmd, N
 * `estimates drop name [...]` — remove named entries (or `_all`)
 * `estimates table [names] [, b se t p stats(...) star]`
 *                            — compact side-by-side table of saved
 *                              estimates (foundation for estout)
 *
 * The storage is a workspace-level linked list, freed by ws_free.
 *
 * `estimates table` defaults: shows coefficients, no SE.
 * Options:
 *   b              show coefficients (default ON)
 *   se             show SEs in parens below each coef
 *   t              show t-stats in parens
 *   p              show p-values in parens
 *   stats(N r2 ll) per-model summary rows
 *   star           add significance stars (*, **, ***)
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

/* ---- workspace helpers ----------------------------------------------- */

static StoredEst *stored_find(Workspace *ws, const char *name)
{
    for(StoredEst *s = ws->stored_est; s; s = s->next)
        if(!strcmp(s->name, name)) return s;
    return NULL;
}

static void stored_remove(Workspace *ws, const char *name)
{
    StoredEst **pp = &ws->stored_est;
    while(*pp){
        if(!strcmp((*pp)->name, name)){
            StoredEst *dead = *pp;
            *pp = dead->next;
            est_free(dead->est);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ---- estimates store -------------------------------------------------- */

static int do_estimates_store(Cmd *c, const char *arg)
{
    if(!c->ws->last_est){
        fprintf(stderr,"estimates store: no estimates available to store\n");
        return 301;
    }
    char name[33] = "";
    sscanf(arg, "%32s", name);
    if(!name[0]){
        fprintf(stderr,"estimates store: name required\n");
        return 198;
    }
    /* Reject names that conflict with reserved words or contain ugly chars. */
    if(name[0] == '_' || !strcmp(name, "all")){
        fprintf(stderr,"estimates store: name cannot start with '_' or be 'all'\n");
        return 198;
    }
    /* Replace if name already exists. */
    StoredEst *existing = stored_find(c->ws, name);
    if(existing){
        est_free(existing->est);
        existing->est = est_clone(c->ws->last_est);
        if(!c->quiet) fprintf(stderr,"(replaced estimates %s)\n", name);
        return 0;
    }
    StoredEst *s = calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->est = est_clone(c->ws->last_est);
    s->next = c->ws->stored_est;
    c->ws->stored_est = s;
    return 0;
}

/* ---- estimates restore ----------------------------------------------- */

static int do_estimates_restore(Cmd *c, const char *arg)
{
    char name[33] = "";
    sscanf(arg, "%32s", name);
    if(!name[0]){ fprintf(stderr,"estimates restore: name required\n"); return 198; }
    StoredEst *s = stored_find(c->ws, name);
    if(!s){
        fprintf(stderr,"estimates restore: %s not found\n", name);
        return 198;
    }
    est_free(c->ws->last_est);
    c->ws->last_est = est_clone(s->est);
    return 0;
}

/* ---- estimates dir --------------------------------------------------- */

static int do_estimates_dir(Cmd *c)
{
    if(!c->ws->stored_est){
        printf("(no stored estimates)\n");
        return 0;
    }
    /* Count first so we can format the header sensibly. */
    int n = 0;
    for(StoredEst *s = c->ws->stored_est; s; s = s->next) n++;
    printf("\n");
    printf("    %-20s %-12s %10s   %s\n", "name", "command", "N", "depvar");
    printf("    %-20s %-12s %10s   %s\n", "----", "-------", "-", "------");
    for(StoredEst *s = c->ws->stored_est; s; s = s->next){
        printf("    %-20s %-12s %10ld   %s\n",
               s->name, s->est->cmd, s->est->N, s->est->depvar);
    }
    printf("\n");
    return 0;
}

/* ---- estimates drop -------------------------------------------------- */

static int do_estimates_drop(Cmd *c, const char *arg)
{
    char tok[33];
    const char *p = arg;
    int n_dropped = 0;
    while(*p){
        while(*p == ' ') p++;
        if(!*p) break;
        int j = 0;
        while(*p && *p != ' ' && j < 32) tok[j++] = *p++;
        tok[j] = 0;
        if(!strcmp(tok, "_all")){
            StoredEst *s = c->ws->stored_est;
            while(s){ StoredEst *n2 = s->next; est_free(s->est); free(s); s = n2; n_dropped++; }
            c->ws->stored_est = NULL;
            break;
        }
        if(stored_find(c->ws, tok)){ stored_remove(c->ws, tok); n_dropped++; }
        else fprintf(stderr,"estimates drop: %s not found (skipped)\n", tok);
    }
    return 0;
}

/* ---- estimates table ------------------------------------------------- */

/* Print a compact side-by-side table of saved estimates.  The argument
 * is a space-separated list of stored-estimates names.  If empty,
 * uses all stored estimates in insertion order. */
static int do_estimates_table(Cmd *c, const char *arg)
{
    bool opt_b   = !opt_present(c->options, "b") ? true : opt_present(c->options, "b");
    bool opt_se  = opt_present(c->options, "se");
    bool opt_t   = opt_present(c->options, "t");
    bool opt_p   = opt_present(c->options, "p");
    bool opt_star= opt_present(c->options, "star") || opt_present(c->options, "stars");
    char stats_buf[256] = "";
    bool has_stats = opt_value(c->options, "stats", stats_buf, sizeof stats_buf);
    (void)opt_b;  /* assumed true */

    /* Collect requested estimates (names → Estimates*) */
    StoredEst *picked[64]; int npicked = 0;
    if(arg[0]){
        char tok[33]; const char *p = arg;
        while(*p){
            while(*p == ' ') p++;
            if(!*p) break;
            int j = 0;
            while(*p && *p != ' ' && j < 32) tok[j++] = *p++;
            tok[j] = 0;
            StoredEst *s = stored_find(c->ws, tok);
            if(!s){ fprintf(stderr,"estimates table: %s not found\n", tok); return 198; }
            if(npicked >= 64){ fprintf(stderr,"estimates table: too many models\n"); return 198; }
            picked[npicked++] = s;
        }
    } else {
        for(StoredEst *s = c->ws->stored_est; s && npicked < 64; s = s->next)
            picked[npicked++] = s;
    }
    if(npicked == 0){
        fprintf(stderr,"estimates table: no stored estimates\n");
        return 198;
    }

    /* Union of variable names across models, in first-seen order. */
    char (*names)[33] = malloc(1024 * sizeof *names);
    int nnames = 0;
    for(int m = 0; m < npicked; m++){
        Estimates *e = picked[m]->est;
        for(int j = 0; j < e->K; j++){
            int found = 0;
            for(int k = 0; k < nnames; k++)
                if(!strcmp(names[k], e->xnames[j])){ found = 1; break; }
            if(!found && nnames < 1024){
                snprintf(names[nnames], 33, "%s", e->xnames[j]);
                nnames++;
            }
        }
    }

    /* Print header */
    printf("\n");
    printf("%-18s", "Variable");
    for(int m = 0; m < npicked; m++) printf(" %12s", picked[m]->name);
    printf("\n");
    int dash_per = 13;
    int total_dash = 18 + npicked * dash_per;
    for(int i = 0; i < total_dash; i++) putchar('-');
    printf("\n");

    /* Critical t-values for stars (z-distribution; close enough at usual N): */
    double z90 = 1.6449, z95 = 1.9600, z99 = 2.5758;

    /* Body */
    for(int k = 0; k < nnames; k++){
        printf("%-18s", names[k]);
        for(int m = 0; m < npicked; m++){
            Estimates *e = picked[m]->est;
            int j = est_idx_of(e, names[k]);
            if(j < 0 || e->omitted[j]){
                printf(" %12s", ".");
            } else {
                double b = e->b[j];
                double v = e->V[(size_t)j*e->K + j];
                double se = v > 0 ? sqrt(v) : 0;
                double tstat = se > 0 ? b / se : 0;
                char stars[4] = "";
                if(opt_star){
                    if(fabs(tstat) > z99)      strcpy(stars,"***");
                    else if(fabs(tstat) > z95) strcpy(stars,"** ");
                    else if(fabs(tstat) > z90) strcpy(stars,"*  ");
                    else                       strcpy(stars,"   ");
                }
                printf(" %9.4g%s", b, opt_star?stars:"");
            }
        }
        printf("\n");

        if(opt_se || opt_t || opt_p){
            printf("%-18s", "");
            for(int m = 0; m < npicked; m++){
                Estimates *e = picked[m]->est;
                int j = est_idx_of(e, names[k]);
                if(j < 0 || e->omitted[j]){
                    printf(" %12s", "");
                } else {
                    double v = e->V[(size_t)j*e->K + j];
                    double se = v > 0 ? sqrt(v) : 0;
                    double b = e->b[j];
                    double tstat = se > 0 ? b / se : 0;
                    char paren[24];
                    if(opt_se)      snprintf(paren, sizeof paren, "(%.4g)", se);
                    else if(opt_t)  snprintf(paren, sizeof paren, "(%.2f)", tstat);
                    else            { /* p */
                        double p = se > 0 ? 2.0 * (1.0 - 0.5*(1.0 + erf(fabs(tstat)/M_SQRT2))) : 1.0;
                        snprintf(paren, sizeof paren, "[%.3f]", p);
                    }
                    printf(" %12s", paren);
                }
            }
            printf("\n");
        }
    }

    /* Footer / stats */
    for(int i = 0; i < total_dash; i++) putchar('-');
    printf("\n");
    /* Default footer: N for each model */
    printf("%-18s", "N");
    for(int m = 0; m < npicked; m++) printf(" %12ld", picked[m]->est->N);
    printf("\n");

    if(has_stats){
        /* Parse stats list */
        char buf[256]; snprintf(buf, sizeof buf, "%s", stats_buf);
        char *sp = NULL;
        for(char *t = strtok_r(buf, " ,", &sp); t; t = strtok_r(NULL, " ,", &sp)){
            if(!strcmp(t, "N")) continue;  /* already shown */
            printf("%-18s", t);
            for(int m = 0; m < npicked; m++){
                Estimates *e = picked[m]->est;
                double v = NAN;
                if(!strcmp(t,"r2") || !strcmp(t,"r2_p")) v = e->r2;
                else if(!strcmp(t,"r2_a"))               v = e->r2_a;
                else if(!strcmp(t,"rmse"))               v = e->rmse;
                else if(!strcmp(t,"F"))                  v = e->F;
                else if(!strcmp(t,"df_r"))               v = e->df_r;
                else if(!strcmp(t,"df_m"))               v = e->df_m;
                else if(!strcmp(t,"sigma_u"))            v = e->sigma_u;
                else if(!strcmp(t,"sigma_e"))            v = e->sigma_e;
                else if(!strcmp(t,"rho"))                v = e->rho;
                if(!isnan(v) && isfinite(v))
                    printf(" %12.4g", v);
                else
                    printf(" %12s", ".");
            }
            printf("\n");
        }
    }

    if(opt_star){
        printf("\nLegend: * p<0.10, ** p<0.05, *** p<0.01\n");
    }

    free(names);
    return 0;
}

/* ---- top-level dispatch ----------------------------------------------- */

int do_estimates(Cmd *c)
{
    /* Subcommand is first token of varlist. */
    char sub[16] = "";
    sscanf(c->varlist, "%15s", sub);
    if(!sub[0]){
        fprintf(stderr,"estimates: subcommand required (store|restore|dir|drop|table)\n");
        return 198;
    }
    const char *rest = c->varlist + strlen(sub);
    while(*rest == ' ') rest++;

    if(!strcmp(sub, "store"))   return do_estimates_store(c, rest);
    if(!strcmp(sub, "restore")) return do_estimates_restore(c, rest);
    if(!strcmp(sub, "dir"))     return do_estimates_dir(c);
    if(!strcmp(sub, "drop"))    return do_estimates_drop(c, rest);
    if(!strcmp(sub, "table"))   return do_estimates_table(c, rest);
    fprintf(stderr,"estimates: unknown subcommand %s\n", sub);
    return 198;
}

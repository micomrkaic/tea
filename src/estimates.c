/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include "estimates.h"
#include <stdlib.h>
#include <string.h>

Estimates *est_new(void){
    return calloc(1, sizeof(Estimates));
}

Estimates *est_clone(const Estimates *src){
    if(!src) return NULL;
    Estimates *e = calloc(1, sizeof *e);
    *e = *src;
    /* deep-copy owned arrays */
    if(src->K > 0){
        e->xnames  = malloc(src->K * sizeof *e->xnames);
        memcpy(e->xnames, src->xnames, src->K * sizeof *e->xnames);
        e->omitted = malloc(src->K * sizeof *e->omitted);
        memcpy(e->omitted, src->omitted, src->K * sizeof *e->omitted);
        e->b = malloc(src->K * sizeof *e->b);
        memcpy(e->b, src->b, src->K * sizeof *e->b);
        e->V = malloc((size_t)src->K * src->K * sizeof *e->V);
        memcpy(e->V, src->V, (size_t)src->K * src->K * sizeof *e->V);
    }
    if(src->nobs_at_fit > 0 && src->used){
        e->used = malloc(src->nobs_at_fit);
        memcpy(e->used, src->used, src->nobs_at_fit);
    }
    return e;
}

void est_free(Estimates *e){
    if(!e) return;
    free(e->xnames);
    free(e->omitted);
    free(e->b);
    free(e->V);
    free(e->used);
    free(e);
}

int est_idx_of(const Estimates *e, const char *name){
    if(!e || !e->xnames) return -1;
    for(int i=0;i<e->K;i++) if(!strcmp(e->xnames[i], name)) return i;
    return -1;
}


/* see estimates.h */
const char *gfit(double x, int w){
    static char bufs[16][32]; static int slot = 0;
    char *b = bufs[slot++ & 15];
    if (x != x){ snprintf(b, 32, "."); return b; }   /* NaN/missing */
    if (w > 30) w = 30;
    /* start at SIX significant digits, not the column's full capacity:
     * on ill-conditioned problems (longley!) OpenBLAS and the WASM
     * reference BLAS agree only to ~6-7 significant digits, and the
     * 7th digit leaking into tables broke golden byte-identity across
     * rigs.  Display precision is capped at the cross-rig reproducible
     * bound; the stored doubles keep full precision. */
    int p0 = w - 2; if (p0 > 6) p0 = 6;
    for (int prec = (p0 > 0 ? p0 : 1); prec >= 1; prec--){
        char t[48];
        snprintf(t, sizeof t, "%.*g", prec, x);
        /* Stata drops the leading zero of |x|<1 */
        char *p = t;
        char out[48]; size_t o = 0;
        if (p[0]=='-' && p[1]=='0' && p[2]=='.'){ out[o++]='-'; p += 2; }
        else if (p[0]=='0' && p[1]=='.') p += 1;
        for (; *p && o+1 < sizeof out; p++) out[o++]=*p;
        out[o]=0;
        if ((int)strlen(out) <= w || prec == 1){ snprintf(b, 32, "%s", out); return b; }
    }
    snprintf(b, 32, "%.1g", x);
    return b;
}

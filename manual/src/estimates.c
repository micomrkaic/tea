/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
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

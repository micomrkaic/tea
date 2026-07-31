/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unified sandwich-VCE module.  See DESIGN_VCE.md for the abstraction,
 * the Stata-compatibility policy table, and the migration plan. */
#ifndef TEA_VCE_H
#define TEA_VCE_H
#include <stddef.h>

typedef enum { VCE_DEFAULT = 0, VCE_ROBUST, VCE_CLUSTER } VceKind;
typedef struct {
    VceKind kind;
    char    cluster_var[33];
} VceSpec;

/* One parser for the whole option surface: bare `robust`, bare
 * `cluster(v)`, and `vce(robust|hc1|cluster v|ols)`.  opts is the
 * command's option string as stored on Cmd.  Returns 0 on success,
 * 198 (with the error already printed) on malformed input. */
int vce_parse(const char *opts, VceSpec *spec);

/* V = c * B M B (K x K, malloc'd; caller frees).
 * bread:  K x K, col-major.
 * scores: N x K, col-major (column j holds s_ij for all i); rows for
 *         omitted regressors must be zero (then V rows/cols are 0).
 * cid:    cluster id per observation (0..G-1), or NULL for robust.
 * c:      finite-sample adjustment from the policy table. */
double *vce_sandwich(const double *bread, const double *scores,
                     long N, int K, const long *cid, long G, double c);

/* Policy-table adjustments (DESIGN_VCE.md).  N obs, k model params,
 * G clusters. */
double vce_adj_ls_robust(long N, int k);              /* HC1 */
double vce_adj_ls_cluster(long N, int k, long G);     /* CR1 */
double vce_adj_ml_robust(long N);                     /* N/(N-1) */
double vce_adj_ml_cluster(long G);                    /* G/(G-1) */

/* Shared table furniture so every estimator prints identically. */
const char *vce_se_header(const VceSpec *s);          /* "Std. err." or "Robust\n std. err." label */
void vce_print_cluster_note(const char *cluster_var, long G);

#endif

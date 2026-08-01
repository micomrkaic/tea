/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kalman.h — the state-space engine interface (DESIGN_SSPACE.md App. B).
 */
#ifndef TEA_KALMAN_H
#define TEA_KALMAN_H

typedef struct {
    int m, p, r;          /* states, observation dim, disturbance dim */
    const double *Z;      /* p x m  col-major */
    const double *Hdiag;  /* p      observation variances (diagonal H) */
    const double *T;      /* m x m */
    const double *R;      /* m x r */
    const double *Q;      /* r x r */
    const double *a1;     /* m     initial mean */
    const double *Pstar1; /* m x m stationary part of P_1 */
    const double *Pinf1;  /* m x m diffuse indicator part of P_1 */
} SSModel;

typedef struct {
    double loglik;
    long   d;             /* diffuse scalar steps */
    long   nsteps;        /* total scalar updates */
    double *at;           /* optional: Tn x m predicted states (a_{t+1}) */
    double *Pt;           /* optional: Tn x m x m */
} SSFilterOut;

int    ss_lyapunov(const double *T, const double *RQR, int m, double *P);
int    ss_filter(const SSModel *M, const double *y, long Tn, SSFilterOut *out);
int    ss_smooth(const SSModel *M, const double *y, long Tn,
                 double *ahat, double *Vt, double *loglik_out, long *d_out);
double ss_loglik(const SSModel *M, const double *y, long Tn, long *d, long *nsteps);

#endif

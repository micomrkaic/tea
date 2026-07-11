/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "stats.h"
#include <gsl/gsl_cdf.h>
#include <math.h>

double tea_pval_t(double t, double df){
    return 2.0 * gsl_cdf_tdist_Q(fabs(t), df);
}
double tea_pval_f(double f, double df1, double df2){
    return gsl_cdf_fdist_Q(f, df1, df2);
}
double tea_pval_chi2(double x, double df){
    return gsl_cdf_chisq_Q(x, df);
}
double tea_invt(double p, double df){
    return gsl_cdf_tdist_Pinv(p, df);
}

/* Stata-named wrappers: ttail(df, t) = P(T_df > t), one-sided upper. */
double tea_ttail(double df, double t){
    return gsl_cdf_tdist_Q(t, df);
}
double tea_invttail(double df, double p){
    return gsl_cdf_tdist_Qinv(p, df);
}
double tea_invnormal(double p){
    return gsl_cdf_ugaussian_Pinv(p);
}

double tea_normal_cdf(double z){
    return gsl_cdf_ugaussian_P(z);
}
double tea_normal_pdf(double z){
    /* (2π)^(-1/2) ≈ 0.3989422804014327 */
    return 0.3989422804014327 * exp(-0.5 * z * z);
}
/* log Φ(z) — careful for large negative z where Φ(z) underflows to 0.
 * For z >= -5 use plain log(Φ(z)).  For z < -5 use the asymptotic
 * expansion log φ(z) - log|z| + log(1 - 1/z² + 3/z⁴ - ...).  GSL's
 * gsl_sf_log_erfc is the cleanest; we approximate via
 * log Φ(z) = log(erfc(-z/√2)/2) and use logspace where erfc tiny. */
double tea_log_normal_cdf(double z){
    if(z >= -5.0){
        double p = gsl_cdf_ugaussian_P(z);
        return log(p);
    }
    /* z < -5: |z| large.  log Φ(z) ≈ log φ(z) - log|z| + log(1 - 1/z²)
     * (using just two terms of the asymptotic series for stability). */
    double az = -z;
    double log_phi = -0.5*z*z - 0.918938533204672742;  /* log φ(z) */
    return log_phi - log(az) + log(1.0 - 1.0/(az*az));
}

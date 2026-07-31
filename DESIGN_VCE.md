# Design note: the unified VCE module (src/vce.c)

Status: approved design, implementation begins v1.6.33.
Owner: Mico Mrkaic.  Drafted from the code audit of 2026-07-31.

## Problem

tea computes robust/clustered standard errors in FOUR independent
implementations: regress (HC1 + CR1 in regress.c), the GLM family
logit/probit/poisson (score sandwich in do_glm_binary), ivregress
(its own 2SLS sandwich), and xtreg (within-model sandwich).  Audit
findings:

1. Drift risk is structural: four copies of the same mathematics,
   each with its own finite-sample adjustment code, none sharing
   tests.
2. One semantic BUG: `xtreg, fe robust` computes plain HC1 on the
   within regression.  Stata (since release 10) silently promotes
   robust to vce(cluster panelvar) for xtreg because HC1 is
   inconsistent under the within transformation (Stock & Watson,
   Econometrica 2008).  tea currently prints a different — wrong —
   number than Stata for the same command line.
3. The surface syntax is accidentally consistent today (bare
   `robust` / `cluster()` and `vce(robust)` / `vce(cluster v)` both
   parse in the audited paths) but only because the parsing code was
   copied around; nothing enforces it.

## The abstraction

Every sandwich VCE in scope is  V = c · B · M · B  where

- B ("bread", K×K) is estimator-specific and already computed by
  each estimator: (X'X)^-1 for OLS and the within model;
  (X̂'X̂)^-1 for 2SLS; the inverse negative Hessian for ML.
- M ("meat") is built from per-observation SCORE rows s_i (1×K):
  s_i = e_i·x_i (OLS/within), e_i·x̂_i with e_i from the ACTUAL
  regressors (2SLS), the gradient contribution (ML).
  robust:   M = Σ_i s_i' s_i
  cluster:  M = Σ_g u_g' u_g,  u_g = Σ_{i∈g} s_i
- c is a finite-sample adjustment from the policy table below.

The module therefore needs exactly three things from an estimator:
bread, the score matrix, and its row in the policy table.  The plain
(non-robust) VCE never touches the module — it stays estimator-owned
(σ²B for least squares, -H^-1 for ML).

## Policy table (the Stata-compatibility core)

| estimator        | robust c            | cluster c                     | test stat, df      |
|------------------|---------------------|-------------------------------|--------------------|
| regress          | N/(N-k)  (HC1)      | G/(G-1) · (N-1)/(N-k)  (CR1)  | t, N-k / t, G-1    |
| ivregress 2sls   | N/(N-k)             | G/(G-1) · (N-1)/(N-k)         | z (Stata uses z)   |
| logit/probit/    | N/(N-1)             | G/(G-1)                       | z, unchanged       |
| poisson (ML)     |                     |                               |                    |
| xtreg, fe        | PROMOTED to cluster | G/(G-1) · (N-1)/(N-k),        | t, G-1; model F    |
|                  | (panelvar) + note   | k = within slopes + intercept,| df (k_w, G-1)      |
|                  |                     | absorbed FEs NOT counted      |                    |

Cells in this table are hypotheses to be pinned by the Stata
verification run (tools/stata_check_vce.do) under the 6-digit
contract; any cell Stata contradicts is corrected to Stata, and the
table is the record.

## API

    typedef enum { VCE_DEFAULT, VCE_ROBUST, VCE_CLUSTER } VceKind;
    typedef struct {
        VceKind kind;
        char    cluster_var[33];
    } VceSpec;

    /* one parser for the whole surface: bare robust / cluster(v)
     * and vce(robust|hc1|cluster v).  Returns 0, or 198 with a
     * message already printed for malformed input. */
    int vce_parse(const char *options, VceSpec *spec);

    /* V = c · B M B, K×K malloc'd; scores col-major N×K; cid may be
     * NULL for robust.  Omitted-column expansion is the caller's
     * concern (it owns the omitted map). */
    double *vce_sandwich(const double *bread, const double *scores,
                         long N, int K, const long *cid, long G,
                         double c);

    /* the shared table furniture: "Robust" column header text and
     * the "(Std. err. adjusted for G clusters in var)" note, so all
     * estimators print identically. */
    const char *vce_se_header(const VceSpec *s);
    void        vce_print_cluster_note(const VceSpec *s, long G);

## Migration plan

1. v1.6.33: vce.c lands.  regress, glm, ivregress ported with
   NUMBERS UNCHANGED (their current adjustments already follow the
   table; porting is refactor, verified by the goldens not moving).
2. Same release: xtreg robust FIXED to promote to cluster(panelvar).
   This is a deliberate golden change wherever a test exercised the
   old wrong number, documented as the semantic bug it is.
3. Test 68: the estimator × {default, robust, cluster} matrix on
   grunfeld and nmes1988, locking cross-rig determinism.
4. tools/stata_check_vce.do: the same matrix for Stata at the IMF;
   its output pins the policy table to 6 digits.

## Non-goals (deliberately)

HC2/HC3, two-way clustering, bootstrap, jackknife, and HAC (newey)
are OUT of this release.  The scores-based meat interface is chosen
so HAC becomes "a different M from the same s_i" when the
time-series tier arrives; nothing else will need to change.

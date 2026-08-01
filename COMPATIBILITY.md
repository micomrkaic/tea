# Compatibility contract

**tea is a proper subset of Stata.**

Every tea do-file must run unchanged in Stata.  This is the project's
primary design rule and it takes precedence over every other
consideration including elegance, ergonomics, and consistency.

## What this means concretely

- Every tea command, option, and function — if it exists in Stata, must
  behave identically.  If it does not exist in Stata, it must not exist
  in tea, or it must be opt-in via the `--tea-extensions` flag.
- Default runtime mode is `--strict-stata`.  Anything Stata wouldn't
  accept is rejected.
- The `.dta` format is the supported exchange format.  Tea reads and
  writes Stata 14+ (.dta format 118).
- Numerical output should match Stata to at least 6 digits on
  coefficients and 4 digits on standard errors.

## What this rules out

- "Tea has a better way to do this" — if it conflicts with Stata, it's
  out, even if Stata's behavior is awkward.
- Convenience extensions that aren't gated behind `--tea-extensions`.
- Silently accepting commands or options Stata wouldn't accept.
- Output formats that don't match Stata's (table headers, column
  widths, statistical reporting conventions).

## What this rules in

- Implementations more efficient than Stata (different algorithm, same
  result) are fine.
- Bug-for-bug compatibility is NOT required — if Stata has a known bug,
  tea may produce the correct result.  This is the one explicit exception.
- Performance, error messages, REPL ergonomics may differ as long as
  the observable behavior on do-files matches.

## Storage-type semantics

Stata has five numeric storage types (`byte`, `int`, `long`, `float`,
`double`) and per-variable width.  Tea stores all numeric values
internally as IEEE double regardless of declared type.  This is a
**deliberate design decision** locked in v0.1; it simplifies the
engine and is invisible to users in almost all cases.

### How the round-trip works

- **Reading `.dta`**: every numeric storage type (byte/int/long/float/
  double) is upcast to double on load.  No value changes — the bit
  patterns of float and the integer ranges of byte/int/long all fit
  losslessly in double.  This costs RAM relative to Stata's native
  representation, but no precision.

- **Writing `.dta`**: every numeric column is per-column compressed to
  the smallest Stata type that fits its actual values losslessly,
  matching what Stata's own `compress` command would produce.  Files
  saved by tea open in Stata with the same storage types they'd have
  if a Stata user had run `compress` before `save`.

- **In-memory**: the type qualifier on `gen byte x = ...` is accepted
  but is essentially a hint that affects nothing at runtime.  The
  column is double; on save, if every value of `x` fits in a byte, it
  will be written as a byte.

### Where this diverges from Stata

In strict Stata, declaring `gen byte x = 1` and then attempting
`replace x = 999` is an error (999 exceeds byte range).  In tea, the
declared type is advisory, so the replace succeeds; on save, the
column is written as the smallest type that fits 999 (int).  This is
one observable behavioral divergence from Stata, in the direction of
permissiveness.

Storage-type strictness will be available as a `--tea-extensions`
opt-in in a future release if there's demand.  In the meantime,
strict storage-type enforcement is documented as a known divergence,
not a bug.

## Documented no-op options

These Stata options are accepted by tea but have no effect, because the
underlying behavior they control isn't relevant in tea's environment:

- `set more on/off` — pagination.  Tea doesn't paginate output; rely on
  your terminal's scrollback or `log using FILE` to capture output for
  later review.  Modern terminals make pagination obsolete.
- `set linesize N` — line wrapping width.  Tea doesn't wrap; if you
  want narrower output, resize your terminal.
- `set matsize N` — maximum matrix size.  Tea has no fixed matrix-size
  limit.
- `set type float|double` — default storage type for new numeric
  variables.  Tea stores all numerics as double regardless.
- `compress` — Stata's `compress` reduces in-memory storage by picking
  the smallest type per column.  Tea is always uncompressed (double)
  in memory; on `save`, columns are automatically compressed.  So
  `compress` issued by the user is a no-op.
- `recast TYPE varlist` — change the storage type of existing
  variables.  Tea's storage is type-agnostic; the declared type is
  updated but the in-memory representation does not change.

Each of these can be set in a do-file without consequence; the
operation is silently a no-op.

## Current tea-extensions

The following are accepted only with `--tea-extensions`.  All produce
a clear "tea extension" error message in strict-stata mode (the
default).

- **`mkdir DIR, recursive`** (and the `p` alias) — creates intermediate
  parents like `mkdir -p`.  Stata's `mkdir` creates only one level.

This list is intentionally short — most things you might expect to be
tea-extensions are actually documented Stata commands (`dir`/`ls`/`rm`
are all official Stata, despite their Unix flavor).

## Reporting compatibility breaks

If you find a tea behavior that differs from Stata, file a bug.
Include:

- The shortest do-file that reproduces the divergence.
- The Stata output for the same input.
- The tea output.

These are treated as bugs, not feature requests.

## Graphics option tolerance (v1.6.6)

Inside graphics commands (`twoway`, `graph box`, `graph combine`, and
their options), unknown **cosmetic** suboptions are accepted and
ignored rather than rejected: Stata's graph grammar has a very long
tail of decorations (`intensity()`, `medtype()`, `box()`, marker
sizing, scheme controls, ...) and real-world do-files nearly always
carry some.  Structure is still strict — an unknown plot type, a
missing variable, a malformed series, an unbalanced paren, or a
name() collision all fail loudly with the usual return codes.
Everywhere outside graphics, tea remains strict about unknown options.

Documented graphics deviations from Stata:

- `name(NAME)` also writes `NAME.svg` to the working directory (Stata
  keeps named graphs only in memory until `graph export`).
- `twoway` sorts each `line` / `lowess` series by x before connecting
  (Stata connects in data order); scatter order is irrelevant.
- The legend is a simple swatch list drawn inside the plot region,
  top-right, rather than Stata's below-plot legend.

## Time-series inference tier (v1.6.35)

- Unit-root p-values ("MacKinnon approximate p-value") are computed by
  probit-space interpolation through the finite-sample 1/5/10%
  MacKinnon (2010) response-surface quantiles, not the MacKinnon
  (1994) p-value regressions Stata uses.  Test statistics and the
  printed critical values match Stata/statsmodels exactly; mid-range
  p-values can differ by ~0.02 (e.g. 0.3548 vs 0.3725).
- var: Sigma uses the ML divisor T, as Stata does.  statsmodels
  divides by T-k, so its orthogonalized IRFs differ by the factor
  sqrt(T/(T-k)); the Phi matrices agree exactly.
- vecrank prints Osterwald-Lenum (1992) 5% trace critical values —
  Stata's tables.  statsmodels prints MacKinnon-Haug-Michelis values
  (e.g. 29.68 vs 29.80 at m-r=3).  Trace statistics agree exactly.
- irf is an in-memory subset (create/table from the last var); no .irf
  files, sets, or fevd.  lpirf is univariate (own-shock responses).

## State-space tier, release one (v1.6.36)

- ucm uses EXACT diffuse initialization (Koopman 1997) with univariate
  filtering; log-likelihoods match statsmodels' use_exact_diffuse=True
  to 5 decimals.  Stata's ucm approximates the diffuse prior with a
  large kappa, so reported log-likelihoods can differ in the diffuse
  terms; variance estimates and smoothed states should agree to
  reported precision.
- The ML optimizer (BFGS, central-difference gradients, three
  deterministic starting points) can in principle land at different
  points of a flat likelihood across BLAS backends; regression goldens
  therefore round derived series before summarizing, and the release
  gate enforces cross-rig identity of everything printed.
- Release one implements models ntrend/llevel/lltrend/rwalk/rwdrift +
  dummy seasonal; cycles, sspace, dfactor, and exact-ML arima follow
  per DESIGN_SSPACE.md.

## Exact-ML arima (v1.6.37)

- arima now computes exact maximum likelihood through the state-space
  engine (differencing first, mean/regression-intercept
  parameterization for the constant — both Stata's conventions).
  Verified against statsmodels exact ML: identical log-likelihoods to
  4 decimals on AR, MA, ARMA-with-constant, ARIMA(0,1,1), and
  regression-with-ARMA-errors test problems.
- Standard errors are OIM: the numerical Hessian of the exact
  likelihood (computed in the transformed parameter space, delta
  method back).  Stata's arima defaults to OPG/BHHH, so SEs differ in
  finite samples.  Note also that statsmodels' cov_type='oim' for
  ARMA terms is itself an approximation: the finite-difference Hessian
  of statsmodels' own log-likelihood at its own optimum reproduces
  tea's standard errors (to 5 decimals), not the ones statsmodels
  reports.

## dfactor and the constraints subsystem (v1.6.39)

- dfactor is exact ML via the state-space engine, k <= 4 factors with
  full interacting AR(p <= 4) dynamics, Var(v)=I normalization, iid
  idiosyncratic errors (idiosyncratic ar() is staged).  Verified
  against statsmodels DynamicFactor: k=1 demeaned loglik identical to
  5 decimals; a k=2 model identified by a triangular constraint
  reproduces the unconstrained statsmodels optimum's loglik exactly
  (constraints used for identification are normalizations and cost
  zero likelihood).  On boundary (Heywood) cases tea's multistart
  optimizer reaches higher likelihoods than statsmodels' default.
- k >= 2 without constraints() runs, per Stata, but is identified only
  up to rotation; tea prints a note.  Goldens and the Stata check
  script only ever lock constrained forms.  A deterministic sign
  convention (first loading positive) is applied when unconstrained.
- constraint define/list/drop implements Stata's linear-constraint
  language; constraints are parsed at estimation time against the
  model's parameter names.  Estimation under R theta = r is by exact
  reparameterization theta = theta_p + N psi (SVD null basis), so any
  linear constraint set is handled without penalties or Lagrangians.
  Constrained coefficients print with "(constrained)".  Constraints on
  variance parameters are not accepted in this release.
- The nonstationarity guard: a factor transition with roots on or
  outside the unit circle is rejected during likelihood evaluation by
  positive-definiteness of the Lyapunov solution (an algebraic
  solution exists for many explosive T; only the PD one is a
  covariance).

## sspace subset (v1.6.40)

- sspace implements time-invariant stationary models: state equations
  linear in lag-1 states (higher lags via auxiliary identity states),
  observation equations linear in contemporaneous states with
  optional constants and per-equation noerror; covstate(identity)
  default and covstate(diagonal), covobs(diagonal) only (the engine's
  diagonal-H requirement).  Identification is the user's job through
  the constraints subsystem, as in Stata.  Nonstationary
  specifications are evaluation errors (the PD-Lyapunov guard);
  Stata's diffuse option is staged.
- Verified: the AR(1) written as sspace reproduces arima's exact ML
  to reported precision INCLUDING the OIM standard error (same
  likelihood through a different front door); a noisy-AR(1)
  signal-extraction model matches a hand-written statsmodels custom
  MLEModel's optimum to 5 decimals; identity-vs-diagonal
  normalizations reach identical likelihoods with the scale migrated
  into the loading.

## set seed and the Box-Muller spare (Bug 40, v1.6.40)

- tea_srand previously reset the PCG stream but not the Box-Muller
  spare, so after an odd number of prior normal draws a stale spare
  survived `set seed` and shifted the post-seed stream by one draw.
  Stata's set seed fully determines subsequent draws; tea's now does
  too.  No shipped golden depended on the buggy behavior (all
  mid-file re-seeds in the suite happened to sit at even draw
  parity), so no golden changed.

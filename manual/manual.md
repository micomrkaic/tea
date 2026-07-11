# Introduction

## What `tea` is

`tea` is a C17 program that runs as either an interactive REPL or a
batch processor for do-files. It does the data manipulation and
econometrics that most applied economists do most days — and nothing
else. No plotting, no Bayesian inference, no machine learning. For
those, escape to Python or R; see
the "Escape hatches" chapter.

The design priority is *Stata fidelity in the overlapping subset*. A
do-file that runs in `tea` should run in Stata with the same results,
modulo a handful of small documented differences. Where Stata’s behavior
includes a footgun (`if x > 5` includes missing values, for instance,
because missing sorts above every number), `tea` keeps the footgun. That
choice is deliberate: it makes `tea` a drop-in for the data-prep phase,
even if you eventually move to Stata or R for the final analysis.

## What `tea` is not

`tea` v1.0 does *not* implement: graphics of any kind, exact-ML ARIMA
(we use conditional likelihood), seasonal ARIMA, GARCH, mixed-effects
models, survival analysis, structural breaks, VAR, VECM, cointegration
tests, Bayesian inference, or machine learning. It also does not
implement Stata’s full programming language — `tea` has macros, loops,
and `capture`, but no `program define`, no `syntax` parser, no Mata.

The `shell` command makes it easy to bridge to Python or R when you need
any of the above. See the "Escape hatches" chapter.

## Design choices, locked

- **Storage** is one of two types: `double` (numeric, with the full
  Stata missing-value algebra) or string. Stata’s `byte`, `int`, `long`,
  `float` are cosmetic on read and compressed back on write of `.dta`
  files.

- **Multiple frames** are supported. Each has its own sort state and
  panel/time settings.

- **`by:` is strict**. It errors if the data isn’t sorted; `bysort`
  sorts first.

- **`reshape` is in-place**. Use `frame copy` first if you want to keep
  the original.

- **`m:m` merges are rejected**. They are almost never what the user
  actually wants.

- **C17**, no novelties. Toolchain stability over newer-standard
  ergonomics.

# Installation

## Build from source

You will have received a tarball, `tea-v1.0.tar.gz`.

``` bash
tar xzf tea-v1.0.tar.gz
cd tea
make
```

The build produces `./tea` in the project root. Move it somewhere on
your `$PATH` (e.g. `~/bin`) or invoke directly with `./tea`.

## Dependencies

You need a C17 compiler (`gcc` 8+ or `clang` 7+) and the following
libraries with their development headers:

<div class="center">

| Library       | Why                                                   |
|:--------------|:------------------------------------------------------|
| `libreadline` | line editing, history, tab completion in the REPL     |
| `libopenblas` | matrix kernels                                        |
| `liblapacke`  | LAPACK C interface (regression, ARIMA, IV)            |
| `libgsl`      | distributions for $p$-values and confidence intervals |
| `libreadstat` | Stata `.dta` (and SAS / SPSS) I/O                     |

</div>

On Debian / Ubuntu:

``` bash
apt install libreadline-dev libopenblas-dev liblapacke-dev \
            libgsl-dev libreadstat-dev
```

On macOS (Homebrew):

``` bash
brew install readline openblas lapack gsl readstat
```

Run `make check-deps` after installing to verify every required header
is reachable. If any dependency is missing, that target reports which
one and the install command to use.

## Sanity check

``` bash
./tea --version          # prints "tea 1.0 -- tiny econometric assistant"
./tea tests/demo.do      # batch: runs a short demo (no errors expected)
make test                # full regression suite, 39 tests
```

If `make test` reports `tea regression: 39/39 passed`, your build is
correct.

## Running interactively

Start the REPL by invoking `tea` with no arguments:

``` bash
$ tea
        (  (
         )  )      tea 1.2.2 — tiny econometric assistant
      .........    free Stata-like data analysis at the command line
      |       |]   GPLv3 · Mico Mrkaic · github.com/micomrkaic/tea
      \       /
       `-----´     type help · sysuse dir for practice datasets · Ctrl-D exits
. _
```

History is persistent at `~/.tea_history` (capped at 2000 lines).
Readline editing is available throughout: arrow keys, Ctrl-A/E/K/U/W,
Ctrl-L to clear, Up/Down history.  Tab completes command names at the
start of a line, dataset names after `sysuse`, and variable names
elsewhere.  `help` lists commands, `help CMD` shows the syntax for one.
Start with `tea -q` to suppress the banner; the browser edition offers
the same editing and completion.

# Your first `tea` session

Let’s load a small panel dataset, look at it, and run a regression. Save
the following as `quickstart.do`:

    * quickstart.do
    import delimited "tests/WEO.csv", clear
    rename INDICATOR_ID indicator_id
    keep if indicator_id == "NGDP_RPCH"
    rename COUNTRY_ID country_id
    keep country_id v*
    reshape long v, i(country_id) j(year)
    rename v growth
    keep if !missing(growth)

    * Restrict to G7 countries for tractable output
    keep if inlist(country_id, "USA", "DEU", "GBR", "FRA", "ITA", "JPN", "CAN")

    * Convert string country codes to numeric so we can use factor vars
    encode country_id, gen(cid)
    xtset cid year

    summarize growth
    xtdescribe

    * Fixed-effects regression of growth on its own lag
    xtreg growth L.growth, fe

Run it:

``` bash
tea quickstart.do
```

This loads the IMF *World Economic Outlook* CSV (shipped with the
distribution), pivots it from wide to long, restricts to G7 countries,
sets up the panel structure with `xtset`, and runs a fixed-effects
regression. Output looks like Stata’s; if you’ve used Stata before,
nothing here should surprise you.

A few things to notice:

- `rename`, `keep if`, `reshape long` all work the same as in Stata.

- `encode` maps string country IDs to consecutive integers 1, 2, 3,
  $\ldots$ and attaches a value label so `list cid` still shows “USA”,
  “DEU”, etc.

- `xtset cid year` declares the panel structure. After this, `L.growth`
  means “growth lagged one year, gap-aware within country.”

- `xtreg ..., fe` runs fixed effects (within transformation), with
  classical SEs by default. Add `vce(robust)` for HC1 or
  `vce(cluster cid)` for clustered SEs.

# The do-file language

A do-file is a sequence of *commands*, one per line, with the following
syntax skeleton:

    [prefix:] command [varlist] [if exp] [in range] [weight] [, options]

## Comments

Four comment forms are supported:

    * Whole-line comment starting with asterisk
    // Inline comment to end of line
    /* Block comment
       spanning multiple lines */
    generate x = 1 ///
           + 2     // line continuation: combine the next line with this one

## Macros

Local macros are referenced as “`` `name' ``” (back-tick … tick); global
macros as `$name`.

    local x = 5
    display "x = `x'"             // x = 5

    global path "/home/me/data"
    use "$path/employment.dta", clear

    local vars "gdp pop unemp"
    foreach v of local vars {
        summarize `v'
    }

After most estimation commands, results are stored in the `e()`
namespace:

    regress y x1 x2
    display e(N)         // sample size
    display e(r2)        // R-squared
    display _b[x1]       // x1's coefficient
    display _se[x1]      // x1's standard error

Summarize stores results in `r()`:

    summarize gdp
    display r(mean)
    display r(sd)

## Control flow

    * Conditional
    if `n' > 100 {
        display "large sample"
    }
    else {
        display "small sample"
    }

    * Foreach over a list, varlist, or numlist
    foreach v of varlist gdp pop unemp {
        quietly summarize `v'
        display "`v': N = " r(N) ", mean = " r(mean)
    }

    * Forvalues
    forvalues i = 1/5 {
        display "iteration `i'"
    }

## Capture and quietly

`quietly` suppresses output; `capture` additionally suppresses errors
(storing the return code in `_rc`):

    quietly regress y x       // runs, no output
    display _b[x]              // coefficient is still available

    capture confirm variable z // doesn't error if z doesn't exist
    if _rc != 0 {
        display "z is missing"
    }

## The `by:` prefix

    sort country year                // required before by:
    by country: generate dy = y - y[_n-1]

    bysort country (year): generate y_first = y[1]   // sorts first

`tea` enforces the sort requirement — using `by:` on unsorted data is a
hard error, not a silent miscomputation. Use `bysort` when you want it
to sort automatically.

# Data management

## Importing data

`tea` reads CSV, TSV, Stata `.dta`, Excel `.xlsx`, and OpenDocument
`.ods` files, plus its own native `.tea` binary format.

    import delimited "data.csv", clear      // CSV
    import delimited "data.tsv", clear      // TSV (auto-detected)
    import excel "data.xlsx", firstrow clear
    import excel "data.xlsx", sheet("Sheet2") firstrow clear

    use "data.dta", clear                   // Stata format
    use "data.tea", clear                   // native tea format

`tea` auto-detects the delimiter for `.csv` and `.tsv`; if the file
extension is ambiguous, pass `delimiter(",")` or `delimiter(tab)`
explicitly.

The `firstrow` option (for Excel) tells `tea` to use the first row as
variable names. Without it, columns are named `A`, `B`, etc.

CSV import handles RFC 4180–style quoted fields, including embedded
commas and newlines.

## Saving data

    save "out.dta", replace            // Stata format (default for .dta)
    save "out.tea", replace            // native tea format
    export delimited "out.csv", replace
    export delimited "out.tsv", replace

**Format choice.** Use `.dta` if the data will be read by Stata. Use
`.tea` if it’s a temporary file between `tea` sessions (faster to
read/write, no readstat overhead). Use `.csv` or `.tsv` if a
non-statistical tool will consume the data.

## Generating and modifying variables

    generate gdp_log = log(gdp)
    generate growth = (gdp - gdp[_n-1]) / gdp[_n-1]
    replace growth = . if year == 1990             // missing for first year

    generate str8 region = "WEST"
    replace region = "EAST" if country_id == "CHN"

The `str#` prefix declares a string variable (the number is the maximum
width, used by Stata when writing `.dta` files; in `tea` it’s cosmetic
on read but compressed back on write).

## `egen` extended generators

`egen` provides aggregate functions that `generate` can’t do in a single
expression:

    egen mean_gdp     = mean(gdp), by(country)
    egen total_pop    = total(pop)
    egen rank_gdp     = rank(gdp)
    egen group_id     = group(country region)         // assigns 1..K
    egen row_total    = rowtotal(x1 x2 x3 x4)
    egen has_any      = anyvalue(x), values(1 5 9)

The full list of functions: `mean`, `sum`, `total`, `count`, `min`,
`max`, `sd`, `var`, `median`, `group`, `rank`, `rowtotal`, `rowmean`,
`rowmin`, `rowmax`, `rowmiss`, `rownonmiss`, `tag`, `seq`, `anyvalue`,
`concat`.

## Type conversion

    * Numeric -> string
    tostring year, generate(year_str)

    * String -> numeric (errors if non-numeric values present)
    destring score, generate(score_n)
    destring score, generate(score_n) force      // converts non-numeric to .

    * String -> integer codes + value label
    encode country_id, generate(cid)             // alphabetical 1..K

    * Integer codes -> string (inverse of encode)
    decode cid, generate(country_str)

    * Recoding numeric values
    recode age (0/17 = 1 "minor") (18/64 = 2 "working") (65/max = 3 "senior"), gen(age_cat)

## Labels

    label data "World Economic Outlook subset"
    label variable growth "Real GDP growth (%)"

    label define region 1 "North" 2 "South" 3 "East" 4 "West"
    label values region region

# Exploring data

## `describe`

    describe                          // all variables
    describe gdp pop                  // just these two
    describe gdp*                     // wildcards work

Shows variable name, storage type, display format, and variable label.

## `list`

    list                              // all rows (capped at 200)
    list in 1/10                      // first 10 rows
    list country year gdp if year == 2020

Column widths auto-fit to the maximum of header and value widths.

## `summarize`

    summarize                         // all numeric variables
    summarize gdp, detail             // adds percentiles, skewness, kurtosis
    summarize gdp if year >= 2000
    bysort country: summarize gdp

After `summarize`, the following `r()` macros are set: `r(N)`,
`r(mean)`, `r(sd)`, `r(min)`, `r(max)`, `r(sum)`, `r(Var)`. With
`detail`, also `r(p1)`, `r(p5)`, `r(p10)`, `r(p25)`, `r(p50)`, `r(p75)`,
`r(p90)`, `r(p95)`, `r(p99)`, `r(skewness)`, `r(kurtosis)`.

## `tabulate`

    tabulate region                                 // one-way frequency table
    tabulate region year, row column cell           // two-way + percentages
    tabulate region, summarize(gdp) means           // means of gdp by region

## `codebook`

    codebook                          // all variables
    codebook gdp                      // just one

Reports type, unique values, missing count, range, and distribution
summary. More compact than `summarize, detail` for screening a new
dataset.

## `count`

    count                             // total observations
    count if missing(gdp)
    count if gdp > 0 & year >= 2000

After `count`, `r(N)` holds the count.

# Reshaping, merging, aggregating

## `sort` and `gsort`

    sort country year                 // ascending on both
    gsort -gdp                        // descending on gdp
    gsort country -year               // mixed

## `reshape`

`reshape` is in-place — to keep the original you must `frame copy`
first.

    * Wide to long: each yY variable becomes y at year Y
    reshape long y, i(country) j(year)

    * Long to wide
    reshape wide y, i(country) j(year)

    * Multiple stubs
    reshape long y z, i(country) j(year)

## `merge`

    merge 1:1 country year using "macro.dta"
    merge m:1 country     using "country_attrs.dta"
    merge 1:m country year using "panel_history.dta"

After `merge`, the variable `_merge` encodes the match status: 1 =
master only, 2 = using only, 3 = matched.

`m:m` is deliberately rejected. If you think you need `m:m`, you almost
certainly want a different operation (`joinby`-style cross product, or
an explicit deduplication before merging).

## `collapse`

    collapse (mean) gdp pop (sum) revenue (sd) sd_gdp = gdp, by(country)
    collapse (median) gdp [aweight=pop], by(region year)

Available aggregators: `mean`, `median`, `sum`, `min`, `max`, `sd`,
`count`, `first`, `last`, `p25`, `p50`, `p75`, `p10`, `p90`.

## Frames

A frame is a named in-memory dataset. Multiple frames let you keep
several datasets loaded at once.

    frame create macro
    frame change macro
    use "macro.dta", clear

    frame change default
    * now we're back on the original frame

    frame copy default backup    // duplicate default into a new frame
    frame drop backup

# Time series and panel data

## Declaring panel / time structure

    tsset year                        // pure time series
    xtset country year                 // panel
    xtset country year, yearly         // explicit time unit

After this, time-series operators are valid:

<div class="center">

| Operator   | Meaning                              |
|:-----------|:-------------------------------------|
| `L.x`      | first lag (one period back)          |
| `L2.x`     | second lag                           |
| `L(1/3).x` | first through third lags             |
| `F.x`      | first lead                           |
| `D.x`      | first difference                     |
| `D2.x`     | second difference (Stata’s notation) |
| `S.x`      | seasonal difference                  |

</div>

These can mix with factor variables (with one limitation; see
the "Factor variables" chapter):

    regress y L.y x i.region              // OLS with lag and region dummies
    xtreg y L(1/3).y x, fe                 // panel FE with three lags

`tea`’s time-series operators are *gap-aware*: if a country has data for
1995 but not 1996, `L.y` for 1997 returns missing (not the 1995 value).

## `xtdescribe`

    xtdescribe

Reports number of panels, observations per panel (min/avg/max), and
sample pattern. Useful for checking whether a panel is balanced.

# Factor variables

Stata’s factor-variable grammar lets you include categorical regressors
and their interactions inline, without manually generating dummies.
`tea` supports the same grammar across every estimator (`regress`,
`xtreg`, `logit`, `probit`, `poisson`, `ivregress`).

## Atoms

<div class="center">

| Token     | Meaning                                                |
|:----------|:-------------------------------------------------------|
| `i.var`   | dummies for each non-base level; base = smallest level |
| `ib2.var` | same, with base level explicitly set to 2              |
| `c.var`   | continuous regressor (mostly for use inside `#`)       |

</div>

    regress wage exper i.region              // region dummies
    regress wage exper ib2.region            // base = region 2

`i.var` requires `var` to be numeric. For string identifiers (e.g. ISO
country codes), use `encode` first:

    encode country_id, gen(cid)
    regress y x i.cid

## Interactions

<div class="center">

| Token  | Meaning                                              |
|:-------|:-----------------------------------------------------|
| `A#B`  | cross product: $(K_A - 1) \times (K_B - 1)$ columns  |
| `A##B` | main effects + interaction (equivalent to `A B A#B`) |

</div>

    regress wage i.region#c.exper            // region-specific slopes
    regress wage i.region##c.exper           // adds main effects of region and exper
    regress wage i.region#i.year             // region-by-year interaction

The `c.` prefix tells `tea` to treat the variable as continuous in an
interaction — without it, a numeric variable would be expanded as a
factor.

## Coefficient naming

Factor expansions produce coefficients with structured names that
`predict`, `margins`, and `_b[]` all understand:

    regress wage i.region
    display _b[2.region]                      // coefficient on region 2 dummy
    display _b[3.region#c.exper]              // interaction coefficient

## Known limitation: factor × TS-op composition

Tokens like `i.country#c.L.gdp` (factor interaction with a lagged
regressor) are **not** supported in v1.0.

**Workaround:** pre-compute the lag manually and use the bare variable
inside the interaction:

    generate L1_gdp = L.gdp
    regress y i.country#c.L1_gdp                // works

# Linear regression

## Basic syntax

    regress y x1 x2 x3
    regress y x1 x2, vce(robust)               // HC1 robust SEs
    regress y x1 x2, vce(cluster country)      // CR1 clustered SEs
    regress y x1 x2 [fweight=n], vce(robust)   // frequency weights

Weight types: `fweight` (frequency, integer), `aweight` (analytic,
rescaled to sum to $N$), `pweight` (probability, no rescaling),
`iweight` (importance, like `aweight` but no rescaling).

## Output

A standard ANOVA table with $F$-statistic and $R^2$, followed by the
coefficient table with $t$-statistics, $p$-values, and 95% confidence
intervals. After `regress`, the following are stored:

- `e(N)`, `e(r2)`, `e(r2_a)`, `e(rmse)`, `e(F)`, `e(p)`, `e(df_r)`,
  `e(df_m)`

- `_b[varname]` and `_se[varname]` for each coefficient, plus
  `_b[_cons]` and `_se[_cons]`.

## Robust and clustered SEs

The implementation uses HC1 for robust and CR1 for clustered, matching
Stata’s defaults. Cluster variables may be string (panel IDs) or
numeric.

## Constraints and hypothesis tests

After `regress`, use `test` for joint hypotheses and `lincom` for linear
combinations:

    regress y x1 x2 x3 x4

    test x1 = x2                                  // single restriction
    test x1 = 0, x2 = 0                           // joint (F-test)
    test x1 x2 x3                                 // all = 0 jointly

    lincom x1 + x2                                 // linear combination + CI
    lincom 0.5*x1 + 0.5*x2 - x3

# Panel regression

## Fixed effects

    xtset country year
    xtreg y x1 x2, fe                              // within (default for xtreg)
    xtreg y x1 x2, fe vce(cluster country)

Output includes:

- Within / between / overall $R^2$

- $\sigma_u$, $\sigma_e$, $\rho$ (variance components)

- $F$-test of $u_i = 0$

- `corr(u_i, Xb)` — informally measures the FE–RE departure

Identifies as `e(cmd) = "xtreg"` with `e(model) = "fe"`.

## Random effects

    xtreg y x1 x2, re                              // Swamy-Arora FGLS

Implementation: Swamy-Arora variant (the Stata default). Two-step FGLS —
first estimate $\sigma_u^2$, $\sigma_e^2$ from the within and between
residuals; then quasi-demean using
$\theta = 1 - \sigma_e / \sqrt{T \sigma_u^2 + \sigma_e^2}$.

## Between effects

    xtreg y x1 x2, be                              // OLS on panel means

Collapses the data to one observation per panel (the panel means of $y$
and each $x$), then runs OLS. This is the BE estimator that underpins
one of the components of the RE-FE decomposition.

## Hausman test

    quietly xtreg y x1 x2, fe                      // populates fe_est
    quietly xtreg y x1 x2, re                      // populates re_est
    hausman                                         // tests FE vs RE

Order of `xtreg fe` and `xtreg re` doesn’t matter; `tea` keeps both in
dedicated workspace slots and the `hausman` command (no arguments
needed) tests their coefficient difference.

# Maximum likelihood estimators

`tea` implements logit, probit, and Poisson regression via a shared
Newton-Raphson driver. All three support `if`, `in`, weights, and
`vce(robust|cluster)`.

## `logit`

    logit voted income education
    logit y x1 x2 i.region, vce(cluster country)

Reports the maximum likelihood iteration trace, LR $\chi^2$ test against
the constant-only model, pseudo $R^2$, and the coefficient table with
$z$-statistics.

## `probit`

Same syntax, normal CDF instead of logistic CDF:

    probit y x1 x2 i.region

## `poisson`

For count outcomes:

    poisson accidents speed_limit population, vce(cluster state)

The link is $\log E[y \mid x] = x'\beta$; coefficients are
log-rate-ratios. $y < 0$ is rejected. Non-integer $y$ is permitted
(treated as the GLM extension).

**Important:** `tea`’s Poisson drops the $\log(y!)$ constant in the
log-likelihood, so the absolute `e(ll)` value differs from Stata’s, but
the coefficient estimates, SEs, and likelihood-ratio tests are
identical.

## Marginal effects via `margins`

After any of the above, `margins` produces average or at-the-mean
marginal effects:

    logit y x1 x2 i.region
    margins, dydx(*)                              // average marginal effects
    margins, dydx(x1)                             // just x1
    margins, dydx(*) atmeans                      // at-the-means MEs

Standard errors use the delta method.

# Instrumental variables

`ivregress 2sls` implements two-stage least squares with a first-stage
$F$ diagnostic for weak instruments.

## Syntax

    ivregress 2sls y [exog_vars] (endog_vars = instruments) [if] [in] [weight] [, options]

The parenthesized group is the key piece: variables to the left of the
`=` are endogenous; variables to the right are excluded instruments.

## Example

    * Wage equation: education is endogenous, parents' education is the IV
    ivregress 2sls wage exper exper2 (educ = momeduc dadeduc), vce(robust)

Output includes the coefficient table (with $z$-statistics from
asymptotic normality), the list of instrumented variables, the list of
instruments (included exogenous + excluded), and the first-stage
$F$-statistic for the weakest endogenous regressor.

## Interpreting the first-stage $F$

A common rule of thumb (Stock-Yogo): if the first-stage $F$ is below 10,
the instrument is weak and the IV estimates may have substantial
finite-sample bias. `tea` flags this in the output with a caret.

## Variance estimators

    ivregress 2sls y (x = z)                              // classical
    ivregress 2sls y (x = z), vce(robust)                 // HC1
    ivregress 2sls y (x = z), vce(cluster country)        // CR1

The sandwich form is $V = A \cdot B \cdot A$ where
$A = (\hat X'\hat X)^{-1}$ (using fitted endogenous columns) and $B$
uses residuals computed from the *original* $X$ (this is the correct
2SLS variance — not the naive OLS-on-fitted variance).

# Time series models

## ARIMA

    tsset year
    arima y, arima(p d q) [noconstant]
    arima y x1 x2, arima(1 0 1)                   // ARMAX with two exog

Specifies an ARIMA$(p, d, q)$ model: AR order $p$, difference order $d$,
MA order $q$. Exogenous regressors can be included (then the model is
ARMAX).

**Important.** v1.0’s `arima` uses *conditional likelihood*, not the
Kalman-filter exact ML that Stata uses by default. The differences:

- Coefficient estimates are consistent and converge to the same limit,
  but finite-sample values can differ by a few percent.

- The absolute log-likelihood differs (we drop the initial-state
  contribution).

- Standard errors come from a numerical-difference Hessian and can be
  off by 1–5% on the MA components specifically. Point estimates are
  unaffected.

If you need exact ML, escape to R’s `arima()` or Python’s
`statsmodels.tsa.arima`.

## Example

    * US growth ARIMA(1,0,0)
    tsset year
    arima growth, arima(1 0 0)

The output groups coefficients into “ARMA model” (constant + exog), “AR”
($\phi_i$), “MA” ($\theta_j$), and `/sigma` (the innovation standard
deviation).

# Post-estimation

## `predict`

After any estimator, `predict` generates a new variable with the model
prediction or a residual:

    * After regress
    regress y x1 x2
    predict yhat                          // linear prediction (default: xb)
    predict resid, residuals              // residuals
    predict resid, r                       // same, shorter

    * After logit/probit
    logit voted income
    predict p                              // Pr(y=1)
    predict xb, xb                         // linear index

    * After xtreg fe
    xtreg y x1, fe
    predict u, u                           // estimated u_i
    predict e, e                           // idiosyncratic residual
    predict ue, ue                         // u_i + e_it

## Hypothesis tests

`test` performs Wald tests on linear restrictions:

    regress y x1 x2 x3
    test x1 = 0                                    // single restriction
    test x1 = 1, x2 = 0                            // joint restriction
    test x1 x2 x3                                  // x1 = x2 = x3 = 0

`lincom` computes linear combinations with their SEs and 95% CIs:

    lincom x1 + x2 - x3
    lincom 0.5*x1 + 0.5*x2

# Output and reproducibility

## Saving estimates

The `estimates` (alias `est`) command suite saves and manages named
estimation results:

    regress y x1
    estimates store m1

    regress y x1 x2
    estimates store m2

    regress y x1 x2 x3
    estimates store m3

    estimates dir                          // list stored estimates
    estimates table m1 m2 m3, stats(N r2 rmse) star
    estimates restore m1                   // load m1 back to last_est
    estimates drop m2                      // remove m2
    estimates drop _all                    // remove all

## `estout`: publication tables

For papers, use `estout` to produce LaTeX, markdown, or plain
side-by-side tables:

    estout m1 m2 m3                                         // default: LaTeX
    estout m1 m2 m3, format(latex) stats(N r2 rmse)
    estout m1 m2 m3, format(markdown) se star
    estout m1 m2 m3, format(plain) t

    estout m1 m2 m3 using "table.tex", stats(N r2)         // write to file
    estout m1 m2 m3, title("Regression results") label(tab:main)

    * Subset of coefficients
    estout m1 m2, keep(x1 x2 _cons) stats(N r2)
    estout m1 m2, drop(_cons)                              // drop constant row

Options:

<div class="center">

| Option                         | Effect                                        |
|:-------------------------------|:----------------------------------------------|
| `format(latex|markdown|plain)` | output format (default: `latex`)              |
| `se`, `t`, `p`                 | show SEs / $t$-stats / $p$-values below coefs |
| `star`                         | add significance stars (10%, 5%, 1%)          |
| `stats(N r2 r2_a rmse F …)`    | footer rows                                   |
| `title(...)`, `label(...)`     | LaTeX caption + reference label               |
| `keep(...)`, `drop(...)`       | restrict coefficient rows                     |
| `nomtitles`, `mtitles(...)`    | override per-model column labels              |
| `using FILE`                   | write to file instead of stdout               |

</div>

LaTeX output produces a stand-alone `tabular` block you can `\input` in
your paper.

## `log`

    log using "session.log", replace
    * commands and their output are captured...
    log close

Captures both the commands you ran (echoed with a `. ` prefix) and the
output they produced.

# Random numbers and Monte Carlo

## Reproducibility

    set seed 12345                                  // fix the PRNG state

`tea` uses a 64-bit PCG generator. `set seed N` is deterministic — the
same seed always produces the same sequence, across runs, machines, and
`tea` versions.

## Functions

    generate u = runiform()                         // Uniform[0, 1)
    generate z = rnormal()                          // Standard normal
    generate w = rnormal(5, 2)                      // Normal(mean=5, sd=2)

Normal draws use Box-Muller with caching. All three functions are
available in any expression context (`generate`, `replace`,
`egen rowfirst/rowlast`, etc.).

## A Monte Carlo example

    * OLS slope sampling distribution under known DGP
    set seed 42
    clear
    set obs 1000

    * True model: y = 2*x + eps,  eps ~ N(0, 1)
    generate x = rnormal()
    generate eps = rnormal()
    generate y = 2*x + eps

    regress y x
    display "Estimated beta = " _b[x] " (true is 2.0)"

# Graphics

Three commands cover the everyday cases, rendered by `tea`'s own
dependency-free SVG engine:

    scatter y x   [if] [in] [, title() xtitle() ytitle() saving(FILE) noview]
    line    y x   [if] [in] [, sort  ...same options...]
    histogram v   [if] [in] [, bins(#) freq ...same options...]

Output is a vector SVG — publication quality by default, and identical
byte-for-byte across platforms (plots are covered by golden-file
regression tests).  In the interactive REPL the plot opens in your OS
viewer; `saving(FILE)` writes to a named file instead, and `noview`
suppresses the viewer.  In do-files plots are only ever written to
files, never opened, so batch output stays deterministic.

`line` connects points in data order; add `sort` to order by x.
`histogram` shows density by default (`freq` for counts); automatic
binning is `min(ceil(sqrt(N)), 50)`.  In the browser edition plots
appear in the panel beside the terminal.

A worked example on bundled data:

    sysuse grunfeld
    scatter invest value, title("Investment vs firm value") ///
        xtitle("Market value") ytitle("Gross investment") saving(grun.svg)

SVG drops directly into LaTeX documents (via the `svg` package, or one
`rsvg-convert` step to PDF).

# Bundled practice datasets (`sysuse`)

Six datasets are embedded inside the binary itself — no files to
install, no network, identical in the browser edition:

    . sysuse dir
      grunfeld  Grunfeld (1958) investment panel: 10 US firms x 1935-1954 (xtreg, hausman)
      airline   Box-Jenkins airline passengers, monthly 1949-1960 (tsset, arima)
      longley   Longley (1967) US macro, 16 obs: famously ill-conditioned (regress)
      nmes1988  Deb-Trivedi (1997) medical care demand, 4406 persons 66+ (poisson, logit)
      pwt       Penn World Table 10.0 sample: 22 countries x 1950-2019, CC BY 4.0 (growth)
      weo       IMF World Economic Outlook, April 2026: 197 countries + 13 aggregates, 145 indicators

Load one with `sysuse NAME[, clear]` — the same unsaved-data guard as
`use` applies.

## The WEO database

`weo` is the complete published IMF World Economic Outlook database,
April 2026 vintage, as a long panel: `country iso year aggregate` plus
one variable per WEO subject code, lowercased.  The codes are the IMF's
own unambiguous names — `ngdp_rpch` is real GDP growth, `pcpipch` CPI
inflation (period average), `lur` the unemployment rate, `bca_ngdpd`
the current account in percent of GDP, `ggxwdg_ngdp` general government
gross debt in percent of GDP, and so on.  The full code → descriptor →
units table ships as `data/weo_codes.txt`.

World and regional groups (World, Advanced Economies, G7, Euro Area,
EU, Emerging Market and Developing Economies, regional aggregates)
carry `aggregate==1`; countries carry `aggregate==0`:

    sysuse weo
    keep if aggregate==0            // the 197-country panel
    xtset iso year
    xtreg ngdp_rpch ggxwdg_ngdp if year<=2025, fe

    sysuse weo, clear
    keep if aggregate==1            // the 13 groups
    line ngdp_rpch year if iso=="G001", sort title("World real GDP growth")

101 of the 145 subject codes are published for groups only and are
empty on country rows — faithful to how the IMF publishes the database.
Years through 2031 are the projections *as published in this vintage*:
the bundled copy is a practice snapshot, not a live data service.  Cite
as *IMF, World Economic Outlook Database, April 2026*.

## The other five

**grunfeld** is the canonical panel-methods teaching dataset; `xtreg
invest value capital, fe` reproduces the textbook estimates (0.110,
0.310).  **airline** is the Box–Jenkins monthly series every ARIMA text
uses.  **longley** is the famously ill-conditioned regression used to
certify least-squares implementations — `regress employed gnpdeflator
gnp unemployed armedforces population year` reproduces the certified
solution.  **nmes1988** is Deb and Trivedi's (1997, *JAE*) medical-care
demand microdata: physician-visit counts, health status, insurance and
income for 4,406 persons 66+ — real material for `poisson` and `logit`.
**pwt** is a Penn World Table 10.0 sample (CC BY 4.0) for growth
regressions.

Provenance, licenses, and citations for all six are in
`data/SOURCES.md`.  To refresh the embedded data (for example a newer
WEO vintage): run `tools/curate_weo.py` on the new download, then
`tools/gen_sysdata.py`, and rebuild — the curator auto-detects both the
classic bulk-file and the data-portal export formats.

# The browser edition

The WebAssembly build at <https://micomrkaic.github.io/tea/> is the
same program compiled for the browser: same commands, same estimators,
same bundled data, same output to the last byte — the full regression
suite runs inside the browser build and passes identically.  Nothing
you load or type leaves your machine; there is no server side.

Practical differences from the native binary:

- **Files** live in an in-browser filesystem.  The *Upload data file*
  button places files where `use` / `import delimited` can see them;
  *Download workspace files* retrieves anything you `save` or `export`.
- **Plots** render in the panel beside the terminal instead of opening
  an OS viewer.
- **`shell`** is unavailable (browsers have no processes) and fails
  with a clear message.
- The terminal offers the same readline-style editing and Tab
  completion as the native REPL.

The first visit downloads about 2 MB (compressed); the browser caches
it afterwards.  The browser edition is a demonstration and practice
channel — for production work, build the native binary.

# Backend-independent output

`tea` promises that **the same do-file prints byte-identical output on
every machine, operating system, CPU, and BLAS library**.  For
well-posed computations this is automatic — results agree to displayed
precision everywhere.  The danger zone is *mathematical zeros*: the
residual sum of squares of a perfect fit, or the standard error of an
exactly-determined coefficient, which different numerical backends
render as different sub-epsilon noise (`5.9e-29` on one machine,
`4.1e-29` on another), occasionally flipping what gets printed
entirely (an SE of `0` versus `1e-16` turns `t = 0.00, p = 1.000` into
`t = 1e+16, p = 0.000`).

`tea` therefore snaps such quantities to exactly zero when they fall
below tight *relative* tolerances: residual SS under 1e-12 of total SS,
and the consequences that follow — residual vectors, standard errors,
coefficients under 1e-8 of the largest in an exact fit, `sigma_u` when
panel intercepts are identical, and the `hausman` difference matrix at
machine precision.  A perfect fit reports `F = inf`, `p = 0.0000`,
deterministically.  **Normal estimation results are untouched to the
last bit** — the thresholds fire only on genuine degeneracy, and a
quantity that is merely small (rather than a mathematical zero) is
left alone by the relative tests.

This is verified, not asserted: the regression suite passes
byte-identically under native gcc + OpenBLAS (release and sanitizer
builds) and under WebAssembly clang + reference BLAS + musl — two
maximally different numerical stacks.  The tradeoff, stated plainly:
raw sub-epsilon values in degenerate fits are not displayed.  The
reasoning is documented in the README's "Semantics decisions" and in
this manual so users of published results can account for it.


# Escape hatches: when to leave `tea`

`tea` is excellent at the data-prep-to-quick-estimate loop, but many
things are out of scope. The escape pattern is:

1.  Do data prep in `tea`.

2.  `export delimited` to CSV.

3.  `shell python myscript.py` (or R, or anything).

4.  `import delimited` the results back.

## When to escape

<div class="center">

| What you need              | Where to go                                     |
|:---------------------------|:------------------------------------------------|
| Plotting                   | `gnuplot`, Python’s `matplotlib`, R’s `ggplot2` |
| GARCH / EGARCH / TGARCH    | Python `arch`, R `rugarch`                      |
| Exact-ML ARIMA (Kalman)    | R’s `arima()`, `forecast::Arima`                |
| Seasonal ARIMA             | R’s `arima()` with `seasonal` arg               |
| Bayesian inference         | PyMC, Stan (`rstan`, `cmdstanpy`)               |
| Mixed-effects models       | R’s `lme4`, Python `statsmodels.MixedLM`        |
| Survival analysis          | R’s `survival`, Python `lifelines`              |
| Cointegration / VAR / VECM | R’s `urca`, `vars`                              |
| Machine learning           | `scikit-learn`, `xgboost`, `lightgbm`           |
| Network / graph models     | `networkx`, R `igraph`                          |
| Spatial econometrics       | R’s `spdep`, Python’s `pysal`                   |

</div>

## A bridge example

    * Estimate panel FE in tea, then use Python for a residuals plot
    xtset country year
    xtreg gdp_growth L.gdp_growth inflation, fe
    predict resid, e

    * Export for plotting
    keep country year resid
    export delimited "resid.csv", replace

    * Hand off to Python (assuming plot_residuals.py exists)
    shell python plot_residuals.py resid.csv resid_plot.png

# Known limitations

These are not bugs in the strict sense — they’re scope decisions for
v1.0 that we may revisit based on testing.

## Data and metadata

- **`.dta` round-trip does not preserve `xtset` state.** After
  `save mydata.dta` then `use mydata.dta`, the panel/time variable
  assignment is lost. Workaround: re-run `xtset country year` after
  `use`. This is the standard Stata-do-file convention anyway.

- **Variable name length cap of 32 characters.** Factor-variable
  interaction names like `2010.year#1985.country#c.gdp` (28 chars) fit;
  longer 3-way interactions truncate.

## Econometric estimators

- **ARIMA uses conditional likelihood, not Kalman-filter exact ML.** See
  the "Time series models" chapter for details. For serious work,
  use R’s `arima()`.

- **No seasonal ARIMA** (`sar`, `sma` terms).

- **ARIMA MA standard errors** have a 1–5% precision loss from the
  numerical-Hessian computation. Point estimates are unaffected.

- **`xtreg, be` uses straight OLS** on panel means. Stata’s default
  applies a slight variance correction for unbalanced panels
  (down-weighting groups with few observations). Differences appear only
  on unbalanced panels and only in the SEs (point estimates are
  identical).

- **No `xtabond` / system GMM** or other GMM estimators.

- **No VAR / VECM.** Escape to R.

## Factor variables

- **No composition with TS operators.** Tokens like `i.country#c.L.gdp`
  are not parsed. Workaround: pre-compute the lag with
  `gen L1_gdp = L.gdp` and use `c.L1_gdp`.

- **`i.var` requires numeric var.** Use `encode` on string identifiers.

- **Cell-count cap of 10 000.** Larger factor expansions are blocked.

## Programming

- **No `program define`, no `syntax` parser, no Mata.** You can write
  loops and use macros, but you cannot define new commands.

## Graphics

`scatter`, `line`, and `histogram` cover the everyday cases (see the
Graphics chapter).  There is no `twoway` grammar, no faceting/`by()`
graphics, no legends or multiple series per plot, and no graph editor;
for figures beyond the basics, `export delimited` and pipe to
Python / R / gnuplot.

# Reporting bugs

If you find a bug:

1.  Distill it into a small reproducer — a do-file plus a small input
    file is ideal. If the input data is sensitive, replace it with
    `set seed N; gen x = rnormal()` or similar synthetic.

2.  Note your `tea --version`, OS, and compiler version.

3.  Send to the author at the email associated with your distribution of
    `tea`. If you don’t have one, ask the person who gave you the
    software.

What counts as a bug:

- Wrong numerical answer (with hand-calculated expected value).

- Crash, segfault, or hang on input that should work.

- Different behavior from documented Stata semantics (when not in the
  “known limitations” list above).

What does not count as a bug:

- Missing feature (those go on the feature-request list).

- Disagreement with Stata’s exact numerical output to many decimal
  places (small differences arise from BLAS vendor variation and are not
  correctness issues).

- Things listed in the "Known limitations" chapter.

# Function reference

Functions available in any expression context (`generate`, `replace`,
`if` clauses, `display`, …). Each entry shows the signature and a
one-line description. In all cases: a missing argument propagates to a
missing result, unless otherwise stated.

## Arithmetic and rounding

<div class="center">

| Signature          | Returns                                        |
|:-------------------|:-----------------------------------------------|
| `abs(x)`           | $|x|$                                          |
| `int(x)`           | truncate toward zero                           |
| `floor(x)`         | largest integer $\le x$                        |
| `ceil(x)`          | smallest integer $\ge x$                       |
| `round(x)`         | round to nearest integer (half away from zero) |
| `round(x, unit)`   | round to nearest multiple of `unit`            |
| `sign(x)`          | $-1$ / $0$ / $+1$                              |
| `mod(x, y)`        | $x \bmod y$, same sign as $x$                  |
| `min(x, y, …)`     | minimum of arguments (missing skipped)         |
| `max(x, y, …)`     | maximum of arguments (missing skipped)         |
| `cond(test, a, b)` | `a` if `test` is true, else `b`                |

</div>

## Exponential and logarithm

<div class="center">

| Signature  | Returns                           |
|:-----------|:----------------------------------|
| `exp(x)`   | $e^x$                             |
| `ln(x)`    | natural log; missing if $x \le 0$ |
| `log(x)`   | alias for `ln`                    |
| `log10(x)` | base-10 log; missing if $x \le 0$ |
| `sqrt(x)`  | $\sqrt{x}$; missing if $x < 0$    |

</div>

## Trigonometric and hyperbolic

<div class="center">

| Signature                       | Returns                      |
|:--------------------------------|:-----------------------------|
| `sin(x)`, `cos(x)`, `tan(x)`    | radians                      |
| `asin(x)`, `acos(x)`            | inverse trig; range checked  |
| `atan(x)`                       | inverse tangent              |
| `atan2(y, x)`                   | two-argument inverse tangent |
| `sinh(x)`, `cosh(x)`, `tanh(x)` | hyperbolic                   |

</div>

## Probability distributions

<div class="center">

| Signature              | Returns                                  |
|:-----------------------|:-----------------------------------------|
| `normal(z)`            | standard-normal CDF $\Phi(z)$            |
| `normalden(z)`         | standard-normal PDF $\phi(z)$            |
| `normalden(z, mu, sd)` | normal PDF with mean and SD              |
| `lnnormal(z)`          | $\log \Phi(z)$, stable in the lower tail |
| `lnnormalden(z)`       | $\log \phi(z)$, stable in extremes       |
| `invnormal(p)`         | inverse CDF $\Phi^{-1}(p)$               |
| `ttail(df, t)`         | upper-tail Student-$t$: $P(T > t)$       |
| `invttail(df, p)`      | inverse: $t$ such that $P(T > t) = p$    |
| `F(df1, df2, F)`       | lower-tail $F$ CDF                       |
| `Ftail(df1, df2, F)`   | upper-tail $F$: $P(F > f)$               |
| `chi2(df, x)`          | lower-tail $\chi^2$ CDF                  |
| `chi2tail(df, x)`      | upper-tail $\chi^2$: $P(X > x)$          |

</div>

## Random number generation

Reproducible from `set seed N`. See
the "Random numbers and Monte Carlo" chapter.

<div class="center">

| Signature         | Returns                           |
|:------------------|:----------------------------------|
| `runiform()`      | uniform $[0, 1)$                  |
| `rnormal()`       | standard normal                   |
| `rnormal(mu)`     | normal with mean `mu`, sd 1       |
| `rnormal(mu, sd)` | normal with mean `mu` and SD `sd` |

</div>

## Missing-value tests and ranges

<div class="center">

| Signature              | Returns                                  |
|:-----------------------|:-----------------------------------------|
| `missing(x)`           | 1 if `x` is any missing code, else 0     |
| `inrange(x, lo, hi)`   | 1 if $\mathtt{lo} \le x \le \mathtt{hi}$ |
| `inlist(x, v1, v2, …)` | 1 if `x` equals any of `v1`, `v2`, …     |

</div>

## String construction and inspection

<div class="center">

| Signature                | Returns                                      |
|:-------------------------|:---------------------------------------------|
| `strlen(s)`, `length(s)` | character count of `s`                       |
| `substr(s, start, len)`  | substring; `start` is 1-based; `-1` for last |
| `strpos(s, sub)`         | 1-based index of `sub` in `s`, 0 if absent   |
| `strrpos(s, sub)`        | like `strpos` but search from the right      |
| `word(s, n)`             | the $n$th whitespace-separated word          |
| `wordcount(s)`           | number of whitespace-separated words         |
| `abbrev(s, n)`           | abbreviate `s` to width `n`                  |
| `char(n)`                | single-character string for ASCII code `n`   |

</div>

## String transformation

<div class="center">

| Signature                   | Returns                                        |
|:----------------------------|:-----------------------------------------------|
| `strupper(s)`, `upper(s)`   | uppercase                                      |
| `strlower(s)`, `lower(s)`   | lowercase                                      |
| `strproper(s)`, `proper(s)` | title case (first letter of each word)         |
| `strtrim(s)`, `trim(s)`     | strip leading and trailing whitespace          |
| `ltrim(s)`, `rtrim(s)`      | strip leading / trailing whitespace            |
| `itrim(s)`                  | collapse internal whitespace runs to one space |
| `strreverse(s)`             | reverse character order                        |
| `subinstr(s, from, to)`     | replace all occurrences of `from` with `to`    |
| `subinstr(s, from, to, n)`  | replace at most `n` occurrences                |
| `subinword(s, from, to)`    | replace whole-word matches only                |
| `strmatch(s, pattern)`      | glob-style match (`*`, `?`); 1 / 0             |

</div>

## Regular expressions (POSIX extended)

<div class="center">

| Signature              | Returns                                                          |
|:-----------------------|:-----------------------------------------------------------------|
| `regexm(s, pat)`       | 1 if `pat` matches anywhere in `s`, else 0; populates `regexs()` |
| `regexs(n)`            | $n$th captured group from last `regexm` (0 = full match)         |
| `regexr(s, pat, repl)` | replace first match in `s`; `repl` may use `\1` … `\9`           |

</div>

## Numeric $\leftrightarrow$ string conversion

<div class="center">

| Signature                           | Returns                                  |
|:------------------------------------|:-----------------------------------------|
| `string(x)`                         | default decimal string                   |
| `string(x, fmt)`                    | format `x` with the given Stata format   |
| `strofreal(x)`, `strofreal(x, fmt)` | alias for `string`                       |
| `real(s)`                           | parse `s` as numeric; missing on failure |

</div>

## Calendar: building dates

<div class="center">

| Signature               | Returns                                                      |
|:------------------------|:-------------------------------------------------------------|
| `mdy(month, day, year)` | daily date (%td) for (month, day, year)                      |
| `ym(year, month)`       | monthly date (%tm)                                           |
| `yq(year, quarter)`     | quarterly date (%tq)                                         |
| `yh(year, half)`        | half-yearly date (%th)                                       |
| `yw(year, week)`        | weekly date (%tw)                                            |
| `date(s, fmt)`          | parse string `s` per `fmt` (`"YMD"`, `"DMY"`, `"MDY"`, etc.) |

</div>

## Calendar: extracting components from a daily date

<div class="center">

| Signature     | Returns                        |
|:--------------|:-------------------------------|
| `year(d)`     | calendar year                  |
| `month(d)`    | month, 1–12                    |
| `day(d)`      | day of month, 1–31             |
| `quarter(d)`  | quarter, 1–4                   |
| `halfyear(d)` | half-year, 1 or 2              |
| `week(d)`     | ISO week number                |
| `dow(d)`      | day of week, 0 (Sun) – 6 (Sat) |
| `doy(d)`      | day of year, 1–366             |

</div>

## Calendar: converting between frequencies

<div class="center">

| Signature                    | Returns                                      |
|:-----------------------------|:---------------------------------------------|
| `mofd(d)`                    | monthly date corresponding to daily date `d` |
| `dofm(m)`                    | first day of month `m` (as daily)            |
| `qofd(d)`                    | quarterly index of daily `d`                 |
| `dofq(q)`                    | first day of quarter `q`                     |
| `hofd(d)`                    | half-year index of daily `d`                 |
| `dofh(h)`                    | first day of half-year `h`                   |
| `wofd(d)`                    | weekly index of daily `d`                    |
| `ty(y)`                      | year-as-yearly-date (just $y$, for display)  |
| `dofy(y)`                    | first day of year `y`                        |
| `tm`, `tq`, `tw`, `th`, `td` | identity converters used for format-tagging  |

</div>

## Special: running and time-series-context

<div class="center">

| Signature            | Returns                                      |
|:---------------------|:---------------------------------------------|
| `sum(x)`             | running cumulative sum (resets per by-group) |
| `_n`                 | current observation index (1-based)          |
| `_N`                 | total observation count in current scope     |
| `x[_n-1]`, `x[_n+1]` | lag / lead within current sort order         |
| `x[1]`, `x[_N]`      | first / last observation of current scope    |

</div>

# egen function reference

Functions usable as `egen newvar = func(...)`; see the `egen` part of the Data-management chapter.

## Group-wise aggregators

When combined with `by(grouplist)`, these compute one value per group
and broadcast that value back to every row in the group. Without `by()`,
they collapse to a single scalar broadcast everywhere.

<div class="center">

| Signature                | Returns                                           |
|:-------------------------|:--------------------------------------------------|
| `mean(exp)`              | arithmetic mean                                   |
| `sum(exp)`, `total(exp)` | sum (missing values skipped)                      |
| `count(exp)`             | number of non-missing observations                |
| `min(exp)`, `max(exp)`   | extrema                                           |
| `sd(exp)`                | sample standard deviation, $n-1$ denominator      |
| `var(exp)`               | sample variance                                   |
| `median(exp)`            | 50th percentile (linear interpolation)            |
| `iqr(exp)`               | interquartile range, 75th $-$ 25th                |
| `pc(exp)`                | percentile; option `p(N)` sets which (default 50) |

</div>

## Position and identity

<div class="center">

| Signature        | Returns                                                 |
|:-----------------|:--------------------------------------------------------|
| `rank(exp)`      | 1-based rank within group; ties handled by mean rank    |
| `group(varlist)` | 1…K integer ID per unique combination of `varlist`      |
| `tag(varlist)`   | 1 on the first row of each `varlist` group, 0 otherwise |

</div>

## Row-wise aggregators (across columns of a single row)

These ignore `by()` (the row is the unit).

<div class="center">

| Signature             | Returns                             |
|:----------------------|:------------------------------------|
| `rowtotal(varlist)`   | sum across columns; missing skipped |
| `rowmean(varlist)`    | mean across columns                 |
| `rowmin(varlist)`     | minimum across columns              |
| `rowmax(varlist)`     | maximum across columns              |
| `rowsd(varlist)`      | sample SD across columns            |
| `rowmiss(varlist)`    | count of missing cells in the row   |
| `rownonmiss(varlist)` | count of non-missing cells          |

</div>

# Summarize / tabstat statistics keywords

Available with `tabstat varlist, statistics(...)`, and many also as
percentile selectors after `summarize, detail`.

<div class="center">

| Keyword                         | Meaning                            |
|:--------------------------------|:-----------------------------------|
| `n`                             | non-missing count                  |
| `mean`                          | arithmetic mean                    |
| `sum`                           | total                              |
| `sd`                            | standard deviation (sample, $n-1$) |
| `variance`                      | variance                           |
| `min`, `max`                    | extrema                            |
| `range`                         | `max - min`                        |
| `median`                        | 50th percentile                    |
| `p1`, `p5`, `p10`, `p25`, `p50` | percentiles                        |
| `p75`, `p90`, `p95`, `p99`      | percentiles                        |
| `iqr`                           | 75th $-$ 25th percentile           |
| `skewness`, `kurtosis`          | after `summarize, detail`          |

</div>

# Stata cheat sheet

A quick reference for Stata users adapting to `tea`.

## Same as Stata

- Syntax: `[prefix:] command [varlist] [if] [in] [weight] [, options]`

- Missing-value algebra, including `if x > 5` including missing

- Macros: `` `local' `` and `$global`, `r()` and `e()` returns

- `by:`, `bysort:`, the `[_n-1]` subscript, `_N`

- Value labels: `label define / values / list`

- Factor variables (`i.`, `c.`, `ib<n>.`, `#`, `##`)

- Time-series operators (`L. F. D. S.`)

## Tea-specific differences

- `m:m` merge is rejected with an explanatory message.

- `xtreg, be` uses simple OLS on panel means.

- `arima` uses conditional likelihood.

- `i.country#c.L.gdp` doesn’t parse; pre-compute the lag.

- No `.gph` graphics — use `export` and external tools.

- `program define`, `syntax`, and Mata are absent.

## Stata commands that don’t exist in tea

- Graphics: `graph`, `twoway`, `histogram`, etc.

- GMM: `gmm`, `xtabond`, `xtdpd`.

- Time series: `var`, `vec`, `dfgls`, `dfuller`, `xcorr`, `wntestq`.

- Survival: `stcox`, `streg`, `stset`.

- Mixed: `mixed`, `xtmixed`, `meqrlogit`.

- Bayesian: `bayes:`, `bayesmh`.

- Multivariate: `mvreg`, `factor`, `pca`.

- Multiple imputation: `mi`.

For each of these, see the "Escape hatches" chapter for the recommended external tool.

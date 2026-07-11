# tea — roadmap

## Done

- **v0.1** — value/missing algebra, columnar store, multiple frames, lexer,
  expression parser, evaluator, command layer (`if`/`in`/`by:`/varlist
  sugar/options), do-file driver, macros, control flow, calendar suite,
  core commands (generate/replace/egen/list/summarize/count/describe/
  drop/keep/rename/order/label/format/sort/gsort/tabulate/tsset/xtset/
  collapse/frames/import/export/save/use/clear).
- **v0.1.5** — `merge` (1:1, m:1, 1:m, no m:m), `reshape` long/wide
  in-place.
- **v0.1.8** — value labels, `recode`, `codebook`, weights
  (`[fw=]/[aw=]/[pw=]/[iw=]`), `t*()` literal-date constructors,
  `frame copy/rename/put/drop`.
- **v0.2** — readline REPL with history + tab completion, `help`,
  `version`, `pwd`, `cd`, `log using/close`, `exit/quit`, GPLv3 license,
  rename to `tea`.
- **v0.3** — shell escape (`! cmd` / `shell cmd`), `assert exp`,
  `#delimit ;`, `import excel`/`import ods` via ssconvert-or-libreoffice
  shellout, context-aware tab completion (commands after `help`,
  filenames in file positions), line-annotated errors in do-file mode.
- **v0.4** — OLS via OpenBLAS + LAPACKE; `regress` with classical / HC1
  robust / CR1 clustered SEs; `predict {xb, residuals}`; `test` (joint
  Wald F); `lincom` (linear combinations).  Workspace-level `Estimates`
  struct so all future estimators feed the same postestimation
  machinery.  GSL for p-values and inverse-CDFs.
- **v0.4.1** — cross-platform Makefile (auto-detects macOS Homebrew
  prefixes); fixed dead-`acc` warning in egen.
- **v0.5** — bug-fix release after first round of real-data testing:
  log no longer hangs REPL; locals strip surrounding quotes (fixed
  foreach with quoted lists); bysort:summarize honors by:; describe
  varlist filters; encode/decode round-trip; mkdir/rmdir/erase/copy/
  do FILENAME; macOS soffice auto-detection; line-annotated aborts.
- **v0.5.2** — round 2 of real-data fixes: list column widths
  max-of-cells (Stata-faithful); `?` and arbitrary wildcards via
  fnmatch; dir/ls command; use/import require `,clear`; trig +
  distribution math functions (sin/cos/tan/normal/ttail/Ftail/chi2tail);
  log captures both commands and output (Stata-faithful); reshape long
  stub spec correctly handles wildcards in stub names.
- **v0.5.3** — reshape segfault hotfix: validate type consistency across
  stub levels; detect i()/stub overlap.
- **v0.5.5** — fixed misleading reshape error suggestion.
- **v0.5.6** — destring/tostring: convert string variables to numeric
  and vice versa, with `,replace` or `,generate(stub)`, `,force` to
  convert non-parseable values to missing, `,ignore("chars")` to strip
  characters before parsing, `,format(%fmt)` for tostring output format.

## v0.6 — econometric expansion

### Compatibility contract (locked, primary design rule)

Tea is a **proper subset of Stata**.  Every tea do-file must run unchanged
in Stata.  Every tea command, option, and function — if Stata has it,
must behave identically.  If Stata doesn't have it, must not exist, or
must be opt-in via `--tea-extensions`.

**Operational rules:**

1. `--strict-stata` is the **default** runtime mode.  Anything Stata
   wouldn't accept is rejected.
2. `--tea-extensions` is an opt-in flag that unlocks tea-only features.
   It currently unlocks nothing because there are no extensions yet;
   the flag exists so future deviations are gated explicitly.
3. Improvements to Stata's behavior are NOT acceptable if they break
   compatibility.  The project's pitch is "free Stata-compatible tool"
   — improvements that break that kill the value proposition.
4. When in doubt about a behavior, default to whatever Stata does.

### v0.6 work items (in order)

1. **`.dta` reader and writer.**  Target Stata 14+ format ("118");
   `save FILE.dta` writes it, `use FILE.dta` reads it.  Older `.tea`
   format stays as tea-native option but is deprecated.  ~400-500 lines.

2. **`set` subcommand audit.**  Implement the standard Stata list
   (more, seed, linesize, matsize, obs, level, varabbrev, rmsg, ...);
   no-op the display-only ones (more, linesize, matsize) because tea's
   REPL doesn't have pagination, line-width formatting, or matsize
   limits; ERROR on truly unrecognized subcommands (matching Stata).
   The current "silently accept everything" behavior is wrong.

3. **Stata math functions** in the expression language: `ttail(df,t)`,
   `Ftail(df1,df2,F)`, `chi2tail(df,x)`, `invttail()`, `normal()`,
   `invnormal()`, `chi2()`, `invchi2()`.  All route to existing
   `stats.c` helpers (GSL-backed) plus a few additions.  ~50 lines.

4. **`--strict-stata` default + `--tea-extensions` flag.**  Infrastructure
   for the contract; no behavior changes today.

5. **`COMPATIBILITY.md`** at repo root codifying the contract.

### v0.6 estimation work (after the compatibility batch)

- **`xtreg, fe`** — fixed-effects panel via within-transformation.
  Reuses the OLS solver after demeaning by panel unit.
- **`xtreg, re`** — random-effects via GLS.
- **`probit` / `logit`** — MLE via Newton-Raphson on the score.
- **`ivregress` / `ivreg2`** — 2SLS, weak-instrument diagnostics.
- **`margins`** — average and at-means marginal effects.

## v0.7 — quality of life

- **`tabulate v1 v2`** — two-way frequency table with row/column
  percentages.
- **`egen` rank/pctile/cut family** — same shape as existing egen handlers.
- **`merge … assert(...)` as hard error** (currently warns).
- **`graph`** — shell out to gnuplot for `scatter`/`line`/`hist`.  Optional
  but cheap.
- **`reshape` with string `j()`**.
- **Survey weights** for `summarize`/`regress` beyond the basic form.

## Deliberately deferred

- **`program define`** / ado files.  May never be needed.
- **`merge m:m`**.  Excluded by design — it's a footgun; users should
  `reshape` or restructure instead.

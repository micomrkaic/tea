# decaf tea — the data-management edition

decaf tea is a build tier of tea, not a fork: one source tree, one
test suite, one history, two binaries.  It contains the data plumbing
that makes up most real Stata usage — `import`, `merge`, `reshape`,
`collapse`, `sort`, `egen`, `encode`/`destring`, frames,
`preserve`/`restore`, `save`/`use` (.dta round-trip), exploration
(`summarize`, `tabulate`, `tabstat`, `correlate`, `codebook`), and the
SVG graphics — with the entire estimation tier compiled out.

decaf computes no statistic you would publish.  There is no `regress`,
no `xtreg`, no `arima`, no `ttest`, no post-estimation.  Data prepared
in decaf saves to Stata-compatible `.dta` for estimation in any full
package.  decaf tea feeds Stata; it does not replace it.

## Why

- **Where the usage is.** Command censuses of real research code show
  the overwhelming bulk of lines are data preparation, not estimation.
- **Auditable correctness.** A wrangling error is loud — merge counts,
  `isid`, byte-comparison of `.dta` output.  Shipping only the
  verifiable half is the right risk posture for institutional use.
- **Zero numerical surface.** The decaf build links no LAPACK and no
  BLAS.  The browser build (`make wasm-decaf`) is a data tool with no
  install and no linear-algebra stack at all.

## Building

    make both             # both tiers in one command: ./tea and ./tea-decaf
    make decaf            # native binary: ./tea-decaf
    make decaf-test       # leak check + tier-filtered regression suite
    make wasm-decaf       # browser build into web/decaf/
    make wasm-decaf-test  # node smoke: plumbing golden + absence proof
    make gate             # both native suites + the decaf leak check

The tier-aware test harness asks the binary what it can run
(`help _names`) and filters the suite accordingly, so decaf's coverage
can never drift from its build: adding a command to decaf
automatically admits its tests.

Every release gate runs both tiers.  The plumbing integration test
(tests/regression/80_decaf_plumbing.do) is asserted byte-identical
between `tea` and `tea-decaf`.

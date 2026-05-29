# Real-data tests for tea

These tests exercise tea against Stata's own `auto.dta` and a synthetic
value-labeled survey dataset, and let you cross-check tea's outputs
against Stata's directly.

## What gets tested

- **`auto_weights.do`** — all four weight types (fweight, aweight,
  pweight, iweight) in `regress`, `summarize`, `count`, `tabulate`
  on Stata's auto.dta.  Also tests `, robust`, `, cluster()`, the
  aweight+robust ≡ pweight identity, and the validation that each
  command rejects the right weight types.

- **`dta_polish.do`** — round-trips value labels, the dataset label,
  variable labels, and exercises `save ... , version(NUM)`.


## One-time setup (Stata only)

```
cd tests/realdata
stata -b do prep_in_stata.do
```

This produces two fixtures in this directory:

- `auto.dta` — Stata's bundled example, saved here so tea can load it
- `survey.dta` — a synthetic 200-row survey with value labels,
  variable labels, dataset label, and a sampling weight column


## Running the tests

Once the fixtures exist, each test file is run in BOTH tea and Stata:

```
tea auto_weights.do > out_tea.txt 2>&1
stata -b do auto_weights.do
# produces auto_weights.log

sh compare.sh out_tea.txt auto_weights.log
```

`compare.sh` normalizes whitespace and shows side-by-side differences.

Same workflow for `dta_polish.do`.


## What to look for

The tests are organized into numbered, captioned blocks.  When diffing,
expect:

- **Coefficients and SEs** to match byte-for-byte (or at least to ~10
  significant digits).
- **R-squared, F-statistic, df** to match.
- **Number of obs** to match (for fweight: the sum of weights; for
  the others: unweighted count).
- **Column alignment, header padding, log timestamps** to differ —
  these are formatting, not substance.

A real discrepancy in a coefficient or SE is a bug.  Report it with the
output diff and we'll dig in.


## Cross-checking that tea-saved .dta files open in Stata

After running `dta_polish.do` in tea, the files `survey_roundtrip.dta`
and `survey_v117.dta` should be left in this directory (the test cleans
them up, so re-run with the cleanup line commented out to keep them).
Open them in Stata:

```
. use survey_roundtrip.dta
. describe       // should show data label + variable labels
. label list     // should show sexlbl, reglbl, edulbl
. list in 1/3    // should show decoded labels
```

If Stata refuses to open a tea-saved file, that's a critical bug —
report immediately.


## Files

```
prep_in_stata.do    Stata-only fixture builder
auto_weights.do     Weight-type tests on auto.dta (runs in tea + Stata)
dta_polish.do       .dta features tests on survey.dta (runs in tea + Stata)
compare.sh          Normalizes whitespace and side-by-side diffs two outputs
README.md           This file
```

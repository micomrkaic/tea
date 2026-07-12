# Command reference

*Generated from the tea binary by `tools/gen_cmdref.sh` — this text is
exactly what `help CMD` prints at the prompt, and regenerating it after
any change keeps the manual and the implementation in agreement.*

## `twoway`

```
  twoway (TYPE y x [if], opts) ... [, gopts]  overlay plot; TYPE: scatter line connected lowess
      per-series: lcolor() lpattern() msymbol(i) mlabel(var) mlabposition(#) bwidth() mean adjust
      global: title() xtitle() ytitle() note() legend(off) yline(#,..) yscale(range()) ylabel(a(s)b) name(N[,replace]) saving()
```

## `graph`

```
  graph box y [if], over(v[,sub]) [over(v2)] [noout] ...   grouped box plots
      graph combine N1 N2 ... [, cols(#) title() note() name()]   compose named graphs
      graph dir | graph drop NAME|_all                            registry
```

## `scatter`

```
  scatter yvar xvar [if] [in] [, title() xtitle() ytitle() saving() noview]
      SVG scatter plot; saving(FILE) writes to FILE instead of tea_graph.svg
      e.g.  scatter gdp_growth inflation if year>2000, title("Growth vs inflation")
```

## `line`

```
  line yvar xvar [if] [in] [, sort title() xtitle() ytitle() saving() noview]
      SVG line plot; connects points in data order (use sort to order by x)
      e.g.  line gdp year if country=="US", sort
```

## `histogram`

```
  histogram var [if] [in] [, bins(#) freq title() saving() noview]
      SVG histogram; density by default, freq for counts, auto bins = min(ceil(sqrt(N)),50)
      e.g.  histogram wage, bins(30) freq
```

## `generate`

```
  generate [type] newvar = exp [if] [in]      create a new variable
      e.g.  gen logy = log(income) if year >= 2000
```

## `replace`

```
  replace var = exp [if] [in]                  modify existing variable
      e.g.  replace income = . if income < 0
```

## `egen`

```
  egen newvar = fn(args) [, by(varlist)]       extended generate (mean/sum/group/...)
      e.g.  egen meanY = mean(y), by(country)
```

## `list`

```
  list [varlist] [if] [in]                     print observations
      e.g.  list country year gdp if year>2020 in 1/10
```

## `summarize`

```
  summarize [varlist] [if] [in] [weight] [, detail]      N/mean/sd/min/max; sets r()
      e.g.  summarize gdp_growth, detail
```

## `tabstat`

```
  tabstat varlist [if] [in], statistics(stat ...) [by(var)] [columns(stat|var)]
      stats: mean sd var cv min max range sum count median p1-p99 iqr
      e.g.  tabstat gdp pop, stats(mean sd p25 p50 p75) by(region)
```

## `count`

```
  count [if] [in] [fw=]                        count matching observations
```

## `describe`

```
  describe [varlist]                           show variables, types, formats
```

## `drop`

```
  drop varlist | if exp | in range             remove vars or observations
      e.g.  drop if gdp == .   |   drop tempvar1 tempvar2
```

## `keep`

```
  keep varlist | if exp | in range             complement of drop
      e.g.  keep country year gdp
```

## `rename`

```
  rename oldname newname                       rename a variable
      e.g.  rename _C* C*   (wildcard rename: strip leading underscore)
```

## `order`

```
  order varlist                                reorder variables (listed ones go first)
      e.g.  order country year
```

## `aorder`

```
  aorder [varlist]                             alphabetize variable order
```

## `label`

```
  label variable v "text"                      attach a description to a variable
    label define lblname # "text" # "text"   define a value-label set
    label values varlist lblname             attach value labels to variables
```

## `format`

```
  format varlist %fmt                          set display format
      e.g.  format date %td   |   format gdp %12.2f
```

## `sort`

```
  sort varlist                                 ascending stable sort
      e.g.  sort country year
```

## `gsort`

```
  gsort [+-]var [+-]var ...                    sort with per-key direction
      e.g.  gsort -gdp +country  (gdp desc, then country asc)
```

## `tabulate`

```
  tabulate var [if] [in] [weight]              one-way frequency table
      e.g.  tabulate region if year == 2020
```

## `tsset`

```
  tsset timevar [, format(%fmt)]               declare time series
      e.g.  tsset year
```

## `xtset`

```
  xtset panelvar timevar                       declare panel (gap-aware L./F./D./S.)
      e.g.  xtset country year
```

## `xtdescribe`

```
  xtdescribe                                   summarize panel structure
      e.g.  xtdescribe   (requires xtset first; shows n, T, balance)
```

## `xtreg`

```
  xtreg y x1 x2 ... [if] [in], fe [vce(robust|cluster v)]   panel FE OLS
      e.g.  xtreg growth L.growth pop, fe
            (requires xtset; within estimator; reports R-w/b/o, sigma_u/e, rho)
```

## `hausman`

```
  hausman                                       FE vs RE specification test
      e.g.  xtreg y x, fe
            xtreg y x, re
            hausman   (Ho: cov(u_i, x) = 0; rejection means use FE)
```

## `logit`

```
  logit y x1 x2 ... [if] [in] [weight], [vce(robust|cluster v)]   logistic regression
      e.g.  logit voted income education, vce(cluster county)
```

## `probit`

```
  probit y x1 x2 ... [if] [in] [weight], [vce(robust|cluster v)]   probit regression
      e.g.  probit voted income education, vce(robust)
```

## `ivregress`

```
  ivregress 2sls y [exog] (endog = instruments) [if] [in] [weight] [, vce(robust|cluster v)]
      Two-stage least squares with first-stage F diagnostic.
      e.g.  ivregress 2sls wage educ (exper = age momeduc daddyEduc)
```

## `poisson`

```
  poisson y x1 x2 ... [if] [in] [weight] [, vce(robust|cluster v)]   poisson regression
      e.g.  poisson accidents speed_limit population, vce(cluster state)
```

## `arima`

```
  arima y [exog] [if] [in], arima(p d q) [noconstant]   ARIMA(p,d,q) via conditional ML
      e.g.  arima gdp, arima(1 1 1)              ARIMA(1,1,1) on gdp
            arima cpi unemp, arima(2 0 1)        ARMAX(2,1) with exog regressor
```

## `margins`

```
  margins , dydx(*|varlist) [atmeans]            average marginal effects
      e.g.  logit y x1 x2
            margins, dydx(*)              AME for all regressors
            margins, dydx(x1) atmeans     MEM for x1 at sample means
```

## `estimates`

```
  estimates store|restore|dir|drop|table          named estimates ledger
      e.g.  regress y x1 x2
            estimates store m1
            regress y x1 x2 x3
            estimates store m2
            estimates table m1 m2, se star  stats(N r2 rmse)
```

## `est`

```
  est = estimates  (short form)                   see `help estimates`
```

## `estout`

```
  estout [names] [, format(latex|markdown|plain) se|t|p stars stats(...)]
      Side-by-side LaTeX/markdown/plain table of stored estimates.
      e.g.  estout m1 m2, stats(N r2 rmse) using table.tex
```

## `collapse`

```
  collapse (stat) v1 v2 ... , by(g) [weight]   aggregate to groups
      e.g.  collapse (mean) gdp pop, by(region year)
```

## `merge`

```
  merge 1:1|m:1|1:m keyvars using FILE [, ...] join master with using file
      e.g.  merge 1:1 country year using gdp.tea   (m:m is rejected by design)
```

## `reshape`

```
  reshape long|wide stubs , i(idvars) j(jvar)  pivot wide<->long (in-place)
      e.g.  reshape long v, i(country) j(year)
            takes v1980 v1981 ... v2031 -> rows of (country, year, v)
```

## `recode`

```
  recode varlist (rules) [, gen(stub)]         map values
      e.g.  recode score (1/3=1)(4/6=2)(7/10=3) (missing=.), gen(score_g)
```

## `encode`

```
  encode strvar, generate(newvar) [label(name)]  map strings to integers + value label
```

## `decode`

```
  decode intvar, generate(newvar)              inverse of encode (codes back to strings)
```

## `destring`

```
  destring varlist, {replace|generate(stub)} [force] [ignore("chars")]   str -> num
      e.g.  destring z4, replace force   (junk values become missing)
```

## `tostring`

```
  tostring varlist, {replace|generate(stub)} [force] [format(%fmt)]      num -> str
```

## `codebook`

```
  codebook [varlist]                           type, uniques, missing, range
```

## `import`

```
  import delimited|excel|ods using FILE [, sheet(name) clear]   read CSV/TSV/XLSX/ODS
      e.g.  import delimited "data/WEO.csv", clear
```

## `export`

```
  export delimited using FILE                  write CSV/TSV
      e.g.  export delimited using out.csv
```

## `save`

```
  save FILE [, replace]                        write Stata .dta (default) or .tea
      e.g.  save mydata.dta, replace        — emits Stata-compatible .dta
      e.g.  save mydata.tea, replace        — native tea binary
```

## `outreg2`

```
  outreg2 using FILE [, replace|append ctitle() dec() bdec() se label
      symbol() alpha() addstat("Name", expr, ...) addtext() addnote()]
      regression-table exporter (tab-separated; opens in Excel)
```

## `which`

```
  which CMD  — report whether CMD is a built-in tea command
```

## `ssc`

```
  ssc install PKG  — accepted and skipped (no package system)
```

## `isid`

```
  isid varlist  — error 459 unless varlist uniquely identifies obs
```

## `duplicates`

```
  duplicates report|drop [varlist]
```

## `tempfile`

```
  tempfile NAME...  — set local macros to fresh temp-file paths
```

## `tempname`

```
  tempname NAME...  — set local macros to fresh scratch names
```

## `pwcorr`

```
  pwcorr varlist  — pairwise correlation matrix
```

## `file`

```
  file open H using F, write [replace|append] | file write H "..." _n | file close H
```

## `confirm`

```
  confirm [new] file FILENAME | confirm [new|numeric|string] variable NAME
      error (601/602/111/110/7) unless the condition holds; use with
      capture:  capture confirm file f.dta
                if _rc { <create it> }
```

## `history`

```
  history [N] | history save FILE [, replace] | history clear
      list, export, or clear this session's interactive commands
      (native REPL arrow-key history persists in ~/.tea_history;
       the browser edition persists history in the browser)
```

## `sysuse`

```
  sysuse dir | sysuse NAME [, clear]
      load a practice dataset bundled inside the tea binary
      e.g.  sysuse grunfeld, clear
            xtset firm year
            xtreg invest value capital, fe
```

## `use`

```
  use FILE [, clear]                           read Stata .dta, .tea, .csv, or .tsv
      e.g.  use mydata.dta, clear            — extension dispatch decides format
      'use foo' with no extension looks for foo.dta (Stata default).
```

## `clear`

```
  clear                                        drop all data in current frame
```

## `error`

```
  error #                                      abort with return code # (Stata do-file assertions)
```

## `compress`

```
  compress                                     accepted for compatibility; tea storage is already minimal
```

## `status`

```
  status                                       one-line summary: source, obs, vars, memory, sort/xtset state
```

## `frame`

```
  frame create|change|copy|rename|put|drop|dir  multiple datasets
      e.g.  frame create alt   |   frame change alt   |   frame put x y, into(alt)
```

## `regress`

```
  regress y x1 x2 ... [if] [in] [weight] [, noconstant robust cluster(var)]   OLS
      e.g.  regress gdp_growth investment trade if year>=2000, cluster(country)
```

## `predict`

```
  predict newvar [, xb residuals]                                          predict from last fit
```

## `test`

```
  test v1 v2 ...                                                           Wald F-test (joint zero)
```

## `lincom`

```
  lincom <linear combo of coefs>                                          point estimate + SE of L'b
```

## `help`

```
  help [cmd]                                   list commands or show one's syntax
```

## `pwd`

```
  pwd                                          print working directory
```

## `cd`

```
  cd DIR                                       change working directory
```

## `mkdir`

```
  mkdir DIR [, recursive]                      create directory (recursive = mkdir -p)
```

## `dir`

```
  dir [pattern]                                list files in current directory
```

## `rmdir`

```
  rmdir DIR                                    remove empty directory
```

## `erase`

```
  erase FILE  |  rm FILE                       delete a file
```

## `copy`

```
  copy SRC DST [, replace]                     copy a file
```

## `do`

```
  do FILENAME                                  run another do-file
```

## `preserve`

```
  preserve                                     snapshot the data in memory
```

## `restore`

```
  restore [, not preserve]                     bring the preserve snapshot back
```

## `version`

```
  version                                      tea version & build info
```

## `log`

```
  log using FILE [, replace append] | log close   tee output to a file
```

## `exit`

```
  exit [, clear]                               leave tea (clear drops data first)
```

## Native statements

Handled by the interpreter before command dispatch:
`display`, `assert`, `shell` (and the `!cmd` escape), `#delimit`,
`local`, `global`, `foreach`, `forvalues`, `while`, `if`/`else`,
`capture`, `quietly`, `program define`, and comments (`*`, `//`, `///`).

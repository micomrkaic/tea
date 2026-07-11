# tea for Stata users — a five-minute quick start

*You already know how to use this program.  This document proves it.*

`tea` (tiny econometric assistant) is a free, open-source (GPLv3),
single-binary program that covers the slice of Stata applied economists
use daily — import, clean, `generate`, `reshape`, `regress`, `xtreg`,
`ivregress`, `logit`, `poisson`, `arima`, publication tables, plots —
with Stata's syntax and, where it matters, Stata's exact semantics.
It runs on Linux and macOS, and **in any web browser with nothing
installed**.  It ships with six serious practice datasets embedded in
the binary, including the complete IMF World Economic Outlook database.

No license server.  No installation on locked-down machines.  No
telemetry — in the browser edition, your data never leaves the tab.

## Sixty seconds, zero installation

Open **<https://micomrkaic.github.io/tea/>** and type:

```stata
sysuse weo
keep if aggregate==0
xtset iso year
xtreg ngdp_rpch ggxwdg_ngdp if year<=2025, fe
```

That is the complete April-2026 World Economic Outlook — 197 economies,
145 indicators — and a growth-on-debt fixed-effects regression across
193 of them.  The output is exactly what your eyes expect:

```
Fixed-effects (within) regression               Number of obs   =     5854
Group variable: iso                             Number of groups =      193

R-sq:                                           Obs per group:
     within  = 0.0132                                  min =       14
     between = 0.0111                                  avg =     30.3
     overall = 0.0115                                  max =       46

------------------------------------------------------------------------------
   ngdp_rpch | Coefficient  Std. err.     t    P>|t|     [95% conf. interval]
-------------+----------------------------------------------------------------
 ggxwdg_ngdp | -0.0201611  0.00231467   -8.71 0.000   -0.0246987  -0.0156234
-------------+----------------------------------------------------------------
     sigma_u |    1.74613
     sigma_e |    5.44188
         rho |  0.0933459   (fraction of variance due to u_i)
------------------------------------------------------------------------------
```

Add `scatter pcpipch lur if year==2024` and a publication-grade vector
plot appears beside the terminal.  Total elapsed time: about a minute,
none of it spent on IT tickets.

## Your muscle memory transfers

These work exactly as your fingers already type them:

```stata
generate lgdp = ln(gdp)
replace  lgdp = . if gdp <= 0
egen     mgdp = mean(gdp), by(country)
bysort country (year): gen g = gdp/gdp[_n-1] - 1
keep if year >= 2000 & !missing(gdp)
sort country year
reshape long gdp, i(country) j(year)
merge 1:1 id using other.dta
tsset year
xtset country year
regress y L.x D2.z i.region c.x#i.region [aw=pop], cluster(country)
ivregress 2sls y (x = z1 z2) w
logit employed education experience
poisson visits chronic income
arima y, arima(1 0 1)
predict yhat
estimates store m1
estimates table m1 m2, se
estout m1 m2 using results.tex
display _b[x] " (" _se[x] ")"
foreach v in gdp inflation unemp {
    quietly summarize `v'
    display "`v': " r(mean)
}
```

The semantics are faithful where faithfulness matters most:

- **Missing values** follow Stata's algebra exactly — including the
  classic footgun that `if x > 5` *includes* missing, because missing
  sorts above every number.  Your defensive habits (`& !missing(x)`)
  work unchanged.
- **`by:`** errors on unsorted data, exactly like Stata; `bysort`
  sorts first; `by g (s):` groups by `g` sorting within by `s`.
- **Time-series operators** `L. F. D. S.` are gap-aware against the
  declared panel, not row-shifts.
- **Factor variables** `i.` `c.` `#` `##` expand in estimation
  varlists with Stata's coefficient naming.
- Weights (`fw aw pw iw`), `robust`, `cluster()`, `if`/`in` — all where
  you expect them.

## Batteries included: `sysuse` with real data

```
. sysuse dir
  grunfeld  Grunfeld (1958) investment panel: 10 US firms x 1935-1954 (xtreg, hausman)
  airline   Box-Jenkins airline passengers, monthly 1949-1960 (tsset, arima)
  longley   Longley (1967) US macro, 16 obs: famously ill-conditioned (regress)
  nmes1988  Deb-Trivedi (1997) medical care demand, 4406 persons 66+ (poisson, logit)
  pwt       Penn World Table 10.0 sample: 22 countries x 1950-2019, CC BY 4.0 (growth)
  weo       IMF World Economic Outlook, April 2026: 197 countries + 13 aggregates, 145 indicators
```

The datasets live *inside* the binary — email one file to a colleague
and they have the program *and* the data.  They are chosen so the
classics reproduce:

```stata
sysuse grunfeld
xtset firm year
quietly xtreg invest value capital, fe
estimates store fe
quietly xtreg invest value capital, re
estimates store re
hausman
```

```
             |      fe           re          Difference     S.E.
-------------+----------------------------------------------------------------
       value |   0.110124      0.109785     0.000338967    0.00549325
     capital |   0.310065      0.308146      0.00191976    0.00247942
------------------------------------------------------------------------------
Test: Ho:  difference in coefficients not systematic
              chi2(2) = (b-B)'[(V_b-V_B)^(-1)](b-B)
                       =     2.07
            Prob > chi2 =   0.3551
```

Those are the textbook Grunfeld numbers, to the digit.  `longley`
reproduces the certified solution of the famously ill-conditioned
regression; `nmes1988` is real health-economics microdata for count
and binary models; the WEO's World and regional aggregates are one
`keep if aggregate==1` away.

## Three things Stata does not give you

1. **A browser edition.**  The identical program compiled to
   WebAssembly: same commands, same estimators, same data, same output
   to the byte.  Send a colleague a link instead of an installation
   guide.  Uploaded `.dta`/`.csv` files stay in the tab — there is no
   server.
2. **Reproducibility as a promise.**  The same do-file prints
   *byte-identical* output on every machine, CPU, and BLAS library —
   engineered (degenerate-case zeros are handled deterministically) and
   verified by running the entire regression suite on two maximally
   different numerical stacks.  Referee asks "does this replicate?" —
   the answer is `diff`.
3. **A single 4 MB binary, free forever.**  GPLv3.  No license file,
   no seat count, no expiry, readable source on GitHub.

## Honesty section: what tea is not

tea is a deliberately small tool, not a Stata replacement.  It has no
`mixed`, no `gmm`, no Bayesian suite, no survey (`svy:`) machinery, no
`twoway` graphics grammar, no matrix language (`mata`), and `merge m:m`
is refused on purpose.  The full differences list ships as
`COMPATIBILITY.md`.  For anything outside scope, the escape hatch is
built in:

```stata
export delimited handoff.csv
shell python3 analyze.py handoff.csv
```

If your day is 80% data-prep and standard estimators — which is most
applied days — tea covers it.  For the other 20%, it hands off cleanly.

## Getting it

- **Try now, install nothing:** <https://micomrkaic.github.io/tea/>
- **Native binary:**

  ```
  git clone https://github.com/micomrkaic/tea
  cd tea && make && make test     # 42/42 expected
  ```

- **Manual:** `tea-manual.pdf` — 38 pages, full command and
  function reference, generated from the program itself so it cannot
  drift from the implementation.

Found a bug, or a missing feature you cannot live without?  Open an
issue — this project moves fast, and field reports drive it.

*tea is an independent open-source project by Mico Mrkaic, not
affiliated with or endorsed by StataCorp LLC.  Stata is a trademark of
StataCorp LLC.  Bundled datasets retain their original licenses and
citations (`data/SOURCES.md`); WEO figures shown are from the April
2026 vintage.*

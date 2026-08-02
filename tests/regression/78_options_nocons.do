* 78_options_nocons — Bug 42 (option hygiene) and Bug 43 (noconstant
* ANOVA convention), both found by Mico testing in the browser.
* Bug 42: opt_present's left-boundary test was !isalnum, so `_nocons`
* MATCHED nocons (underscore isn't alnum), while any other unknown
* option was silently IGNORED — a misspelled `robusst` silently gave
* classical SEs.  Fix: token boundaries are space/comma only, plus
* consumption tracking — after a handler succeeds, any option token it
* never queried is Stata's r(198).  graph/outreg2 parse their own
* options and are exempt via the Disp rawopts flag.
* Bug 43: with noconstant, regress used the CENTERED total SS, so a
* through-origin fit printed negative Model SS and R-squared (-28.7 on
* the airline trend).  Stata decomposes about zero: uncentered TSS,
* df_total = N.  Verified against statsmodels to printed precision
* (R2 .8139, adj .8126, F 625.46).
sysuse airline
quietly gen lnp = ln(passengers)
quietly gen t = _n
capture regress lnp t, _nocons
display "underscore-prefixed option rejected: " (_rc == 198)
capture regress lnp t, robusst
display "misspelled option rejected: " (_rc == 198)
capture summarize lnp, detil
display "misspelled sum option rejected: " (_rc == 198)
quietly regress lnp t, robust
display "legit option ok: " (_rc == 0)
quietly regress lnp t, nocons
display "nocons r2 ok: "  (abs(e(r2) - .8139) < .0005)
display "nocons b ok: "   (abs(_b[t] - .060017) < .00005)
quietly regress lnp t i.month, nocons
display "nocons factor r2 ok: " (abs(e(r2) - .9504) < .0005)
quietly regress lnp t i.month
display "with-cons r2 unchanged ok: " (abs(e(r2) - .9835) < .0005)

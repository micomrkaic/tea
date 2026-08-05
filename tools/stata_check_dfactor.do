* stata_check_dfactor.do — run in STATA to pin the dfactor tier against
* tea test 73.  Conventions (COMPATIBILITY.md): tea's dfactor is exact
* ML with OIM standard errors; Stata's dfactor is also exact ML but
* reports OIM by default here too, so estimates AND SEs should agree
* to reported precision on identified models.  tea's constraint
* command accepts the same linear-constraint syntax.
* ---- reference oracle (added v1.6.50) ---------------------------------
* Pin against Stata 19 SEMANTICS regardless of the running release:
* `version 19' is StataCorp's own reproducibility contract, so a
* rolling StataNow license still delivers a frozen target.  The next
* two lines make every pin run self-documenting in its log.
version 19
display "oracle: Stata " c(stata_version) " " c(edition_real) ", born " c(born_date)
* Datasets: run tools/make_pin_data.do in tea first (same seeds and
* recursions as the regression tests; writes eight .dta files).

use df1, clear                 // export from tea: the k=1 sim
tsset t
dfactor (y1 y2 y3 = ) (f1 = , ar(1))
use df3, clear                 // the k=2 sim, demeaned
tsset t
constraint 1 [y1]f2 = 0
dfactor (y1 y2 y3 y4 = , noconstant) (f1 f2 = , ar(1)), constraints(1)

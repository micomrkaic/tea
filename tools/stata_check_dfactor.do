* stata_check_dfactor.do — run in STATA to pin the dfactor tier against
* tea test 73.  Conventions (COMPATIBILITY.md): tea's dfactor is exact
* ML with OIM standard errors; Stata's dfactor is also exact ML but
* reports OIM by default here too, so estimates AND SEs should agree
* to reported precision on identified models.  tea's constraint
* command accepts the same linear-constraint syntax.
use df1, clear                 // export from tea: the k=1 sim
tsset t
dfactor (y1 y2 y3 = ) (f1 = , ar(1))
use df3, clear                 // the k=2 sim, demeaned
tsset t
constraint 1 [y1]f2 = 0
dfactor (y1 y2 y3 y4 = , noconstant) (f1 f2 = , ar(1)), constraints(1)

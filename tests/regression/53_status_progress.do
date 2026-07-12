* status + `set progress` — tea quality-of-life extensions (not Stata).
* status: one-line dataset summary (source, obs, vars, exact memory,
* sort/xtset state).  Source tracking: set by use/import/sysuse, updated
* by save, cleared by clear.
status
sysuse grunfeld, clear
status
sort year
status
xtset firm year
status
save /tmp/tea_test53.tea, replace
status
use /tmp/tea_test53.tea, clear
status
clear
status
* progress drawing is TTY-gated (invisible in batch by design); the
* toggle parsing is what we can lock here
set progress off
set progress on
capture set progress banana
display _rc

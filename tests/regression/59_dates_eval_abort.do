* 59_dates_eval_abort — OECD HPI round:
* (1) period-date constructors: quarterly/monthly/halfyearly/weekly/yearly
*     (+ daily as the Stata alias of date); out-of-range periods and
*     unparseable strings give missing, never a wrong date
* (2) a runtime eval error ABORTS gen/replace (rc=133) with full rollback:
*     the old code printed the error once per row, stored missing anyway,
*     counted "real changes", and returned 0 — a do-file then marched into
*     collapse/merge with an all-missing column and saved garbage
clear
set obs 4
gen str8 t = ""
replace t = "2020-Q1" in 1
replace t = "2020-Q3" in 2
replace t = "2021q4"  in 3
replace t = "2020-Q7" in 4
gen q = quarterly(t, "YQ")
format q %tq
list t q
display quarterly("2020-Q3","YQ")
display monthly("1997m11","YM")
display yearly("2005","Y")
display halfyearly("2019-H2","YH")
display daily("15-3-2020","DMY")
display dofq(quarterly("2020-Q3","YQ"))
display yofd(dofq(quarterly("2020-Q3","YQ")))
* unknown function: gen must abort loudly and leave NO trace of the var
capture gen bad = nosuchfunc(t)
display _rc
capture confirm variable bad
display _rc
* replace must roll back completely on abort
gen x = _n
capture replace x = nosuchfunc(t)
display _rc
list x

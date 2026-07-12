version 16
capture which outreg2
if _rc ssc install outreg2
sysuse grunfeld, clear
xtset firm year
label variable invest "Gross investment"
label variable value "Market value"
local OR2OPTS excel dec(3) bdec(3) se symbol(***, **, *) alpha(0.01, 0.05, 0.10) label
local NOTES addnote("SEs in parentheses. Significance: *** p<0.01, ** p<0.05, * p<0.10.")
quietly xtreg invest value capital, fe
outreg2 using /tmp/tea_test_table1.txt, replace `OR2OPTS' `NOTES' ///
    ctitle("FE") addstat("Within R^2", e(r2_w), "Groups", e(N_g))
quietly xtreg invest value capital, re
outreg2 using /tmp/tea_test_table1.txt, append `OR2OPTS' `NOTES' ///
    ctitle("RE") addstat("Within R^2", e(r2_w), "Groups", e(N_g)) ///
    addtext(Country FE, YES, Year FE, NO)
quietly regress invest value
outreg2 using /tmp/tea_test_table1.txt, append `OR2OPTS' `NOTES' ctitle("OLS")
tempfile scratch
display "tempfile ok"
isid firm year
duplicates report firm year
pwcorr invest value capital
shell cat /tmp/tea_test_table1.txt

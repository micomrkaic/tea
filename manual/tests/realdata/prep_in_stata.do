* prep_in_stata.do — ONE-TIME setup, run in Stata only.
*
* Produces two test fixtures in this directory:
*   auto.dta        — Stata's bundled example dataset, copied here
*   survey.dta      — synthetic value-labeled dataset for .dta-polish tests
*
* After this runs once, the other .do files in this directory can be
* executed in BOTH tea and Stata against these fixtures.
*
* Usage in Stata:
*   cd /path/to/tea/tests/realdata
*   do prep_in_stata.do

clear all

* ---- 1. Copy auto.dta into this directory ----
sysuse auto, clear
save auto.dta, replace

* ---- 2. Build a value-labeled survey-like dataset ----
clear
set obs 200
set seed 20260528
gen long id = _n
gen byte sex = 1 + (runiform() < 0.5)
gen byte region = 1 + int(runiform() * 4)
gen byte edu = 1 + int(runiform() * 3)
gen double income = 30000 + 50000 * runiform() + 20000 * (edu - 1)
gen double age = 18 + int(60 * runiform())
* sampling weight: probability inverse — heavier weight on lower-income strata
gen double svywt = 1 / (0.3 + (income / 100000))

label define sexlbl 1 "Female" 2 "Male"
label values sex sexlbl
label define reglbl 1 "Northeast" 2 "Midwest" 3 "South" 4 "West"
label values region reglbl
label define edulbl 1 "HS" 2 "College" 3 "Graduate"
label values edu edulbl

label variable id "respondent id"
label variable sex "respondent sex"
label variable region "census region"
label variable edu "educational attainment"
label variable income "annual income, USD"
label variable age "age in years"
label variable svywt "sampling weight"

label data "Synthetic survey for tea testing, n=200"

save survey.dta, replace
display "OK: wrote auto.dta and survey.dta"

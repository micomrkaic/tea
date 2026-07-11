* dta_polish.do — exercises the .dta polish work: value labels, dataset
* label, version() option, and read-side fweight metadata capture.
*
* Runs in BOTH tea and Stata.
*
* Usage:
*   tea   dta_polish.do > out_tea.txt   2>&1
*   stata -b do dta_polish.do          # produces dta_polish.log

clear all
use survey.dta

display "=========================================================="
display "1. DESCRIBE: dataset label should show"
display "=========================================================="
describe

display "=========================================================="
display "2. LIST first 5: value labels should be decoded"
display "=========================================================="
list in 1/5

display "=========================================================="
display "3. LABEL LIST: all three label sets should appear"
display "=========================================================="
label list

display "=========================================================="
display "4. TABULATE sex: should show Female/Male, not 1/2"
display "=========================================================="
tabulate sex
display "--- and region ---"
tabulate region

display "=========================================================="
display "5. ROUND-TRIP: save -> clear -> use, check everything preserved"
display "=========================================================="
save survey_roundtrip.dta, replace
clear
use survey_roundtrip.dta
display "--- after round-trip, describe ---"
describe
display "--- list 1/5 should still show labels ---"
list in 1/5
display "--- label list still populated? ---"
label list

display "=========================================================="
display "6. VERSION 117 (older Stata) round-trip"
display "=========================================================="
save survey_v117.dta, replace version(117)
clear
use survey_v117.dta
describe
display "--- did the v117 file still round-trip? ---"
list in 1/3

display "=========================================================="
display "7. NUMERIC SUMMARY (verify data values survived encoding)"
display "=========================================================="
summarize income age
display "Expected (Stata too): same numbers from original vs round-tripped"

* Cleanup
capture erase survey_roundtrip.dta
capture erase survey_v117.dta

display "=========================================================="
display "END OF TEST"
display "=========================================================="

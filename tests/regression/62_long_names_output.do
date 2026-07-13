* 62_long_names_output — WPP 26-char names shoved sum/describe/tabstat
* columns out of line.  sum and tabstat now use Stata's abbrev() rule
* (first n-2 chars + '~' + last char: totalpopul~r); describe pads its
* name column dynamically to the longest name in view (16..32), header
* included.  Short-name frames render byte-identically to before.
clear
set obs 12
gen totalpopulationasof1januar = _n*1.5
gen femalepopulationasof1july = _n*2.5
gen TFR = 4 - _n*0.1
gen grp = 1 + mod(_n,2)
label define grpl 1 "Advanced economies plus" 2 "Rest"
label values grp grpl
sum
describe
sum TFR
tabstat totalpopulationasof1januar TFR, by(grp) stats(mean) format(%8.2f)
tabstat totalpopulationasof1januar, by(grp) stats(mean sd)

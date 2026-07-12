* 45 — unique variable abbreviation + block comments
set obs 3
gen lics = 1
gen lacs = 2
display "abbrev read: " lic[1]
replace lics = 5 if lac == 2
summarize lic
keep lic lac
capture keep if l == 1
display "ambiguous rc: " _rc
gen lic = 99
display "exact creation: " lic[1]

/* a block comment
spanning several lines
with an unrecognized command inside: import haver x@Y */
display "after block comment"
display 1 /* inline */ + 1
gen z = 6 /* nested /* inner */ outer */ + 1
display "nested ok: " z[1]
display 5 /* end of line
start of line */ + 5

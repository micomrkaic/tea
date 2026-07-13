* 61_tabstat_labels_format — fertility descriptives round:
* (1) tabstat by() groups are named by VALUE LABELS when attached
*     (encoded ctr_group -> "Advanced"), same rule as list / graph box
* (2) format(%5.2f) applies to every cell INCLUDING the Total row
*     (a second FMT_NUM definition in the Total block had been missed);
*     quoted format("%5.2f") accepted; non-numeric formats are loud
clear
set obs 90
gen g = 1 + mod(_n-1,3)
label define glbl 1 "Advanced" 2 "Emerging" 3 "LIC"
label values g glbl
set seed 55
gen y = 2 + g + 0.3*rnormal(0,1)
gen z = 100*g + rnormal(0,5)
tabstat y, by(g)
tabstat y, by(g) format(%5.2f)
tabstat y z, by(g) stats(mean sd) format(%9.2f)
tabstat y, by(g) format("%6.3f")
capture tabstat y, by(g) format(%s)
display _rc
capture tabstat y, by(g) format(banana)
display _rc

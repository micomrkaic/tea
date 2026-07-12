* 58_vallab_survival — value-label ATTACHMENTS must survive every frame
* rebuild.  Bug (v1.6.6): reshape, merge, frame copy, and the native
* .tea format (hence preserve/restore) all copied format+vlabel but
* dropped Variable.vallab — encoded group variables silently reverted
* to raw numerics in graph box band labels.  The .tea format is now
* TEA2 (carries vallab); legacy TEA1 files still read.
clear
cd /tmp
set obs 8
gen id = ceil(_n/2)
gen j = 1 + mod(_n-1,2)
gen g = 1 + mod(id-1,2)
label define glbl 1 "Alpha" 2 "Beta"
label values g glbl
gen v = _n*1.5
* reshape wide then long: attachment carried both directions
reshape wide v, i(id g) j(j)
reshape long v, i(id g) j(j)
graph box v, over(g) saving(/tmp/t58_a.svg)
shell grep -c ">Alpha<" /tmp/t58_a.svg
* preserve/restore (pst snapshot round-trip)
preserve
keep in 1/4
restore
graph box v, over(g) saving(/tmp/t58_b.svg)
shell grep -c ">Beta<" /tmp/t58_b.svg
* native .tea save/use (TEA2)
save /tmp/t58.tea, replace
clear
use /tmp/t58.tea
graph box v, over(g) saving(/tmp/t58_c.svg)
shell grep -c ">Alpha<" /tmp/t58_c.svg
* .dta save/use
save /tmp/t58.dta, replace
clear
use /tmp/t58.dta
graph box v, over(g) saving(/tmp/t58_d.svg)
shell grep -c ">Beta<" /tmp/t58_d.svg
* merge: attachments on both master and using survive
tempfile lut
preserve
    keep id g
    duplicates drop
    quietly save `lut'
restore
drop g
merge m:1 id using `lut'
drop _merge
graph box v, over(g) saving(/tmp/t58_e.svg)
shell grep -c ">Alpha<" /tmp/t58_e.svg
shell rm -f /tmp/t58_a.svg /tmp/t58_b.svg /tmp/t58_c.svg /tmp/t58_d.svg /tmp/t58_e.svg /tmp/t58.tea /tmp/t58.dta

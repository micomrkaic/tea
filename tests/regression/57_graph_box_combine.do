* 57_graph_box_combine — graph box with one and two over() levels, value
* labels + relabel(), label(angle() labsize()), noout, outliers drawn by
* default, and graph combine from the name() registry.  Golden SVG diffs.
clear
cd /tmp
set obs 120
set seed 991
gen g1 = 1 + mod(_n-1, 3)
gen str8 g2 = cond(_n <= 60, "North", "South")
gen y = 10 + 3*g1 + 2*(g2=="South") + rnormal(0,2)
replace y = 40 in 7
replace y = -10 in 8
label define g1lbl 1 "one" 2 "two" 3 "three"
label values g1 g1lbl
* single over(): value labels name the groups; outliers drawn
graph box y, over(g1) title("boxes") saving(/tmp/tea_t57_a.svg)
* two-level over() with relabel + rotated small labels + noout
graph box y, over(g1, relabel(1 "A" 2 "B" 3 "C") label(angle(45) labsize(vsmall))) ///
    over(g2, label(labsize(small))) noout note("no outliers") saving(/tmp/tea_t57_b.svg)
* registry + combine
graph box y, over(g1) name(t57p1, replace)
graph box y if g2 == "South", over(g1) name(t57p2, replace)
graph combine t57p1 t57p2, cols(2) title("combined") name(t57c, replace)
graph dir
graph drop t57p1
graph dir
shell cat /tmp/tea_t57_a.svg
shell cat /tmp/tea_t57_b.svg
shell cat /tmp/t57c.svg
* error paths
capture graph box y, over(nosuchvar)
display _rc
capture graph combine nosuchgraph
display _rc
* name collision without replace is rc 110; with replace it succeeds
capture graph box y, over(g1) name(t57p2)
display _rc
graph box y, over(g1) name(t57p2, replace)
graph drop _all
graph dir
* one cleanup at the very end: host-side rm of files the wasm rig has
* open NODEFS nodes for must never precede a rewrite (stale-cache trap)
shell rm -f /tmp/tea_t57_a.svg /tmp/tea_t57_b.svg /tmp/t57p1.svg /tmp/t57p2.svg /tmp/t57c.svg /tmp/tea_graph.svg

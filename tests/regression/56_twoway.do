* 56_twoway — multi-series twoway: line/scatter/lowess overlays with
* per-series if, marker labels (msymbol(i) + mlabel + mlabposition),
* yline, ylabel rule vs range semantics, legend, series-level axis
* titles, and the name() registry writing NAME.svg.  Golden SVG diffs.
clear
cd /tmp
set obs 30
set seed 777
gen t = _n
gen a = 2 + 0.1*t + rnormal(0,0.3)
gen b = 4 - 0.08*t + rnormal(0,0.3)
gen str3 tag = ""
replace tag = "END" in 30
twoway (line a t, lcolor(blue)) (line b t if t > 5, lcolor(red) lpattern(dash)) ///
    (scatter a t if tag != "", msymbol(i) mlabel(tag) mlabposition(3) mlabcolor(black)), ///
    yline(3, lpattern(dot) lcolor(black)) ylabel(0(1)5) legend(off) ///
    xtitle("time") ytitle("level") title("golden twoway") saving(/tmp/tea_t56_a.svg)
twoway (scatter a t, ytitle("inside series")) (lowess a t, adjust lcolor(green)), ///
    saving(/tmp/tea_t56_b.svg)
* legend defaults ON for multi-series (deterministic swatch list)
twoway (line a t) (line b t), saving(/tmp/tea_t56_c.svg)
* name() writes NAME.svg alongside the registry
twoway (line a t), name(t56g, replace)
shell cat /tmp/tea_t56_a.svg
shell cat /tmp/tea_t56_b.svg
shell cat /tmp/tea_t56_c.svg
shell cat /tmp/t56g.svg
shell rm -f /tmp/tea_t56_a.svg /tmp/tea_t56_b.svg /tmp/tea_t56_c.svg /tmp/t56g.svg /tmp/tea_graph.svg
* error paths: loud, never silent
capture twoway (banana a t)
display _rc
capture twoway (line a nosuchvar)
display _rc
graph drop _all

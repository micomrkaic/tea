* 40_plot — scatter / line / histogram render deterministic SVG
* Strategy: write each plot with saving(), stream the files back through
* stdout with `shell cat`, so the .expected file carries the golden SVG.
set obs 40
set seed 12345
gen x = rnormal(0, 1)
gen y = 1 + 2*x + rnormal(0, 0.5)
gen t = _n

scatter y x, title("golden scatter") saving(/tmp/tea_t40_sc.svg)
line y t in 1/20, sort saving(/tmp/tea_t40_ln.svg)
histogram x, bins(8) freq saving(/tmp/tea_t40_hs.svg)
scatter y x if x > 0, saving(/tmp/tea_t40_if.svg)

shell cat /tmp/tea_t40_sc.svg
shell cat /tmp/tea_t40_ln.svg
shell cat /tmp/tea_t40_hs.svg
shell cat /tmp/tea_t40_if.svg
shell rm -f /tmp/tea_t40_sc.svg /tmp/tea_t40_ln.svg /tmp/tea_t40_hs.svg /tmp/tea_t40_if.svg

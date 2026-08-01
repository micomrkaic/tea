* 76_panel_tier — areg, xtivreg fe, xtabond (DESIGN_PANEL.md).
* areg verified by internal exactness vs regress with the dummy set
* (coefficients AND classical SEs identical to every printed digit,
* including the N-K-G df).  xtivreg fe verified against a numpy
* within+2SLS referee (exact).  xtabond one-step/robust/two-step
* verified against an independent numpy GMM referee implementing the
* same Arellano-Bond formulas: every reported digit identical
* (L.y .449738, se .061213, robust .036321; Sargan 16.8021 on 20 df).
* Tolerance asserts guard against cross-BLAS dust in the 22x22
* weight inversions.
clear
set seed 9
set obs 200
gen id = ceil(_n/40)
gen x1 = rnormal(0,1)
gen x2 = rnormal(0,1)
gen fe = id*0.7
gen y = 2*x1 - 1*x2 + fe + rnormal(0,1)
quietly areg y x1 x2, absorb(id)
display "areg b ok: "  (abs(_b[x1] - 1.96729) < .00002) * (abs(_b[x2] - (-1.07641)) < .00002)
display "areg se ok: " (abs(_se[x1] - .0725277) < .00002)
quietly areg y x1 x2, absorb(id) cluster(id)
display "areg cluster se ok: " (abs(_se[x2] - .0522852) < .0002)
* xtivreg fe
clear
set seed 33
set obs 300
gen id = ceil(_n/30)
gen t = _n - 30*(id-1)
quietly xtset id t
gen z = rnormal(0,1)
gen ev = rnormal(0,1)
gen w = 0.8*z + 0.6*ev + rnormal(0,0.5)
gen x1 = rnormal(0,1)
gen y = 1.5*w + 0.7*x1 + id*0.3 + ev + rnormal(0,0.3)
quietly xtivreg y x1 (w = z), fe
display "xtivreg b ok: "  (abs(_b[w] - 1.62429) < .0002) * (abs(_b[x1] - .733693) < .0002)
display "xtivreg se ok: " (abs(_se[w] - .0655738) < .0002)
* xtabond
clear
set seed 44
set obs 1600
gen id = ceil(_n/8)
gen t = _n - 8*(id-1)
quietly xtset id t
gen a = 0.5*ceil(_n/8)/25
gen x1 = rnormal(0,1)
gen y = 0
quietly replace y = a + x1*0.6 + rnormal(0,1) if t==1
forvalues tt = 2/8 {
    quietly replace y = 0.5*y[_n-1] + 0.6*x1 + a + rnormal(0,1) if t==`tt'
}
quietly xtabond y x1, lags(1)
display "xtabond b ok: "      (abs(_b[Ly] - .449738) < .0005) * (abs(_b[Dx1] - .58292) < .0005)
display "xtabond se ok: "     (abs(_se[Ly] - .061213) < .0005)
display "xtabond sargan ok: " (abs(e(sargan) - 16.8021) < .01)
quietly xtabond y x1, lags(1) robust
display "xtabond robust se ok: " (abs(_se[Ly] - .036321) < .0005)
quietly xtabond y x1, lags(1) twostep
display "xtabond twostep b ok: " (abs(_b[Ly] - .446) < .001)

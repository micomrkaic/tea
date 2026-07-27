* 41_sysuse — bundled datasets load and estimate correctly.
* Embedded data is fully deterministic, so estimates diff byte-for-byte.
sysuse dir

sysuse grunfeld
display "grunfeld: " _N " obs"
xtset firm year
xtreg invest value capital, fe

* clear guard: loading over data must fail without ,clear (capture: no abort)
capture sysuse longley
sysuse longley, clear
* longley is deliberately near-singular: OpenBLAS on x86, OpenBLAS on
* Apple silicon, and the WASM reference BLAS agree on this matrix only
* to ~5-6 significant digits, so the full coefficient table is not
* byte-reproducible across rigs.  Smoke-test the regression at the
* precision this matrix actually reproduces everywhere.
quietly regress employed gnpdeflator gnp unemployed armedforces population year
display e(N)
display round(e(r2), .0001)
display round(_b[year], .0001)
display round(_se[year], .0001)
display round(_b[_cons], .01)

sysuse airline, clear
summarize passengers

sysuse nmes1988, clear
poisson visits chronic school income

sysuse pwt, clear
gen lgdp = ln(rgdpna/pop)
summarize lgdp

* unknown dataset errors cleanly
capture sysuse nosuchdata, clear
display "still alive"

* weo: the full April 2026 WEO database
sysuse weo, clear
display "weo: " _N " obs"
keep if aggregate==0
display "countries only: " _N " obs"
xtset iso year
xtreg ngdp_rpch ggxwdg_ngdp if year<=2025, fe
summarize pcpipch if year==2023

* utf-8 column alignment (Côte d'Ivoire, São Tomé must not shift pipes)
sysuse weo, clear
keep if iso=="CIV" | iso=="USA" | iso=="STP"
keep if year==2024
tabulate country

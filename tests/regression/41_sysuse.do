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
regress employed gnpdeflator gnp unemployed armedforces population year

sysuse airline, clear
summarize passengers

sysuse nmes1988, clear
poisson visits chronic school income

sysuse pwt, clear
gen lgdp = ln(rgdpna/pop)
summarize lgdp

sysuse weo, clear
display "weo: " _N " obs"
keep if aggregate==0
xtset iso year
summarize ngdp_rpch if iso=="USA"
xtreg ngdp_rpch pcpipch lur, fe

* unknown dataset errors cleanly
capture sysuse nosuchdata, clear
display "still alive"

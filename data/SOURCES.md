# Bundled practice datasets — provenance and licensing

These ship inside the tea binary and load with `sysuse NAME`.  `sysuse dir`
lists them at the prompt.  All were obtained via the Rdatasets mirror
(github.com/vincentarelbundock/Rdatasets) of the cited R packages and
lightly curated (row-name columns dropped, variable names normalized,
PWT trimmed to core columns).  Regenerate the embedded C source after any
change with:  python3 tools/gen_sysdata.py

## grunfeld  (200 obs: 10 US firms x 1935-1954)
Grunfeld, Y. (1958) "The Determination of Corporate Investment", PhD
thesis, University of Chicago.  Via R package `plm`.  The canonical panel
teaching dataset (fixed vs random effects, hausman).
Variables: firm year invest value capital.
Status: public-domain by long academic convention; ships with R (GPL).

## airline  (144 obs: monthly, 1949-1960)
Box, G.E.P. & Jenkins, G. (1976) "Time Series Analysis", Series G.
Via R `datasets::AirPassengers`.  The classic ARIMA series (trend +
multiplicative seasonality).  Variables: year month passengers.
Status: public-domain classic; ships with R.

## longley  (16 obs: US macro, 1947-1962)
Longley, J.W. (1967) JASA 62.  Via R `datasets::longley`.  Famously
ill-conditioned regression used to test numerical accuracy of least
squares — fitting `regress employed gnpdeflator gnp unemployed
armedforces population year` stresses any solver.
Status: US government statistics; public domain.

## nmes1988  (4406 obs: US National Medical Expenditure Survey, 1987/88)
Deb, P. & Trivedi, P.K. (1997) "Demand for Medical Care by the Elderly",
Journal of Applied Econometrics 12, 313-336.  Via R package `AER`
(Kleiber & Zeileis).  Health-economics microdata: physician visit counts,
hospital stays, health status, insurance, income for persons 66+.
Ideal for `poisson` (visits) and `logit` (insurance).
Status: JAE data archive / AER package (GPL); cite Deb & Trivedi.

## pwt  (1540 obs: 22 countries x 1950-2019)
Feenstra, R.C., Inklaar, R. & Timmer, M.P. (2015) "The Next Generation
of the Penn World Table", AER 105(10).  Sample of PWT 10.0 via R package
`stevedata`.  Variables: country isocode year pop emp hc rgdpna labsh avh.
Growth-accounting and cross-country panel practice.
Status: PWT is CC BY 4.0 — redistribution permitted with attribution.

## weo  (10168 obs: 197 economies + 13 aggregates x 1980-2031, 145 indicators)
International Monetary Fund, World Economic Outlook Database, April 2026
vintage (published 2026-04-14; snapshot of the public database, not live
updates).  Downloaded from the IMF data portal ("entire dataset" export)
and reshaped to a long panel: `country iso year aggregate` plus one
variable per WEO subject code, lowercased (`ngdp_rpch` = real GDP growth,
`pcpipch` = CPI inflation, ...).  The full code -> descriptor -> units
table is in data/weo_codes.txt.  `aggregate==1` marks World/regional
groups (G7, Euro Area, ...): use `keep if aggregate==0` for country
panels.  Years past the publication date are WEO projections.
Citation: IMF, World Economic Outlook Database, April 2026.
Status: public IMF database; use with attribution.  To refresh to a newer
vintage: download the portal export, run tools/curate_weo.py FILE, rerun
tools/gen_sysdata.py, update this entry.

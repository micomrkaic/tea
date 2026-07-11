* Regression: tabstat with if + by(...) must omit by-groups that have no
* rows passing the filter, instead of showing rows of dots.
clear
set obs 12
gen year = 2010 + mod(_n - 1, 4)
gen growth = _n * 0.5
* filter to just years 2011-2012 — groups for 2010 and 2013 must be omitted
tabstat growth if year > 2010 & year < 2013, by(year) columns(stats) stats(mean n)

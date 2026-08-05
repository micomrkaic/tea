
* ---- hygiene tier (v1.6.34): compare against tea test 69 ----
* ---- reference oracle (added v1.6.50) ---------------------------------
* Pin against Stata 19 SEMANTICS regardless of the running release:
* `version 19' is StataCorp's own reproducibility contract, so a
* rolling StataNow license still delivers a frozen target.  The next
* two lines make every pin run self-documenting in its log.
version 19
display "oracle: Stata " c(stata_version) " " c(edition_real) ", born " c(born_date)
* Datasets: run tools/make_pin_data.do in tea first (same seeds and
* recursions as the regression tests; writes eight .dta files).

use grunfeld, clear
correlate invest value capital
correlate invest value capital, means
correlate invest value, covariance
ttest invest == 145
gen post = year >= 1945
ttest invest, by(post)
ttest invest, by(post) unequal
ttest invest == capital

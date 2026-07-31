* 69_correlate_ttest — the hygiene tier (v1.6.34): correlate (listwise,
* with means and covariance) and ttest (one-sample, paired, two-sample
* pooled and Welch).  Numbers cross-checked against an independent
* implementation; tools/stata_check_vce.do's companion section pins
* layout against Stata.
sysuse grunfeld, clear
correlate invest value capital
correlate invest value capital, means
correlate invest value, covariance
correlate invest value if firm <= 5
ttest invest == 145
quietly gen post = year >= 1945
ttest invest, by(post)
ttest invest, by(post) unequal
ttest invest == capital

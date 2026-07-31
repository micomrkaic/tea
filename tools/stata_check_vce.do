
* ---- hygiene tier (v1.6.34): compare against tea test 69 ----
use grunfeld, clear
correlate invest value capital
correlate invest value capital, means
correlate invest value, covariance
ttest invest == 145
gen post = year >= 1945
ttest invest, by(post)
ttest invest, by(post) unequal
ttest invest == capital

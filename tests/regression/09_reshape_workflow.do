* Regression: a complete wide-to-long reshape pipeline like the IMF WEO
* workflow, exercising rename, keep, reshape, and list together.
clear
set obs 3
gen str3 country_id = "USA"
replace country_id = "FRA" in 2
replace country_id = "JPN" in 3
gen v2020 = -2.0 + _n * 0.5
gen v2021 = 5.0 + _n * 0.5
gen v2022 = 3.0 + _n * 0.5
reshape long v, i(country_id) j(year)
rename v growth
list, sep(0)

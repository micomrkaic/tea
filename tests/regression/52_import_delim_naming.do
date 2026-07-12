* Regression: `import delimited` must follow Stata's naming rule —
* lowercase by default, invalid characters REMOVED (not underscored),
* empty/digit-leading/duplicate headers become position names v#,
* case(preserve|upper) opt-outs.
tempfile csv
file open fh using `csv', write
file write fh "Country Code,GDP growth (%),1960,gdp_pc,x,x" _n
file write fh "USA,1,2,3,4,5" _n
file close fh
import delimited using `csv', clear
describe
import delimited using `csv', clear case(preserve)
describe
import delimited using `csv', clear case(upper)
describe
capture import delimited using `csv', clear case(banana)
display _rc

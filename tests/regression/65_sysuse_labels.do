* 65_sysuse_labels — bundled datasets carry variable labels.  Each
* dataset embeds a "var<TAB>label" table (data/NAME.lbl, weo's generated
* from data/weo_codes.txt) applied by sysuse after the CSV load: the
* practice environment should teach the full describe/label workflow,
* not just bare column names.
sysuse grunfeld
describe
sysuse longley, clear
describe
sysuse nmes1988, clear
describe health adl insurance
sysuse pwt, clear
describe rgdpna hc labsh
sysuse airline, clear
describe
sysuse weo, clear
describe country iso year aggregate ngdp_rpch bca_ngdpd dsi

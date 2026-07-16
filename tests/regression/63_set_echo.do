* 63_set_echo — `set echo on` echoes each do-file line Stata-style
* (". <line>") before its output.  Default is OFF (documented deviation:
* Stata echoes `do` by default, but tea's batch-output contract and all
* golden tests predate the feature).  Interactive input never echoes
* (it is already visible); the web editor's Run turns echo on so the
* terminal shows commands with their results.
cd /tmp
tempname t
file open fh using /tmp/t63_inner.do, write replace
file write fh "clear" _n
file write fh "set obs 2" _n
file write fh "gen x = _n" _n
file write fh "display x[1] + x[2]" _n
file close fh
set echo on
do /tmp/t63_inner.do
set echo off
do /tmp/t63_inner.do
capture set echo banana
display _rc
shell rm -f /tmp/t63_inner.do

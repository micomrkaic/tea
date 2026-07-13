* 60_import_ranges — WPP fertility round:
* (1) import excel cellrange(A17:AF22000): real workbooks carry title
*     junk above the table; with firstrow, the range's first row is the
*     header row.  The slicer is CSV-aware (quoted fields may contain
*     commas, doubled quotes, embedded newlines).  The xlsx path itself
*     needs ssconvert (absent on the WASM rig), so this test exercises
*     the SAME slicer via import delimited's rowrange()/colrange().
* (2) case("lower") — the quoted form is legal Stata; it errored on the
*     delimited branch and was silently ignored (hardcoded preserve) on
*     the excel branch.
clear
cd /tmp
tempname junk
file open fh using /tmp/t60.csv, write replace
file write fh "United Nations junk title" _n
file write fh "more junk, with, commas" _n
file write fh `"quoted "junk" line"' _n
file write fh "IndexCol,Region Name,Val A,Val B,Trailing" _n
file write fh `"1,"Country, One",10,20,x"' _n
file write fh "2,CountryTwo,30,40,x" _n
file write fh "3,CountryThree,50,60,x" _n
file write fh "4,CountryFour,70,80,x" _n
file close fh
* rows 4..7 (header + 3 data rows), cols 1..4 (Trailing trimmed)
import delimited /tmp/t60.csv, rowrange(4:7) colrange(1:4) case("upper") clear
describe
list
* start-only rowrange: header at 4, ALL remaining rows
import delimited /tmp/t60.csv, rowrange(4) colrange(1:4) case(preserve) clear
describe
list IndexCol ValA in 4
* error paths: malformed ranges are loud
capture import delimited /tmp/t60.csv, rowrange(banana) clear
display _rc
capture import delimited /tmp/t60.csv, rowrange(7:4) clear
display _rc
shell rm -f /tmp/t60.csv

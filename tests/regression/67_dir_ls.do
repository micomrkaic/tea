* 67_dir_ls — the dir/ls command (Stata's file listing), resurrected:
* it was fully implemented (patterns and all) but never registered in
* the command table — dead code found while adding sandbox-filesystem
* visibility for the browser edition, where dir is how you see what
* you dropped in.  Sizes are deterministic because the dta writer is.
quietly cd /tmp
capture rmdir t67
mkdir t67
quietly cd t67
clear
set obs 3
gen x = _n
quietly save a.dta, replace
quietly save b.dta, replace
dir
ls *.dta
quietly cd /tmp

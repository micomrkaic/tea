* Regression, WEO test_04 round:
* (1) `if COND {` used to evaluate against an EMPTY SCRATCH frame with
*     _N hardwired to 1 — `if _N > 0` was ALWAYS true and `if _N == 0`
*     ALWAYS false, independent of the data.  Conditions now evaluate
*     against the active frame (_n=1, _N=real obs count), so variable
*     references like x[1] work too.
* (2) quietly { ... } / capture { ... } block forms (prefixes chain).
* (3) `error #` and `compress`; display `as error` etc. are directives.
clear
set obs 3
gen x = _n
keep if x > 99
if _N > 0 {
    display "WRONG: ran with _N==0"
}
if _N == 0 {
    display "ok: empty branch"
}
clear
set obs 2
gen x = _n
if _N > 0 {
    display "ok: nonempty branch, _N=`=_N'"
}
if x[1] == 1 {
    display "ok: variable-indexed condition"
}
* quietly block: swallows display and command chatter
quietly {
    display "WRONG: quietly display printed"
    gen y = x*2
}
display y[2]
* capture block: swallows the abort, sets _rc
capture {
    gen z = x*3
    nosuchcommand
}
display _rc
display z[2]
* chained prefixes on a block
capture quietly {
    error 459
}
display _rc
* error and compress
capture error 459
display _rc
capture error banana
display _rc
compress
display as error "styled" as text " and more"

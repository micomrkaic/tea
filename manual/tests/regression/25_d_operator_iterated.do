* Regression: D# operator computes iterated k-th differences (Stata
* convention).  Before the fix, D2.x, D3.x etc. all returned the same
* as D.x (silent math bug — caused perfect collinearity in regress and
* a misleading "(omitted)" message).
*
*   D.x  = x[t] - x[t-1]
*   D2.x = D.D.x = x[t] - 2*x[t-1] + x[t-2]
*   D3.x = x[t] - 3*x[t-1] + 3*x[t-2] - x[t-3]
*
* Verified with x = t^2 (squares):
*   x   = 1, 4, 9, 16, 25, 36, 49, ...
*   D.x = ., 3, 5,  7,  9, 11, 13, ...   (2t-1, varies linearly)
*   D2.x= ., ., 2,  2,  2,  2,  2, ...   (constant 2)
*   D3.x= ., ., ., 0,  0,  0,  0, ...   (constant 0, since D2 is constant)
clear
set obs 10
gen str1 panel = "A"
gen year = _n
gen x = _n * _n
xtset panel year
gen dx  = D.x
gen d2x = D2.x
gen d3x = D3.x
list year x dx d2x d3x
display ""
display "S vs D: S2.x = x[t]-x[t-2] (simple gap), not iterated"
gen s2x = S2.x
list year x s2x
display "Expected: S2.x = (t+1)*(t-1) at row t, so 3,8,15,24,35,48,63,80,99 from row 3 onward"

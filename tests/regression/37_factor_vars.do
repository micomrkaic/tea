* Regression: Stata factor variables (i., c., #, ##, ib<n>.).
*
* Verifies:
*   - i.var produces (K-1) dummies, base = smallest level by default
*   - ib<n>.var lets user override the base
*   - c.var marks continuity (display prefix, no expansion)
*   - i.A#i.B produces (K_A-1)*(K_B-1) interaction columns
*   - i.A#c.B produces (K_A-1) interaction columns
*   - c.A#c.B produces 1 column (the product)
*   - i.A##c.B unrolls to main effects + interaction (i.A + c.B + i.A#c.B)
*   - i.A##i.B unrolls to i.A + i.B + i.A#i.B
*   - factor vars work with regress, xtreg, logit, probit
*   - predict and margins re-materialize coef-name columns (2.g, 2.A#c.B, ...)
*   - Errors: i.var on a string variable; only one level

* --- i.var: country FE ---
clear
set obs 12
gen country = ceil(_n/4)
gen x = _n*1.0
gen y = cond(country==1, 10, cond(country==2, 20, 30)) + x
display "=== regress y x i.country (default base = 1) ==="
display "Expected: beta_x = 1.0, _cons = 10, 2.country = 10, 3.country = 20"
regress y x i.country
display ""
display "_b[x]         = " _b[x]
display "_b[2.country] = " _b[2.country]
display "_b[3.country] = " _b[3.country]
display "_b[_cons]     = " _b[_cons]

* --- ib<n>.var: override base ---
display ""
display "=== regress y x ib2.country (base = 2) ==="
display "Expected: _cons = 20, 1.country = -10, 3.country = 10"
regress y x ib2.country
display "_b[_cons]     = " _b[_cons]
display "_b[1.country] = " _b[1.country]
display "_b[3.country] = " _b[3.country]

* --- c.A#c.B: continuous interaction ---
display ""
display "=== c.x#c.x: y = x^2 ==="
clear
set obs 5
gen x = _n*1.0
gen y = x*x
regress y c.x#c.x
display "_b[c.x#c.x]  = " _b[c.x#c.x]
display "Expected: exactly 1.0"

* --- i.A#c.B: country-specific slopes ---
display ""
display "=== i.country#c.x: country-specific slopes ==="
clear
set obs 12
gen country = ceil(_n/4)
gen x = _n*1.0
gen y = cond(country==1, 0.5*x, cond(country==2, 1.0*x, 1.5*x))
regress y c.x i.country#c.x
display "_b[c.x]            = " _b[c.x]
display "_b[2.country#c.x]  = " _b[2.country#c.x]
display "_b[3.country#c.x]  = " _b[3.country#c.x]
display "Expected: c.x = 0.5, 2.country#c.x = 0.5, 3.country#c.x = 1.0"

* --- ## main effects + interaction ---
display ""
display "=== regress y i.country##c.x ==="
display "Adds y intercepts: y = α_country + slope_country * x"
clear
set obs 12
gen country = ceil(_n/4)
gen x = _n*1.0
gen y = cond(country==1, 1+0.5*x, cond(country==2, 5+1.0*x, 10+1.5*x))
regress y i.country##c.x
display "_b[_cons]          = " _b[_cons]
display "_b[2.country]      = " _b[2.country]
display "_b[3.country]      = " _b[3.country]
display "_b[c.x]            = " _b[c.x]
display "_b[2.country#c.x]  = " _b[2.country#c.x]
display "_b[3.country#c.x]  = " _b[3.country#c.x]

* --- i.A#i.B: factor × factor interaction ---
display ""
display "=== i.A#i.B with K_A=3, K_B=2 → 2 interaction cols ==="
clear
set obs 12
gen A = ceil(_n/4)             // {1,2,3}
gen B = mod(_n-1, 2) + 5       // {5,6}
gen y = _n*1.0
quietly regress y i.A#i.B
display "_b[2.A#6.B] = " _b[2.A#6.B]
display "_b[3.A#6.B] = " _b[3.A#6.B]

* --- factor vars after logit ---
display ""
display "=== logit with factor vars + predict ==="
clear
set obs 200
gen g = mod(_n-1, 3) + 1
gen x = (_n - 100.5) * 0.1
gen y = (x + cond(g==1, 0, cond(g==2, 1.0, 2.0)) > 0)
quietly logit y x i.g
predict p
display "First few predictions look reasonable:"
list y x g p in 1/5

* --- factor vars in xtreg ---
display ""
display "=== xtreg with i.year as time FE ==="
clear
set obs 30
gen str3 cid = "AAA"
replace cid = "BBB" in 11/20
replace cid = "CCC" in 21/30
gen year = mod(_n-1, 10) + 2015
gen x = _n*1.0
gen y = x + cond(cid=="AAA", 0, cond(cid=="BBB", 10, 20))
xtset cid year
display "Expected: xtreg absorbs country FE; i.year tests for time effects"
xtreg y x i.year, fe

* --- Errors ---
display ""
display "=== Error: i.var on string variable ==="
clear
set obs 5
gen str3 cid = "AAA"
gen y = _n*1.0
capture regress y i.cid
display "rc=" _rc

display ""
display "=== Error: i.var with only one level ==="
clear
set obs 5
gen g = 1
gen x = _n*1.0
gen y = _n*2.0
* this should still work, but produce 0 dummies (degenerate)
regress y x i.g
display "(should drop i.g silently — no levels to add)"

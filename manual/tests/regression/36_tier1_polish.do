* Regression: Tier 1 polish.  Locks in:
*   - new statistical functions (normalden, lnnormal, lnnormalden)
*   - regex submatch (regexs) and regex replace (regexr)
*   - egen extensions (median, iqr, p25/p75, pc, rank, tag)
*   - egen multi-arg row functions (rowtotal, rowmean, rowmin, rowmax, rowmiss)
*   - custom number formats applied by `list`
*   - _b[name] / _se[name] macro access after estimation
*   - e(name) NOT substituted inside double-quoted strings

* --- statistical functions ---
clear
set obs 1
display "=== normalden ==="
display "normalden(0)       = " normalden(0)
display "normalden(0,1)     = " normalden(0,1)
display "normalden(1,0,2)   = " normalden(1,0,2)
display "lnnormal(0)        = " lnnormal(0)
display "lnnormal(-10)      = " lnnormal(-10)
display "lnnormalden(0)     = " lnnormalden(0)

* --- regex submatch ---
display ""
display "=== regex submatch ==="
gen s = "John Doe, age 42"
gen m = regexm(s, "([A-Za-z]+) ([A-Za-z]+).*age ([0-9]+)")
gen first = regexs(1)
gen last  = regexs(2)
gen age   = regexs(3)
list s m first last age

* --- regex replace ---
display ""
display "=== regexr replace ==="
gen replaced = regexr("hello world", "world", "tea")
list replaced

* --- egen extensions ---
display ""
display "=== egen median/iqr/percentile ==="
clear
set obs 10
gen x = _n*1.0
egen med = median(x)
egen iqr1 = iqr(x)
egen p10 = pc(x), p(10)
egen p90 = pc(x), p(90)
list x med iqr1 p10 p90 in 1/3

display ""
display "=== egen rank ==="
clear
set obs 5
gen z = 5 - _n
egen r = rank(z)
list z r

display ""
display "=== egen tag ==="
clear
set obs 6
gen g = mod(_n-1, 2)
egen tg = tag(g)
list g tg

display ""
display "=== egen row functions over multiple vars ==="
clear
set obs 3
gen a = _n
gen b = _n * 2
gen c = _n * 3
egen tot = rowtotal(a b c)
egen avg = rowmean(a b c)
egen rmin = rowmin(a b c)
egen rmax = rowmax(a b c)
list a b c tot avg rmin rmax

* --- custom format applied by list ---
display ""
display "=== custom format ==="
clear
set obs 3
gen pct = _n * 0.07654
format pct %6.3f
list pct

* --- _b/_se macros ---
display ""
display "=== _b[] and _se[] macros ==="
clear
set obs 100
gen x = _n*0.1
gen y = 2 + 3*x + mod(_n*7, 5) - 2
quietly regress y x
display "_b[x]      = " _b[x]
display "_se[x]     = " _se[x]
display "_b[_cons]  = " _b[_cons]
display "_se[_cons] = " _se[_cons]
display "t-stat of x = " _b[x] / _se[x]
gen pred = _b[_cons] + _b[x]*x
list x pred in 1/3

display ""
display "=== _b/_se with TS-op coefficient name ==="
clear
set obs 30
gen str3 cid = "AAA"
replace cid = "BBB" in 11/20
replace cid = "CCC" in 21/30
gen t = mod(_n-1, 10) + 2015
gen z = _n*1.0
xtset cid t
quietly regress z L.z
display "_b[L.z]      = " _b[L.z]
display "_se[L.z]     = " _se[L.z]

* --- quotes-aware e() substitution ---
display ""
display "=== e(N) not substituted inside double quotes ==="
clear
set obs 25
gen x = _n*1.0
gen y = _n*2.0
quietly regress y x
display "the string e(N) is literal; the value follows: " e(N)

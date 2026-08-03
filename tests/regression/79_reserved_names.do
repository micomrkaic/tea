* 79_reserved_names — Bug 44: tea accepted Stata-reserved words as
* variable names.  `gen _n = 3` created a variable shadowing the
* observation counter; `rename x if` planted a variable named `if`
* in the frame, poisoning every later if-clause parse.  Fix: Stata's
* reserved list (_all _b byte _coef _cons double float if in int
* long _n _N _pi _pred _rc _skip str# strL using with) is rejected
* r(198) at every user-facing creation surface: generate, egen,
* rename (all three forms), encode/decode/destring/tostring gen(),
* recode gen(), reshape long j() and reshape wide generated names.
* Note _merge is NOT reserved (merge creates it legitimately).
* Also pinned: `gen ln = ln(x)` is LEGAL, exactly as in Stata — the
* expression parser resolves ln( as the function (even with a space
* before the paren) and bare ln as the variable, after shadowing.
clear
set obs 3
capture gen _n = 3
display "gen _n rejected: " (_rc == 198)
capture gen int = 4
display "gen int rejected: " (_rc == 198)
capture gen byte = 9
display "gen byte rejected: " (_rc == 198)
capture gen using = 5
display "gen using rejected: " (_rc == 198)
capture gen _cons = 6
display "gen _cons rejected: " (_rc == 198)
capture gen str5 = "a"
display "gen str5 rejected: " (_rc == 198)
quietly gen x = 1
capture rename x if
display "rename to if rejected: " (_rc == 198)
capture egen _pi = mean(x)
display "egen _pi rejected: " (_rc == 198)
capture recode x (1=2), gen(with)
display "recode gen(with) rejected: " (_rc == 198)
gen int typed = 4
display "type-token form ok: " (typed[1] == 4)
clear
sysuse airline
quietly gen ln = ln(passengers)
display "gen ln legal (Stata parity): " (_rc == 0)
quietly gen z = ln(2.718281828459045)
display "fn resolves after shadow: " (abs(z[1] - 1) < 1e-9)
quietly gen w = ln
quietly summarize w
display "var ref resolves: " (abs(r(mean) - 5.542176) < 1e-5)
quietly gen q = ln (2.718281828459045)
display "spaced call is the function: " (abs(q[1] - 1) < 1e-9)

* 64_missing_args_defined — Apple-silicon round (Bug 35):
* casting a MISSING double to long is undefined behavior and diverges by
* ISA: x86 gives LONG_MIN, arm64's fcvtzs gives 0.  subinstr(s,f,t,.)
* therefore silently replaced NOTHING on Apple silicon while replacing
* all on x86 — same binary logic, opposite results.  Every numeric
* argument coercion in the function evaluator now checks missing first
* and implements Stata's documented semantics, so these outputs are
* ISA-independent by construction:
*   subinstr/subinword n=.  -> all occurrences (Stata-documented)
*   substr n1=. -> "";  substr(s,n,.) -> remainder of string
*   word(s,.) / char(.) -> ""
*   date functions of missing -> missing
display subinstr("aaa bbb aaa","aaa","X",.)
display subinstr("aaa bbb aaa","aaa","X",1)
display subinword("the cat and cats","cat","DOG",.)
display substr("hello",2,.)
display substr("hello",.,2)
display length(word("a b c",.))
display length(char(.))
display year(.)
display mdy(1,.,2020)
display dofq(.)
display yq(2020,.)
display qofd(.)

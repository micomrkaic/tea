* Regression: the string function family.
clear
set obs 1
gen s = "  Hello, World  "
display "upper: " upper("hello")
display "lower: " lower("WORLD")
display "proper: " proper("hello world")
display "trim: '" strtrim(s) "'"
display "ltrim: '" ltrim(s) "'"
display "rtrim: '" rtrim(s) "'"
display "itrim: '" itrim("  a   b  c  ") "'"
display "strreverse: " strreverse("abcde")
display "strpos: " strpos("hello world", "world")
display "strrpos: " strrpos("aaa", "a")
display "substr: " substr("hello", 2, 3)
display "subinstr: " subinstr("aaa bbb aaa", "aaa", "X", .)
display "subinword: " subinword("the cat and cats", "cat", "DOG", .)
display "word: " word("the quick fox", 2)
display "wordcount: " wordcount("a b c d e")
display "length: " length("hello")
display "char: " char(65)
display "real: " real("3.14")
display "string-fmt: " string(3.14159, "%5.2f")

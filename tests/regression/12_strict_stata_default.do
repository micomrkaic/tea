* Regression: strict-stata is the default; tea-only extensions are rejected.
* Bug: previously mkdir's 'recursive' option silently accepted (tea was not
* strict-Stata by default).  Now must error with rc=198.
* Note: this test runs in default mode (no --tea-extensions), which is strict.
capture mkdir /tmp/tea_strict_test_a/b/c, recursive
display "after attempted recursive mkdir"
* normal mkdir still works (no options)
mkdir /tmp/tea_strict_test_topdir
display "after non-recursive mkdir"
rmdir /tmp/tea_strict_test_topdir

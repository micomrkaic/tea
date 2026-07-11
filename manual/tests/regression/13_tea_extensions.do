* Regression: --tea-extensions enables tea-only extensions.
* This test runs with --tea-extensions (see 13_tea_extensions.flags).
mkdir /tmp/tea_ext_test_a/b/c, recursive
display "after recursive mkdir"
* tidy up — POSIX rmdir requires empty, so go bottom-up
rmdir /tmp/tea_ext_test_a/b/c
rmdir /tmp/tea_ext_test_a/b
rmdir /tmp/tea_ext_test_a
display "tidied up"

* 77_help — the thematic help system (v1.6.43).
* `help _check` is the drift guard: any with-help command missing
* from every theme prints "uncategorized: NAME" and exits rc 9, so a
* new command registered without a theme fails this golden loudly
* (the silent-failure doctrine applied to documentation).  The topic
* views are goldened (they carry no version string, so the golden is
* release-stable); `help` itself is exercised via capture for rc only
* since its header carries the version.
help _check
help statespace
help panel
capture help
display "help rc: " _rc
capture help estimation
display "topic rc: " _rc
capture help nosuchtopic
display "unknown is not a topic, falls through to unknown-cmd rc: " _rc

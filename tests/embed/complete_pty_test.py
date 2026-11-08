#!/usr/bin/env python3
"""tea completion gate: drive ./tea through a REAL pty and press TAB.

The GUI and browser complete through tea_complete() (same TU as the
dispatch table); the native REPL completes through GNU readline in
interp.c.  That path shipped a segfault (v1.6.57: a stale 4-field
shadow of Disp walked TABLE misaligned once Disp had 5 fields) because
no gate ever pressed TAB in a terminal.  This one does:
  1. first-word completion:  'summar' + TAB -> 'summarize '
  2. command arg of help:    'help regre' + TAB -> completes; output
     shows the regress help after Enter
  3. variable completion:    after sysuse airline, 'list passen' + TAB
  4. the session survives all of it
"""
import os, pty, sys, time, select, fcntl, struct, termios

TEA = sys.argv[1] if len(sys.argv) > 1 else "./tea"

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv(TEA, [TEA])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

def read_until(needle, timeout=8.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                c = os.read(fd, 4096)
            except OSError:
                break
            if not c:
                break
            buf += c
            if needle in buf:
                return buf, True
    return buf, False

def fail(msg, buf=b""):
    sys.stderr.write("complete pty test FAIL: %s\n" % msg)
    sys.stderr.write("--- last output ---\n%s\n" % buf[-300:].decode("utf-8", "replace"))
    try: os.kill(pid, 9)
    except OSError: pass
    sys.exit(1)

def alive():
    return os.waitpid(pid, os.WNOHANG) == (0, 0)

buf, ok = read_until(b"type help")
if not ok: fail("no banner", buf)

# 1. first-word completion (unique): summar<TAB> -> summarize
os.write(fd, b"summar\t")
buf, ok = read_until(b"summarize", 5.0)
if not alive(): fail("CRASHED on first-word TAB", buf)
if not ok: fail("first-word TAB did not complete 'summar'", buf)
os.write(fd, b"\x15")            # ^U: clear the line

# 2. help's second word: help regre<TAB> -> regress; Enter shows help
os.write(fd, b"help regre\t")
buf, ok = read_until(b"regress", 5.0)
if not alive(): fail("CRASHED on 'help <cmd>' TAB (the v1.6.57 segfault)", buf)
if not ok: fail("'help regre' TAB did not complete", buf)
os.write(fd, b"\n")
buf, ok = read_until(b"OLS", 5.0)
if not ok: fail("completed help line did not run", buf)

# 3. variable completion after a dataset is loaded
os.write(fd, b"sysuse airline\n")
read_until(b"airline", 5.0)
os.write(fd, b"list passen\t")
buf, ok = read_until(b"passengers", 5.0)
if not alive(): fail("CRASHED on variable TAB", buf)
if not ok: fail("variable TAB did not complete 'passen'", buf)
os.write(fd, b"\x15")

# 4. still alive and responsive
os.write(fd, b"display 41+1\n")
buf, ok = read_until(b"42", 5.0)
if not ok: fail("session dead after completions", buf)

os.write(fd, b"exit\n")
time.sleep(0.3)
try: os.kill(pid, 9)
except OSError: pass
print("complete pty test: PASS")

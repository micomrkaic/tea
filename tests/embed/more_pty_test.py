#!/usr/bin/env python3
"""tea pager gate: drive ./tea through a REAL pty (the pipe-based
harness can never exercise `more` — its isatty guard is the point) and
prove the full contract:
  1. `set more on` + a long command pauses at --more--
  2. space advances a full page
  3. q aborts the remainder (--Break--) and the next command runs
  4. `set more off` turns it back off (no pause on long output)
"""
import os, pty, sys, time, select

TEA = sys.argv[1] if len(sys.argv) > 1 else "./tea"

def spawn():
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm"
        os.execv(TEA, [TEA])
    return pid, fd

def read_until(fd, needle, timeout=8.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if needle in buf:
                return buf, True
    return buf, False

def send(fd, s):
    os.write(fd, s.encode())

def fail(msg, buf=b""):
    sys.stderr.write("more pty test FAIL: %s\n" % msg)
    tail = buf[-400:].decode("utf-8", "replace")
    sys.stderr.write("--- last output ---\n%s\n" % tail)
    sys.exit(1)

pid, fd = spawn()
# make the pty small so a modest listing pages deterministically
import fcntl, struct, termios
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 80, 0, 0))

buf, ok = read_until(fd, b".")
if not ok: fail("no prompt", buf)

send(fd, "set more on\n")
read_until(fd, b".")

# 1. long output must pause
send(fd, "help\n")
buf, ok = read_until(fd, b"--more--")
if not ok: fail("no --more-- pause on help", buf)

# 2. space advances a page (another pause must follow: help is long)
send(fd, " ")
buf, ok = read_until(fd, b"--more--")
if not ok: fail("no second page pause after space", buf)

# 3. q aborts with --Break-- and the session lives on
send(fd, "q")
buf, ok = read_until(fd, b"--Break--")
if not ok: fail("q did not abort with --Break--", buf)
send(fd, "display 41+1\n")
buf, ok = read_until(fd, b"42")
if not ok: fail("session dead after q", buf)

# 4. set more off: help must complete with no pause
send(fd, "set more off\n")
read_until(fd, b".")
send(fd, "help\n")
buf, ok = read_until(fd, b"Native statements", timeout=8.0)
if not ok: fail("help incomplete with more off", buf)
if b"--more--" in buf: fail("paused despite set more off", buf)

send(fd, "exit\n")
time.sleep(0.3)
try: os.kill(pid, 9)
except OSError: pass
print("more pty test: PASS")

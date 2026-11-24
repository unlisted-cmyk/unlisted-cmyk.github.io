#!/usr/bin/env python3
"""Tests image/common/logl-session (the console of a Log computer) inside a pseudo-terminal with plain `sh`. Linux only.
Usage: python3 tests/session_test.py ./logl"""
import os, pty, select, shutil, subprocess, sys, tempfile, time
binary = os.path.abspath(sys.argv[1]); here = os.path.dirname(os.path.abspath(__file__))
script = os.path.join(here, "..", "image", "common", "logl-session")
ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
def session(env_extra, inputs, wait=1.2, total=15):
    m, s = pty.openpty()
    env = dict(os.environ); env.update(env_extra)
    p = subprocess.Popen(["sh", script], stdin=s, stdout=s, stderr=s, close_fds=True, env=env); os.close(s); out = b""
    def drain(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([m], [], [], 0.1)
            if r:
                try: d = os.read(m, 4096)
                except OSError: return
                if not d: return
                out += d
    drain(wait)
    for i in inputs: os.write(m, i.encode()); drain(0.7)
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < total: drain(0.2)
    exited = p.poll() is not None
    if not exited: p.kill()
    return out.decode(errors="replace"), exited, p.returncode
tmp = tempfile.mkdtemp(); home = os.path.join(tmp, "home"); boot = os.path.join(tmp, "boot"); os.makedirs(boot)
base = {"LOGL_BIN": binary, "LOGL_HOME": home, "LOGL_AUTORUN_DIR": boot, "LOGL_FLAGS": "--no-window"}
try:
    # 1. autorun, then the interactive terminal, then :quit ends the session script (init would start it again)
    open(os.path.join(boot, "autorun.log+"), "w").write('<log+>\nPrint("autorun corriendo")\n')
    o, ex, rc = session(base, ['Print("hola desde la terminal")\n', ':quit\n'])
    check("autorun program ran first", "[autorun]" in o and "autorun corriendo" in o)
    check("then the Log terminal opened (prompt + a command works)", "log+>" in o and "hola desde la terminal" in o)
    check("autorun came before the prompt", o.find("autorun corriendo") < o.find("log+>"))
    check(":quit ends the session script cleanly (init restarts it)", ex and rc == 0 and "[Log ended with code 0]" in o)
    # 2. no autorun: straight to the terminal
    os.remove(os.path.join(boot, "autorun.log+"))
    o, ex, rc = session(base, ['Print("directo")\n', ':quit\n'])
    check("without autorun it goes straight to the terminal", "[autorun]" not in o and "directo" in o and "Log OS - logl" in o)
    # 3. maintenance shell door
    open(os.path.join(boot, "logl-shell"), "w").close()
    o, ex, rc = session(base, ['echo SHELL_OK_$((40+2))\n', 'exit\n', ':quit\n'], wait=1.5)
    check("logl-shell file opens a maintenance shell once", "SHELL_OK_42" in o and "log+>" in o)
    check("the logl-shell file is removed (next boot is normal)", not os.path.exists(os.path.join(boot, "logl-shell")))
    # 4. broken installation must not hang or spin
    bad = dict(base); bad["LOGL_BIN"] = os.path.join(tmp, "no-such-logl")
    t0 = time.time(); o, ex, rc = session(bad, [], wait=0.5, total=10)
    check("missing logl binary: the script ends by itself (no hang)", ex and time.time() - t0 < 9)
    # 5. LOGL_FLAGS from /etc/logl.conf style default is documented; unset variable uses the appliance default flags
    src = open(script).read(); check("appliance default flags are --hw --allow-all --no-window", "--hw --allow-all --no-window" in src)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("session test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

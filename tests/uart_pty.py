#!/usr/bin/env python3
"""Real UART test (termios + poll) against a pseudo-terminal. Linux/macOS only.  Usage: python3 tests/uart_pty.py ./logl"""
import os, pty, subprocess, sys, threading, tempfile
binary = sys.argv[1]
m, s = pty.openpty(); slave = os.ttyname(s)
prog = f'''<log+>
UART:Open("Port;{slave}","Baud;115200","Id;p")
UART:Write("Id;p","Text;ping")
Var:t("Text;")
UART:Read("Id;p","Count;4","Timeout;3000","As;Text","Into;t")
Print("recibido=[{{t}}]")
Var:l("List;")
UART:Read("Id;p","Count;8","Timeout;200","Into;l")
Print("extra={{l.length}}")
UART:Close("Id;p")
'''
path = os.path.join(tempfile.gettempdir(), "uart_pty_test.log+")
open(path, "w").write(prog)
got = bytearray()
def pump():
    while b"ping" not in got:
        got.extend(os.read(m, 64))
    os.write(m, b"pong")
threading.Thread(target=pump, daemon=True).start()
r = subprocess.run([binary, path, "--no-window", "--plain", "--no-wait", "--hw", "--allow-pins"], capture_output=True, text=True, timeout=30)
ok = "recibido=[pong]" in r.stdout and "extra=0" in r.stdout and bytes(got) == b"ping" and r.returncode == 0
print(r.stdout)
print("UART pty test:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

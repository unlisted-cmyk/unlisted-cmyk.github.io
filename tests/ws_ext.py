#!/usr/bin/env python3
"""WebSocket test with independent clients and test vectors from RFC 6455.  Usage: python3 tests/ws_ext.py ./logl"""
import os, signal, socket, struct, subprocess, sys, tempfile, time
binary = sys.argv[1]
prog = '''<log+>
Var:p("Value;0")
On:Net("WsOpen")
{
 Print("abierto {req_path}")
 Net:Ws("Send","Id;{net_id}","Text;bienvenido")
}
On:Net("WsMessage")
{
 Net:Ws("Send","Id;{net_id}","Text;eco:{net_data}")
 If(net_data=='todos')
 {
  Net:Ws("Broadcast","Text;difusion")
 }
}
On:Net("WsClose")
{
 Print("cerrado")
}
Net:Http("Listen","Port;0","Into;p")
Print("PORT {p}")
'''
path = os.path.join(tempfile.gettempdir(), "ws_ext_test.log+"); open(path, "w").write(prog)
p = subprocess.Popen([binary, path, "--no-window", "--plain", "--allow-net"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
port = None
for _ in range(50):
    line = p.stdout.readline()
    if line.startswith("PORT "): port = int(line.split()[1]); break
ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
check("server started", port is not None)

def recv_n(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b
def read_frame(s):
    b0, b1 = recv_n(s, 2); ln = b1 & 127
    if ln == 126: ln = struct.unpack(">H", recv_n(s, 2))[0]
    elif ln == 127: ln = struct.unpack(">Q", recv_n(s, 8))[0]
    return b0 & 15, recv_n(s, ln)
def masked(op, payload, key=b"\x01\x02\x03\x04", fin=True):
    h = bytes([(0x80 if fin else 0) | op]); n = len(payload)
    if n < 126: h += bytes([0x80 | n])
    elif n < 65536: h += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: h += bytes([0x80 | 127]) + struct.pack(">Q", n)
    return h + key + bytes(c ^ key[i % 4] for i, c in enumerate(payload))
def connect(path_="/chat", key="dGhlIHNhbXBsZSBub25jZQ=="):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path_, key)).encode())
    h = b""
    while b"\r\n\r\n" not in h: h += s.recv(1)
    return s, h.decode()
try:
    s, h = connect()
    check("101 Switching Protocols", h.startswith("HTTP/1.1 101"))
    check("RFC 6455 accept key s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in h)
    check("welcome frame", read_frame(s) == (1, b"bienvenido"))
    s.sendall(bytes.fromhex("818537fa213d7f9f4d5158"))                       # RFC 6455 5.7: masked "Hello"
    check("RFC masked 'Hello' -> echo", read_frame(s) == (1, b"eco:Hello"))
    s.sendall(masked(1, b"Hel", fin=False) + masked(0, b"lo", key=b"\x0a\x0b\x0c\x0d"))   # fragmented message
    check("fragmented message reassembled", read_frame(s) == (1, b"eco:Hello"))
    s.sendall(masked(9, b"Hi"))
    check("ping -> pong with the same payload", read_frame(s) == (10, b"Hi"))
    big = bytes((i * 7) % 26 + 97 for i in range(300)); s.sendall(masked(1, big))
    check("300 bytes (16-bit length) echoed", read_frame(s) == (1, b"eco:" + big))
    big2 = bytes((i * 5) % 26 + 97 for i in range(70000)); s.sendall(masked(1, big2))
    op, d = read_frame(s) if False else (None, None)
    s2, h2 = connect("/otro")
    check("second client connects", h2.startswith("HTTP/1.1 101") and read_frame(s2) == (1, b"bienvenido"))
    # the 70000-byte message was sent above; drain its echo from client 1 first
    s.settimeout(10); op, d = read_frame(s); check("70000 bytes (64-bit length) echoed", op == 1 and d == b"eco:" + big2)
    s.sendall(masked(1, b"todos"))
    check("broadcast reaches client 1 (after its echo)", read_frame(s) == (1, b"eco:todos") and read_frame(s) == (1, b"difusion"))
    check("broadcast reaches client 2", read_frame(s2) == (1, b"difusion"))
    s2.sendall(masked(8, b"\x03\xe8"))
    check("close handshake: server echoes the close frame", read_frame(s2) == (8, b"\x03\xe8"))
    try: eof = s2.recv(10) == b""
    except Exception: eof = True
    check("connection closed after close frame", eof)
    bad, hb = connect("/bad"); read_frame(bad); bad.sendall(b"\x81\x05Hello")   # NOT masked: protocol error
    try: bad.settimeout(5); r = bad.recv(10)
    except Exception: r = b""
    check("unmasked client frame -> server closes", r == b"")
    g, hg = connect("/garbage"); read_frame(g); g.sendall(b"\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff")
    try: g.settimeout(5); r = g.recv(10)
    except Exception: r = b""
    check("garbage / absurd length -> server closes", r == b"")
    s.sendall(masked(1, b"sigue vivo?"))
    check("server still serves client 1", read_frame(s) == (1, b"eco:sigue vivo?"))
    plain = socket.create_connection(("127.0.0.1", port), timeout=5); plain.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("plain HTTP (no upgrade) gets a normal answer", plain.recv(100).startswith(b"HTTP/1.1 404"))
    check("process still running", p.poll() is None)
finally:
    p.send_signal(signal.SIGTERM)
    try: p.wait(timeout=10)
    except Exception: p.kill()
print("WebSocket external test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

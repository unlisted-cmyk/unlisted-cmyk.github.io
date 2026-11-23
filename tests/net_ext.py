#!/usr/bin/env python3
"""Real HTTP clients against the logl server (parallel requests, big bodies, garbage, a stalled client).  Usage: python3 tests/net_ext.py ./logl"""
import http.client, os, signal, socket, subprocess, sys, tempfile, threading, time
binary = sys.argv[1]
prog = '''<log+>
Var:p("Value;0")
Net:Http("Listen","Port;0","Into;p")
Var:n("Value;0")
On:Net("Request")
{
 n += 1
 If(req_path=='/eco')
 {
  Net:Respond("Type;text/plain","Text;{req_method}:{req_body.length}:{req_query}")
 }
 Else
 {
  Net:Respond("Status;404","Text;no")
 }
}
Print("PORT {p}")
'''
path = os.path.join(tempfile.gettempdir(), "net_ext_test.log+"); open(path, "w").write(prog)
p = subprocess.Popen([binary, path, "--no-window", "--plain", "--allow-net"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
port = None
for _ in range(50):
    line = p.stdout.readline()
    if line.startswith("PORT "): port = int(line.split()[1]); break
ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and cond
check("server started", port is not None)
def req(method, target, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, target, body=body); r = c.getresponse(); d = r.read().decode(); c.close(); return r.status, d
try:
    res = []
    def worker(i):
        try: res.append(req("GET", "/eco?i=%d" % i))
        except Exception as e: res.append(("ERR", str(e)))
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(20)]
    [t.start() for t in ts]; [t.join() for t in ts]
    check("20 parallel GETs", len(res) == 20 and all(r[0] == 200 and r[1].startswith("GET:0:i=") for r in res))
    s, d = req("POST", "/eco?big", "x" * 200000)
    check("POST 200 KB body", s == 200 and d == "POST:200000:big")
    s, d = req("GET", "/nada"); check("404 path", s == 404 and d == "no")
    # The server refuses the body (limit 1 MB) and closes while the client is still sending, so a correct client sees EITHER a 400
    # answer OR a connection reset / broken pipe (which one depends on the OS timing). Both mean "rejected"; what must NOT happen is a 200.
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        c.request("POST", "/eco", body="y" * 2000000); r = c.getresponse(); rejected = r.status in (400, 413)
    except (ConnectionError, BrokenPipeError, OSError, http.client.HTTPException):
        rejected = True
    check("2 MB body rejected", rejected)
    c.close()
    g = socket.create_connection(("127.0.0.1", port)); g.sendall(b"\x00\xff\x01garbage\r\n\r\n"); g.settimeout(5)
    try: data = g.recv(200)
    except Exception: data = b""
    g.close(); check("garbage gets an answer or a clean close", data == b"" or data.startswith(b"HTTP/1.1 400"))
    slow = socket.create_connection(("127.0.0.1", port)); slow.sendall(b"GET /eco HTTP/1.1\r\nHost: x\r\n")   # never finishes the head
    time.sleep(0.2)
    s, d = req("GET", "/eco?after_slow")   # the poller reads one request at a time (2 s timeout): must still answer afterwards
    check("server alive after a stalled client", s == 200 and d.endswith("after_slow"))
    slow.close()
    check("process still running", p.poll() is None)
finally:
    p.send_signal(signal.SIGTERM)
    try: p.wait(timeout=10)
    except Exception: p.kill()
print("NET external test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

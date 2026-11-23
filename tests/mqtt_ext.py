#!/usr/bin/env python3
"""MQTT 3.1.1 test: a scripted fake broker checks the EXACT bytes logl sends (packet layouts from the MQTT 3.1.1 spec).  Usage: python3 tests/mqtt_ext.py ./logl"""
import os, socket, subprocess, sys, tempfile, threading, time
binary = sys.argv[1]
ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
def run(prog, steps, tmo=20):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", 0)); srv.listen(1); port = srv.getsockname()[1]
    res = {"err": None}
    def broker():
        try:
            srv.settimeout(10); conn, _ = srv.accept(); conn.settimeout(8)
            for st in steps:
                if st[0] == "expect":
                    d = b""
                    while len(d) < len(st[1]):
                        c = conn.recv(len(st[1]) - len(d))
                        if not c: break
                        d += c
                    if d != st[1]: res["err"] = "expected %s... got %s... (%d vs %d bytes)" % (st[1][:24].hex(), d[:24].hex(), len(st[1]), len(d)); break
                elif st[0] == "send": conn.sendall(st[1])
                elif st[0] == "sleep": time.sleep(st[1])
                elif st[0] == "close": conn.close(); return
            conn.close()
        except Exception as e: res["err"] = "broker exception: %r" % (e,)
    th = threading.Thread(target=broker, daemon=True); th.start()
    path = os.path.join(tempfile.gettempdir(), "mqtt_ext_test.log+"); open(path, "w").write(prog.replace("@PORT@", str(port)))
    r = subprocess.run([binary, path, "--no-window", "--plain", "--allow-net"], capture_output=True, text=True, timeout=tmo)
    th.join(15); srv.close()
    return r.stdout, res["err"]

# --- A: connect, subscribe, receive QoS0 + QoS1, publish (plain, retained, 5000 bytes), unsubscribe, disconnect
big = b"z" * 5000
progA = '''<log+>
On:Net("Message")
{
 Print("msg {mqtt_topic} = {mqtt_msg}")
}
Mqtt:Connect("Id;m","Host;127.0.0.1","Port;@PORT@","Client;logl-test","Keepalive;30")
Mqtt:Subscribe("Id;m","Topic;a/b")
Wait("Ms;400")
Mqtt:Publish("Id;m","Topic;a/b","Text;hi")
Mqtt:Publish("Id;m","Topic;t/r","Text;x","Retain;1")
Var:big("Text;")
Var:i("Value;0")
While(i<500)
{
 big += 'zzzzzzzzzz'
 i += 1
}
Mqtt:Publish("Id;m","Topic;big","Text;{big}")
Mqtt:Unsubscribe("Id;m","Topic;a/b")
Wait("Ms;300")
Mqtt:Close("Id;m")
Stop
'''
stepsA = [
 ("expect", bytes.fromhex("1015" "00044d515454" "04" "02" "001e" "0009" "6c6f676c2d74657374")), ("send", bytes.fromhex("20020000")),
 ("expect", bytes.fromhex("8208" "0001" "0003612f62" "00")), ("send", bytes.fromhex("90030001" "00")),
 ("send", bytes.fromhex("300a" "0003612f62" "68656c6c6f")),                  # QoS 0 PUBLISH a/b "hello"
 ("send", bytes.fromhex("320c" "0003612f62" "0007" "68656c6c6f")),           # QoS 1 PUBLISH (packet id 7)
 ("expect", bytes.fromhex("40020007")),                                      # PUBACK
 ("expect", bytes.fromhex("3007" "0003612f62" "6869")),
 ("expect", bytes.fromhex("3106" "0003742f72" "78")),                        # retain flag
 ("expect", bytes([0x30, 0x8d, 0x27]) + bytes.fromhex("0003" "626967") + big),   # remaining length 5005 = 8d 27
 ("expect", bytes.fromhex("a207" "0002" "0003612f62")),                      # UNSUBSCRIBE, packet id 2
 ("expect", bytes.fromhex("e000")),                                          # DISCONNECT
]
try:
    out, err = run(progA, stepsA)
    check("A: every packet matches the MQTT 3.1.1 byte layout", err is None)
    if err: print("     ", err)
    check("A: QoS 0 and QoS 1 messages delivered to On:Net(\"Message\")", out.count("msg a/b = hello") == 2)
except Exception as e: check("A: ran (%r)" % (e,), False)

# --- B: user + password, broker refuses (code 5)
progB = '''<log+>
Try
{
 Mqtt:Connect("Id;m","Host;127.0.0.1","Port;@PORT@","Client;c1","User;u","Pass;p","Keepalive;0")
 Print("conecto?!")
}
Catch:e
{
 Print("{e}")
}
'''
stepsB = [("expect", bytes.fromhex("1014" "00044d515454" "04" "c2" "0000" "00026331" "000175" "000170")), ("send", bytes.fromhex("20020005"))]
try:
    out, err = run(progB, stepsB)
    check("B: CONNECT with user/pass flags (0xC2) is byte-exact", err is None)
    if err: print("     ", err)
    check("B: refusal reported", "not authorized" in out and "conecto" not in out)
except Exception as e: check("B: ran (%r)" % (e,), False)

# --- C: keepalive: logl must send PINGREQ (c0 00) by itself
progC = '''<log+>
Mqtt:Connect("Id;m","Host;127.0.0.1","Port;@PORT@","Client;k","Keepalive;2")
Wait("Ms;1500")
Mqtt:Close("Id;m")
Stop
'''
stepsC = [("expect", bytes.fromhex("100d" "00044d515454" "04" "02" "0002" "00016b")), ("send", bytes.fromhex("20020000")), ("expect", bytes.fromhex("c000")), ("send", bytes.fromhex("d000")), ("expect", bytes.fromhex("e000"))]
try:
    out, err = run(progC, stepsC)
    check("C: PINGREQ sent automatically at half the keepalive", err is None)
    if err: print("     ", err)
except Exception as e: check("C: ran (%r)" % (e,), False)

# --- D: the broker drops the connection -> On:Net("Closed")
progD = '''<log+>
On:Net("Closed")
{
 Print("broker se fue: {net_id}")
}
Mqtt:Connect("Id;m","Host;127.0.0.1","Port;@PORT@","Client;d","Keepalive;0")
Wait("Ms;600")
Try
{
 Mqtt:Publish("Id;m","Topic;x","Text;y")
}
Catch:e
{
 Print("{e}")
}
Stop
'''
stepsD = [("expect", bytes.fromhex("100d" "00044d515454" "04" "02" "0000" "000164")), ("send", bytes.fromhex("20020000")), ("sleep", 0.1), ("close",)]
try:
    out, err = run(progD, stepsD)
    check("D: broker disconnect raises On:Net(\"Closed\")", "broker se fue: m" in out)
    check("D: Publish after the drop gives a clear error", "is not connected" in out or "send failed" in out)
except Exception as e: check("D: ran (%r)" % (e,), False)

# --- E: a wildcard in Publish and a missing broker are errors, not crashes
progE = '''<log+>
Try
{
 Mqtt:Connect("Id;m","Host;127.0.0.1","Port;1","Timeout;1500")
}
Catch:e
{
 Print("{e}")
}
Try
{
 Mqtt:Publish("Id;nope","Topic;a/#","Text;x")
}
Catch:e2
{
 Print("{e2}")
}
'''
r = subprocess.run([binary, os.path.join(tempfile.gettempdir(), "e.log+")], capture_output=True, text=True) if False else None
p = os.path.join(tempfile.gettempdir(), "mqtt_e.log+"); open(p, "w").write(progE)
r = subprocess.run([binary, p, "--no-window", "--plain", "--allow-net"], capture_output=True, text=True, timeout=20)
check("E: connection refused is a clean error", "could not connect" in r.stdout)
check("E: Publish on an unknown Id is a clean error", "is not connected" in r.stdout)
print("MQTT external test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

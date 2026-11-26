#!/usr/bin/env python3
"""Tests of the SIMULATED parts of the simulator (things logl cannot compare in the same program): simulated WebSocket browsers, MQTT broker,
external HTTP/TCP clients of the panel, PWM display, pin events from a circuit button.   Usage: python3 tests/sim_features.py path/to/index.html"""
import sys, os, time
from playwright.sync_api import sync_playwright
sim = os.path.abspath(sys.argv[1]); ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
with sync_playwright() as pw:
    br = pw.chromium.launch(); pg = br.new_page(); errs = []
    pg.on("pageerror", lambda e: errs.append(str(e))); pg.goto("file://" + sim); pg.wait_for_timeout(600)
    def out(): return pg.evaluate("[...OL.children].map(d=>d.textContent).join('\\n')")
    def start(src, wait_for=None, tmo=6):
        pg.evaluate("([s]) => { files['t.log+'] = s; window.__p = runFile('t.log+'); }", [src])
        t0 = time.time()
        while time.time() - t0 < tmo:
            o = out()
            if wait_for and wait_for in o: return o
            if "ERROR" in pg.evaluate("document.querySelector('#os').textContent") and not wait_for: return o
            pg.wait_for_timeout(30)
        return out()
    def stop(): pg.evaluate("stopRun()"); pg.wait_for_timeout(250)
    def wait_out(text, tmo=4):
        t0 = time.time()
        while time.time() - t0 < tmo:
            if text in out(): return True
            pg.wait_for_timeout(30)
        return False

    # ---------------- WebSocket (simulated browsers)
    start('''<log+>
Var:p("Value;0")
On:Net("WsOpen")
{
 Net:Ws("Send","Id;{net_id}","Text;hola {req_path}")
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
 Print("cerrado {net_id}")
}
Net:Http("Listen","Port;0","Into;p")
Print("listo")
''', "listo")
    a = pg.evaluate("SIM21.wsConnect('/chat')"); b = pg.evaluate("SIM21.wsConnect('/otro')"); pg.wait_for_timeout(150)
    check("WebSocket: WsOpen handler ran and sent a welcome", pg.evaluate("([a])=>SIM21.wsLog(a)", [a]) == ["< hola /chat"])
    pg.evaluate("([a])=>SIM21.wsSend(a,'ping')", [a]); pg.wait_for_timeout(150)
    check("WebSocket: WsMessage echo", pg.evaluate("([a])=>SIM21.wsLog(a)", [a])[-1] == "< eco:ping")
    pg.evaluate("([a])=>SIM21.wsSend(a,'todos')", [a]); pg.wait_for_timeout(200)
    check("WebSocket: Broadcast reaches every client", pg.evaluate("([a])=>SIM21.wsLog(a)", [a])[-1] == "< difusion" and pg.evaluate("([b])=>SIM21.wsLog(b)", [b])[-1] == "< difusion")
    pg.evaluate("([a])=>SIM21.wsClose(a)", [a]); check("WebSocket: WsClose handler ran", wait_out("cerrado " + a))
    stop()
    start('<log+>\nPrint("sin manejadores")\n', "sin manejadores"); check("WebSocket: refused (null) when the program has no WsOpen/WsMessage handler", pg.evaluate("SIM21.wsConnect('/')") is None)

    # ---------------- HTTP: external request from the panel to the program's server
    start('''<log+>
Var:p("Value;0")
On:Net("Request")
{
 If(req_path=='/vacio')
 {
 }
 Else
 {
  Net:Respond("Status;201","Text;{req_method} {req_path} q={req_query} b={req_body}")
 }
}
Net:Http("Listen","Port;0","Into;p")
Print("listo")
''', "listo")
    r = pg.evaluate("SIM21.httpRequest('POST','/x?a=1','cuerpo')"); check("HTTP: external request answered by Net:Respond", r == {"status": 201, "body": "POST /x q=a=1 b=cuerpo"})
    r = pg.evaluate("SIM21.httpRequest('GET','/vacio','')"); check("HTTP: handler that does not answer gives 204", r["status"] == 204)
    pg.click("#n21h", force=True) if pg.evaluate("!document.querySelector('#n21').hidden") else None
    check("panel 'Network (simulated)' is shown when the program uses the network", pg.evaluate("!document.querySelector('#n21').hidden"))
    pg.fill("#n21p", "/panel?z=9"); pg.select_option("#n21m", "GET"); pg.click("#n21h"); pg.wait_for_timeout(200)
    check("panel: the HTTP form works", "201 GET /panel q=z=9" in pg.inner_text("#n21ho"))
    stop()
    start('<log+>\nOn:Net("Request")\n{\n Net:Respond("Text;x")\n}\nPrint("listo")\n', "listo")
    check("HTTP: no listener -> no server answers", pg.evaluate("SIM21.httpRequest('GET','/','')")["status"] == 0); stop()

    # ---------------- TCP: external client of the panel against a program's server (Data/Closed events)
    start('''<log+>
Var:p("Value;0")
Net:Tcp("Listen","Id;srv","Port;7000")
On:Net("Data")
{
 Print("servidor: {net_data}")
 Net:Tcp("Send","Id;{net_id}","Text;ECO:{net_data}")
}
On:Net("Closed")
{
 Print("cerrada {net_id}")
}
Print("listo")
''', "listo")
    c = pg.evaluate("SIM21.tcpConnect(7000)"); check("TCP: the panel client connects to the listening port", c is not None)
    check("TCP: connection to a closed port is refused", pg.evaluate("SIM21.tcpConnect(7001)") is None)
    pg.evaluate("([c])=>SIM21.tcpSend(c,'hola')", [c]); check("TCP: Data event ran", wait_out("servidor: hola"))
    pg.wait_for_timeout(100); check("TCP: the program's reply reaches the panel client", pg.evaluate("([c])=>SIM21.tcpLog(c)", [c]) == ["< ECO:hola"])
    pg.evaluate("([c])=>SIM21.tcpClose(c)", [c]); check("TCP: Closed event when the client closes", wait_out("cerrada srv."))
    stop()

    # ---------------- MQTT: one virtual broker shared by every connection of the program and the panel
    start('''<log+>
On:Net("Message")
{
 Print("{net_id} {mqtt_topic} = {mqtt_msg}")
}
Mqtt:Connect("Id;a","Host;cualquier.broker","Port;1883")
Mqtt:Connect("Id;b","Host;otro","Client;x","User;u","Pass;p")
Mqtt:Publish("Id;a","Topic;casa/luz","Text;ON","Retain;1")
Mqtt:Subscribe("Id;b","Topic;casa/+")
Mqtt:Subscribe("Id;a","Topic;#")
Mqtt:Publish("Id;b","Topic;casa/temp","Text;21")
Mqtt:Unsubscribe("Id;a","Topic;#")
Mqtt:Publish("Id;b","Topic;casa/temp","Text;22")
Print("fin")
''', "fin")
    pg.wait_for_timeout(300); o = out()
    check("MQTT: retained message delivered when subscribing (+ wildcard)", "b casa/luz = ON" in o and "a casa/luz = ON" in o)
    check("MQTT: a publish reaches every matching subscriber", "b casa/temp = 21" in o and "a casa/temp = 21" in o)
    check("MQTT: Unsubscribe stops delivery", "a casa/temp = 22" not in o and "b casa/temp = 22" in o)
    pg.evaluate("SIM21.mqttPublish('casa/externo','hola',false)"); pg.wait_for_timeout(200); check("MQTT: publish from the panel (another client)", "b casa/externo = hola" in out())
    stop()
    o = start('''<log+>
Try
{
 Mqtt:Publish("Id;nope","Topic;a","Text;x")
}
Catch:e
{
 Print("{e}")
}
Mqtt:Connect("Id;m","Host;h")
Try
{
 Mqtt:Publish("Id;m","Topic;a/#","Text;x")
}
Catch:e2
{
 Print("{e2}")
}
Try
{
 Mqtt:Connect("Id;z","Host;h","Pass;p")
}
Catch:e3
{
 Print("{e3}")
}
Print("fin")
''', "fin")
    check("MQTT: clear errors (not connected / wildcard in Publish / Pass without User)", "Mqtt: 'nope' is not connected" in o and "cannot contain + or #" in o and "Mqtt: Pass needs User" in o); stop()

    # ---------------- PWM in the Circuits mode is tested in tests/sim_circuit_pwm.py (real circuit solver)
    # pin() of a PWM pin follows the clock; duty 100 is always high, 0 always low (same as logl)
    o = start('<log+>\nPin:13("PWM;50","Duty;100")\nVar:s("Value;0")\nFor:i(0,50)\n{\n s += pin(13)\n}\nPrint("s={s}")\n', "s=")
    check("pin() reads 1 every time with duty 100", "s=50" in o)

    # ---------------- a button of the Circuits mode (or the panel) raises On:Pin; handlers run in order
    start('''<log+>
Var:n("Value;0")
On:Pin(5)("Rising")
{
 n += 1
 Print("subio {n}")
}
On:Pin(5)("Falling")
{
 Print("bajo")
}
Print("listo")
''', "listo")
    pg.evaluate("SIM21.pinInput(5,1)"); pg.wait_for_timeout(120); pg.evaluate("SIM21.pinInput(5,1)"); pg.evaluate("SIM21.pinInput(5,0)"); pg.wait_for_timeout(120); pg.evaluate("SIM21.pinInput(5,1)"); pg.wait_for_timeout(150)
    o = out(); check("On:Pin from a circuit input: Rising twice, Falling once, no event without a change", o.count("subio") == 2 and o.count("bajo") == 1 and "subio 2" in o); stop()
    br.close()
if errs: print("JS errors in the page:", errs[:3]); ok = False
print("simulator features:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

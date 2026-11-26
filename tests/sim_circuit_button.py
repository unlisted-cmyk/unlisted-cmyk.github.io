#!/usr/bin/env python3
"""Circuits mode: pressing a push button wired to an input pin raises On:Pin events (the real solver path, not SIM21.pinInput).
Usage: python3 tests/sim_circuit_button.py index.html"""
import os, sys, time
from playwright.sync_api import sync_playwright
sim = os.path.abspath(sys.argv[1]); ok = True
def check(name, cond):
    global ok; print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
with sync_playwright() as pw:
    br = pw.chromium.launch(); pg = br.new_page(); errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("file://" + sim); pg.wait_for_timeout(500)
    pg.evaluate("cmd('circ')"); pg.wait_for_timeout(400); pg.click("#csim"); pg.wait_for_timeout(300)
    src = '''<log++>
Setup
{
    Pin:14("Input;PullDown")
    Pin:13("Output")
    Var:n("Value;0")
    On:Pin(14)("Rising")
    {
        n += 1
        Print("boton apretado {n}")
        Pin:13("Set;High")
    }
    On:Pin(14)("Falling")
    {
        Print("boton suelto")
        Pin:13("Set;Low")
    }
}
Loop
{
    Wait("ms;50")
}
'''
    pg.evaluate("([s])=>{files['b.log++']=s; window.__p=runFile('b.log++')}", [src]); pg.wait_for_timeout(700)
    # circuit: push button between 3V3 and G14 (the wiring described in Circuit_Programs/button_led.log++), built through the same data the editor uses
    btn = pg.evaluate("""()=>{const S=CIR._.S;S.comps=[];S.wires=[];const id=S.nid++;S.comps.push({id,t:'pb',x:10,y:5,r:0,v:0});
      S.wires.push({id:S.nid++,a:'B:V3_0',b:'C'+id+':0',pts:[]},{id:S.nid++,a:'C'+id+':1',b:'B:G14',pts:[]});CIR._.RT[id]=CIR._.RT[id]||{};return String(id)}""")
    pg.evaluate("window.__fr=setInterval(()=>CIR._.solve(),16)")
    check("the circuit has a push button", btn is not None)
    def press(v): pg.evaluate("([id,v])=>{const r=CIR._.RT[id]||(CIR._.RT[id]={});r.down=v;CIR._.solve()}", [btn, v]); pg.wait_for_timeout(250)
    press(True); press(False); press(True); press(False)
    o = pg.evaluate("[...OL.children].map(d=>d.textContent).join('\\n')")
    check("two presses -> two Rising events and two Falling events from the real circuit", o.count("boton apretado") == 2 and o.count("boton suelto") == 2 and "boton apretado 2" in o)
    check("the handler drove the LED pin (G13 low after release)", pg.evaluate("PIN[13]|0") == 0)
    pg.evaluate("stopRun()")
    if errs: print("JS errors:", errs[:3]); ok = False
    br.close()
print("circuit button test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

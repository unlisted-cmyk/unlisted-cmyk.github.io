#!/usr/bin/env python3
"""Loads every example of the Circuits mode menu ("Examples") through the real UI: the circuit must be built and the program must start without errors.
The two 2.1 examples are also checked for what they promise.   Usage: python3 tests/sim_examples.py index.html"""
import os, sys, time
from playwright.sync_api import sync_playwright
sim = os.path.abspath(sys.argv[1]); ok = True
def check(name, cond):
    global ok; print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
with sync_playwright() as pw:
    br = pw.chromium.launch(); pg = br.new_page(); errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("file://" + sim); pg.wait_for_timeout(500)
    pg.evaluate("cmd('circ')"); pg.wait_for_timeout(400)
    def menu(): pg.click("#cex"); pg.wait_for_timeout(250)
    menu(); names = pg.evaluate("[...document.querySelectorAll('#cmenu .pi span')].map(s=>s.firstChild.textContent.trim())"); pg.click("#cex"); pg.wait_for_timeout(150)
    print("examples in the menu:", len(names))
    check("the menu has the 9 original examples and the 2 new ones", len(names) == 11 and "PWM dimmable LED" in names and any("On:Pin" in n for n in names))
    out = lambda: pg.evaluate("[...OL.children].map(d=>d.textContent).join('\\n')")
    for i, n in enumerate(names):
        pg.evaluate("stopRun()"); pg.wait_for_timeout(200); menu(); pg.click('#cmenu button[data-x="%d"]' % i); pg.wait_for_timeout(1300)
        comps = pg.evaluate("CIR._.S.comps.length"); o = out(); st = pg.evaluate("document.querySelector('#os').textContent")
        check("example '%s': circuit built (%d parts), program running, no error" % (n, comps), comps > 0 and "ERROR" not in o and ("RUNNING" in st or "FINISHED" in st))
        if n == "PWM dimmable LED":
            pg.evaluate("window.__fr=setInterval(()=>CIR._.solve(),16)"); vals = []; t0 = time.time()
            while time.time() - t0 < 3.2:
                vals.append(pg.evaluate("(()=>{const ids=CIR._.S.comps.map(c=>String(c.id));const a=ids.map(i=>CIR._.RT[i]).filter(Boolean).map(o=>o.ia).filter(x=>typeof x==='number');return a.length?Math.max(...a):0})()")); pg.wait_for_timeout(40)
            check("PWM example: the LED brightness rises and falls (min %.1f mA, max %.1f mA)" % (min(vals) * 1000, max(vals) * 1000), max(vals) > 0.003 and min(vals) < 0.5 * max(vals))
            pg.evaluate("clearInterval(window.__fr)")
        if "On:Pin" in n:
            pg.evaluate("window.__fr=setInterval(()=>CIR._.solve(),16)")
            btn = pg.evaluate("(()=>{const c=CIR._.S.comps.find(c=>c.t==='pb');return c?String(c.id):null})()")
            def press(v): pg.evaluate("([id,v])=>{const r=CIR._.RT[id]||(CIR._.RT[id]={});r.down=v;CIR._.solve()}", [btn, v]); pg.wait_for_timeout(300)
            press(True); press(False); press(True)
            o = out(); check("button example: On:Pin counts the presses", "pressed 1 times" in o and "pressed 2 times" in o)
            pg.evaluate("clearInterval(window.__fr)")
    pg.evaluate("stopRun()")
    if errs: print("JS errors:", errs[:3]); ok = False
    br.close()
print("examples test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

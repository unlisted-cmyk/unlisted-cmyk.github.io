#!/usr/bin/env python3
"""Circuits mode: the LED current must follow the PWM duty (average current), in the real circuit solver.   Usage: python3 tests/sim_circuit_pwm.py index.html"""
import os, random, sys, time
from playwright.sync_api import sync_playwright
sim = os.path.abspath(sys.argv[1]); ok = True
with sync_playwright() as pw:
    br = pw.chromium.launch(); pg = br.new_page(); errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("file://" + sim); pg.wait_for_timeout(500)
    pg.evaluate("cmd('circ')"); pg.wait_for_timeout(400)
    pg.get_by_role("button", name="Example", exact=True).click(); pg.wait_for_timeout(300); pg.click("#csim"); pg.wait_for_timeout(500)
    # The page redraws (and solves) every animation frame; a headless page does not draw, so a 16 ms timer plays that role. Reading is passive.
    pg.evaluate("window.__fr = setInterval(()=>CIR._.solve(), 16)")
    def sample():
        return pg.evaluate("(()=>{const v=Object.values(CIR._.RT).map(o=>o.ia).filter(x=>typeof x==='number');return v.length?Math.max(...v):-1})()")
    res = {}
    for d in ("set", 0, 25, 50, 75, 100):
        body = 'Pin:13("Set;High")' if d == "set" else 'Pin:13("PWM;500","Duty;%s")' % d
        src = '<log++>\nSetup\n{\n Pin:13("Output")\n %s\n}\nLoop\n{\n Wait("ms;100")\n}\n' % body
        pg.evaluate("([s])=>{files['p.log++']=s; window.__p=runFile('p.log++')}", [src]); pg.wait_for_timeout(700)
        vals = []; t0 = time.time()
        while time.time() - t0 < 4.0: vals.append(sample()); pg.wait_for_timeout(random.randint(5, 37))   # random reading instants
        burnt = pg.evaluate("Object.values(CIR._.RT).some(o=>o.burnt)")
        pg.evaluate("stopRun()"); pg.wait_for_timeout(400)
        tail = vals[len(vals) // 3:]; res[d] = (sum(tail) / len(tail), burnt)
        print("%-4s -> mean LED current %6.2f mA  burnt=%s" % (d if d == "set" else str(d) + "%", res[d][0] * 1000, burnt))
    full = res["set"][0]
    def chk(name, cond):
        global ok; print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
    chk("the LED conducts with Set;High", full > 0.003)
    chk("duty 0 % = no current", res[0][0] < 0.0005)
    chk("current grows with the duty", res[0][0] < res[25][0] < res[50][0] < res[75][0] < res[100][0])
    chk("duty 100 % equals a steady High (within 10 %)", abs(res[100][0] - full) < 0.1 * full)
    for d in (25, 50, 75): chk("duty %d%% gives %d%% of the full current (within 10 points; measured %.0f%%)" % (d, d, res[d][0] / full * 100), abs(res[d][0] / full - d / 100) < 0.10)
    chk("PWM never burns the LED", not any(b for _, b in res.values()))
    if errs: print("JS errors:", errs[:3]); ok = False
    br.close()
print("circuit PWM test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)

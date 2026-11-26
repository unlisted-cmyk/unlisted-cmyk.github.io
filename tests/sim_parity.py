#!/usr/bin/env python3
"""Runs the portable test programs in the SIMULATOR (headless Chromium) and compares what it prints with what logl printed (the tests/*.out files).
Usage: python3 tests/sim_parity.py path/to/index.html [-v] [name ...]
Needs:  pip install playwright && playwright install chromium"""
import os, re, sys, glob
from playwright.sync_api import sync_playwright
HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("-")]; verbose = "-v" in sys.argv
sim = os.path.abspath(args[0]); only = set(args[1:])

# Programs that use only portable instructions and must print the same in both. (name, reason it is excluded is in EXCLUDED below)
CASES = ["t01_hello", "t02_loop", "t03_augassign", "t04_switch", "t05_pin_virtual", "t07_juego3d", "t08_i2c_virtual", "t09_spi_loopback", "t10_uart_loopback",
         "t11_errors_nested", "t12_pwm_virtual", "t13_adc_virtual", "t14_onpin_virtual", "t15_watchdog", "t16_safe_on_error", "t18_tcp_loopback", "t19_udp_loopback",
         "t20_http_server_client", "t21_onnet_events", "e01_case_outside", "e02_undeclared_aug", "e06_bus_badop", "e11_net_badops"]
EXCLUDED = {  # differences that are by design (see the table in README_2.1.md)
    "t06_disk_inside": "Host:Write needs kernel mode in the simulator", "e03_disk_escape": "no real disk in the simulator", "e03_disk_allowed": "no real disk",
    "e04_pin_virtual": "uses Pin:17 only, same as t05", "e04_pin_denied": "real GPIO / permission flags do not exist in the simulator", "e05_admin_denied": "Admin:Run is not simulated",
    "e05_admin_allowed": "Admin:Run is not simulated", "e07_bus_noperm": "permission flags", "e08_fakegpio_noperm": "permission flags", "e09_net_noperm": "permission flags",
    "e10_http_noperm": "permission flags", "t17_fakegpio_hw": "real-hardware code path (--fake-gpio)", "t18b": ""}

def find_src(name):
    for p in glob.glob(os.path.join(HERE, name + ".*")):
        if not p.endswith(".out") and not p.endswith(".py"): return p
    return None
def norm_logl(txt):
    out = []
    for l in txt.replace("\r", "").split("\n"):
        if l.startswith("Compiled with no errors") or l in ("FINISHED", "STOPPED") or re.match(r"^rc=\d+$", l): continue
        out.append(l)
    while out and out[-1] == "": out.pop()
    return out
def norm_sim(lines):
    out = []
    for l in "\n".join(lines).split("\n"):          # a text with a line break is ONE entry in the simulator, two lines in logl
        if l.startswith("Compiled without errors") or l.startswith("Waiting for events") : continue
        if l.startswith("WARNING l."): continue          # the simulator also warns about unknown options at compile time; logl never does (documented difference)
        if re.match(r"^   \d+ \| ", l) or l.startswith("   -> "): continue          # source line and hint under an error
        out.append(l.rstrip())
    while out and out[-1] == "": out.pop()
    return out

RUN = """async ([name, src, tmo]) => {
  files[name] = src; const p = runFile(name); const t0 = Date.now();
  while (true) {
    await new Promise(r => setTimeout(r, 25));
    const st = document.querySelector('#os').textContent;
    if (/FINISHED|STOPPED|ERROR/.test(st) && !(R && R.on)) break;
    if (Date.now() - t0 > tmo * 1000) { stopRun(); break; }
  }
  await Promise.race([p, new Promise(r => setTimeout(r, 3000))]);
  return { lines: [...OL.children].map(d => d.textContent), status: document.querySelector('#os').textContent };
}"""
ok = bad = 0
with sync_playwright() as pw:
    br = pw.chromium.launch(); pg = br.new_page(); errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("file://" + sim); pg.wait_for_timeout(800)
    pg.evaluate("['graphics3d','uivisuals','storage'].forEach(e => EXT.add(e))")   # pkg install ... (documented difference: logl has them built in)
    for name in CASES:
        if only and name not in only: continue
        src_path = find_src(name); out_path = os.path.join(HERE, name + ".out")
        if not src_path or not os.path.exists(out_path): print("SKIP  %s (no file yet)" % name); continue
        ext = src_path.split(".", 1)[1]
        want = norm_logl(open(out_path, encoding="utf-8").read())
        res = pg.evaluate(RUN, ["t." + ext, open(src_path, encoding="utf-8").read(), 25])
        got = norm_sim(res["lines"])
        if got == want: ok += 1; print("PASS  " + name)
        else:
            bad += 1; print("FAIL  " + name)
            if verbose or len(want) < 40:
                import difflib
                for d in list(difflib.unified_diff(want, got, "logl", "simulator", lineterm="", n=1))[:14]: print("      " + d)
    br.close()
if errs: print("JS errors in the page:", errs[:3])
print("---- parity: %d same, %d different" % (ok, bad)); sys.exit(0 if bad == 0 and not errs else 1)
